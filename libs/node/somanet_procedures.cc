#include "node/somanet_procedures.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <format>
#include <initializer_list>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mm::node {

namespace {

// Bounds on the client-settable timings. The lower bounds keep a request from busy-polling the
// mailbox or timing out before the drive can answer; the upper bound stops a typo parking a
// device's busy claim for hours.
constexpr auto kMinTimeout = std::chrono::milliseconds(1);
constexpr auto kMaxTimeout = std::chrono::minutes(10);
constexpr auto kMinPollInterval = std::chrono::milliseconds(1);
constexpr auto kMaxPollInterval = std::chrono::seconds(1);

// What an encoder register access is sized for: a millisecond-scale exchange between the drive and
// its encoder, so the ceiling is generous and the poll fine. The same figures are the defaults on
// SomanetDrive::readEncoderRegister — spelled out again here because this call also passes a stop
// token, which replaces the whole default config rather than adding to it.
constexpr auto kEncoderRegisterTimeout = std::chrono::seconds(5);
constexpr auto kEncoderRegisterPollInterval = std::chrono::milliseconds(20);

// The inclusive bounds of every byte-valued parameter, named once so the validation and the
// parameter description cannot disagree about them.
constexpr int64_t kMinByte = 0;
constexpr int64_t kMaxByte = 0xFF;

// Reads an optional millisecond field, defaulting to what the request already holds.
std::expected<std::chrono::milliseconds, std::string> readMillis(const nlohmann::json& body,
                                                                 const char* field,
                                                                 std::chrono::milliseconds fallback,
                                                                 std::chrono::milliseconds min,
                                                                 std::chrono::milliseconds max) {
  auto it = body.find(field);
  if (it == body.end() || it->is_null()) {
    return fallback;
  }
  if (!it->is_number_unsigned()) {
    return std::unexpected(
        std::format("'{}' must be a non-negative whole number of milliseconds", field));
  }
  const std::chrono::milliseconds value{it->get<uint64_t>()};
  if (value < min || value > max) {
    return std::unexpected(
        std::format("'{}' must be between {} and {} ms", field, min.count(), max.count()));
  }
  return value;
}

// Reads a byte-valued field. A @p fallback of nullopt makes the field required, which is the one
// case a client cannot recover from by guessing: there is no register address worth defaulting to.
std::expected<uint8_t, std::string> readByte(const nlohmann::json& body, const char* field,
                                             std::optional<uint8_t> fallback) {
  auto it = body.find(field);
  if (it == body.end() || it->is_null()) {
    if (fallback) {
      return *fallback;
    }
    return std::unexpected(std::format("'{}' is required", field));
  }
  // is_number_integer accepts both signed and unsigned: a JSON *document* parses 117 as unsigned,
  // but a body built in C++ from an int literal is signed, and rejecting that would be an accident
  // of how the object was made rather than anything about the value. The range check below is what
  // actually decides, and it rejects a negative either way.
  if (!it->is_number_integer()) {
    return std::unexpected(
        std::format("'{}' must be a byte value ({}-{})", field, kMinByte, kMaxByte));
  }
  const int64_t raw = it->get<int64_t>();
  if (raw < kMinByte || raw > kMaxByte) {
    return std::unexpected(
        std::format("'{}' must be a byte value ({}-{}), got {}", field, kMinByte, kMaxByte, raw));
  }
  return static_cast<uint8_t>(raw);
}

// Reads an optional boolean field, defaulting to what the request already holds.
std::expected<bool, std::string> readBool(const nlohmann::json& body, const char* field,
                                          bool fallback) {
  auto it = body.find(field);
  if (it == body.end() || it->is_null()) {
    return fallback;
  }
  if (!it->is_boolean()) {
    return std::unexpected(std::format("'{}' must be true or false", field));
  }
  return it->get<bool>();
}

// Puts back everything a diagnostics-mode procedure changed, on every path out of the body — an
// early return, a failure, a cancellation — because a procedure that leaves the brake released and
// the drive in diagnostics mode has left the machine in a worse state than it found it.
//
// Inert until told what to restore, so a body that failed before changing anything restores nothing
// and does not claim a restore it never performed. Each restore is armed *before* the change it
// undoes, so a change that fails half-way is still unwound.
//
// It reports through kRestoreStep rather than only logging, because a restore that itself fails is
// precisely what a user needs to see.
class DiagnosticsRestorer {
 public:
  // @param procedure  Names the procedure in the log line a failed restore emits.
  DiagnosticsRestorer(SomanetDrive& drive, ProgressReporter& reporter, std::string_view procedure)
      : drive_(drive), reporter_(reporter), procedure_(procedure) {}

  DiagnosticsRestorer(const DiagnosticsRestorer&) = delete;
  DiagnosticsRestorer& operator=(const DiagnosticsRestorer&) = delete;

  ~DiagnosticsRestorer() {
    if (!mode_ && !brake_) {
      return;
    }
    reporter_.start(kRestoreStep);
    std::string failures;
    const auto note = [&failures](const std::string& what) {
      failures += failures.empty() ? what : "; " + what;
    };
    // Brake first, then the state, then the mode — and each position is load-bearing.
    //
    // The brake has to go back first because it is only the master's to command while the drive is
    // still enabled and in diagnostics mode, so undoing either of those first would strand it.
    //
    // The mode goes back **after** the drive is disabled, never before: the mode being restored is
    // whatever the drive was in before the procedure, which is very often a motion mode, and its
    // setpoint object is not something a procedure ever wrote. Restoring it while the drive is
    // still in Operation Enabled asks the drive to follow that setpoint — in CSP, a target position
    // staged at 0 against a real position somewhere else — for as long as it takes the disable to
    // land. On the default PDO mapping that window is usually nil, since 0x6060 and 0x6040 are both
    // RxPDO-mapped and stage into the same cycle; it opens for a mailbox round-trip as soon as a
    // custom mapping leaves 0x6060 out of the RxPDO and the write falls back to SDO. Disabling
    // first costs nothing (0x6060 is writable in any state) and closes it either way.
    if (brake_) {
      if (auto r = drive_.setBrakeStatus(*brake_); !r) {
        note(std::format("failed to restore the brake: {}", r.error()));
      }
    }
    if (mode_) {
      if (auto r = drive_.disable(); !r) {
        note(std::format("failed to return the drive to Switch On Disabled: {}", r.error()));
      }
      if (auto r = drive_.setOperationModeValue(*mode_); !r) {
        note(std::format("failed to restore operation mode {}: {}", *mode_, r.error()));
      }
    }
    if (failures.empty()) {
      reporter_.succeed(kRestoreStep);
    } else {
      spdlog::error("Device {}: {} restore: {}", drive_.device().slavePosition(), procedure_,
                    failures);
      reporter_.fail(kRestoreStep, failures);
    }
  }

  // Arms the restore of operation mode @p mode — and with it the return to Switch On Disabled,
  // since the only reason a procedure changes the mode is to enable the drive in it.
  void restoreMode(int8_t mode) { mode_ = mode; }

  // Arms the restore of brake status @p status.
  void restoreBrake(somanet::BrakeStatus status) { brake_ = status; }

 private:
  SomanetDrive& drive_;
  ProgressReporter& reporter_;
  std::string_view procedure_;
  std::optional<int8_t> mode_;
  std::optional<somanet::BrakeStatus> brake_;
};

// The preparation every diagnostics-mode OS command needs: SOMANET's diagnostics operation mode and
// CiA402 Operation Enabled. The firmware enforces both by refusing the command with OS error 251
// ("command not allowed") rather than by misbehaving, so this is what stands between a procedure
// and an unexplained refusal.
//
// Reports on kPrepareStep and arms @p restorer with the mode it found, so a drive that will not
// enable still has its mode change unwound.
std::expected<void, std::string> prepareForDiagnostics(SomanetDrive& drive,
                                                       ProgressReporter& reporter,
                                                       DiagnosticsRestorer& restorer) {
  reporter.start(kPrepareStep);

  auto savedMode = drive.operationModeValue();
  if (!savedMode) {
    reporter.fail(kPrepareStep, savedMode.error());
    return std::unexpected(savedMode.error());
  }
  if (auto r = drive.setOperationMode(somanet::OperationMode::kDiagnostics); !r) {
    reporter.fail(kPrepareStep, r.error());
    return std::unexpected(r.error());
  }
  restorer.restoreMode(*savedMode);

  if (auto r = drive.enable(); !r) {
    const auto reason = std::format(
        "the drive did not reach Operation Enabled, which the command requires: {}", r.error());
    reporter.fail(kPrepareStep, reason);
    return std::unexpected(reason);
  }
  reporter.succeed(kPrepareStep);
  return {};
}

// Releases the brake for the commands whose restrictions require it, arming its restore first.
//
// Must be called *after* prepareForDiagnostics, never folded into it: writing the brake state only
// performs a real release while the drive is in OP ENABLED, and in diagnostics mode enabling the
// drive does **not** release the brake the way normal operation does — which is the whole reason
// the master has to do it here.
//
// Reports on kReleaseBrakeStep.
std::expected<void, std::string> releaseBrakeForDiagnostics(SomanetDrive& drive,
                                                            ProgressReporter& reporter,
                                                            DiagnosticsRestorer& restorer) {
  reporter.start(kReleaseBrakeStep);

  auto savedBrake = drive.brakeStatus();
  if (!savedBrake) {
    reporter.fail(kReleaseBrakeStep, savedBrake.error());
    return std::unexpected(savedBrake.error());
  }
  // Armed before the attempt, not after it: a release that writes the brake and then fails reading
  // back has still released it.
  restorer.restoreBrake(*savedBrake);

  auto brake = drive.releaseBrake();
  if (!brake) {
    reporter.fail(kReleaseBrakeStep, brake.error());
    return std::unexpected(brake.error());
  }
  reporter.succeed(kReleaseBrakeStep, *brake);
  return {};
}

// Confirms the drive is still in Operation Enabled, before the next command of a multi-command
// sequence is issued.
//
// Worth one bus read per step rather than letting the command speak for itself. A drive that has
// left Operation Enabled midway — a fault, or the quick stop the firmware documentation says aborts
// offset detection and disables the drive — refuses every command after it with OS error 251,
// "command not allowed". That code names a *precondition*, so it reads as though the mode or the
// brake were wrong and looks identical whatever actually happened; the fault that caused it is
// nowhere in the message. Reading the state first names the real cause, and for a fault attaches
// the drive's own description of it.
//
// Only the composites need this: a single-command procedure enables the drive and issues its
// command immediately, so there is no window for the state to change behind it.
std::expected<void, std::string> confirmStillEnabled(SomanetDrive& drive) {
  auto state = drive.state();
  if (!state) {
    return std::unexpected(
        std::format("could not confirm the drive is still enabled: {}", state.error()));
  }
  if (*state == cia402::State::kOperationEnabled) {
    return {};
  }

  auto reason = std::format(
      "the drive left Operation Enabled and is now in {}, so the command was not issued",
      cia402::toString(*state));
  if (*state == cia402::State::kFault || *state == cia402::State::kFaultReactionActive) {
    // Best-effort: a description is what makes the fault actionable, but failing to read one must
    // not replace the state we do know with a read error.
    if (auto report = drive.errorReport(); report && !report->empty()) {
      reason += std::format(" (drive error report: {})", *report);
    }
  }
  return std::unexpected(reason);
}

// Runs one command of a composite procedure: checks for cancellation, starts the step, confirms the
// drive is still enabled, issues the command, and records what it produced. On failure the step
// carries the reason and the composite stops — every later step depends on the ones before it, so
// carrying on would measure against a value that was never established.
//
// @param run  Issues one command and returns its result, or why there is none.
template <typename Run>
std::expected<void, std::string> compositeStep(ProgressReporter& reporter, SomanetDrive& drive,
                                               std::string_view step, std::string_view what,
                                               const std::stop_token& stop, Run run) {
  if (stop.stop_requested()) {
    return std::unexpected(std::format("{} was cancelled", what));
  }
  reporter.start(step);
  // Reported against the step that could not run, not the one that broke the drive: that is the
  // step a user is waiting on, and the reason says what the drive is doing instead.
  if (auto ready = confirmStillEnabled(drive); !ready) {
    reporter.fail(step, ready.error());
    return std::unexpected(ready.error());
  }
  auto result = run();
  if (!result) {
    reporter.fail(step, result.error());
    return std::unexpected(result.error());
  }
  reporter.succeed(step, *result);
  return {};
}

// What differs between the diagnostics-mode measurement procedures. Everything else — the prepare,
// the optional brake release, the measurement, the restore, the cancellation checks — is identical,
// which is why they share one body.
struct MeasurementProcedure {
  std::string_view procedure;  // Procedure name, for the restorer's log line.
  std::string_view step;       // Step id the measurement reports against.
  std::string_view what;       // How to name the measurement in a cancellation message.

  // Whether this command's restrictions require a disengaged brake. **Per command, taken from the
  // firmware specification, and not a matter of symmetry**: pole pair (7) and motor phase order (4)
  // require it; open phase (6), phase resistance (8) and phase inductance (9) do not, and for them
  // an engaged brake merely keeps the shaft still while the command runs. Releasing a brake a
  // command does not need would drop whatever it was holding for nothing. When false the brake is
  // never written, so there is nothing about it to restore either.
  bool releaseBrake = false;

  std::chrono::milliseconds timeout{30000};  // Ceiling on the command itself.
};

// The shared body of the diagnostics-mode measurement procedures: prepare, optionally release the
// brake, measure, and restore on the way out however it goes.
//
// @param measure  Issues the command on a prepared drive.
template <typename Measure>
std::expected<void, std::string> runMeasurementProcedure(Device& device, ProgressReporter& reporter,
                                                         std::stop_token stop,
                                                         const MeasurementProcedure& spec,
                                                         Measure measure) {
  auto drive = createSomanetDrive(device);
  if (!drive) {
    return std::unexpected(drive.error());
  }

  DiagnosticsRestorer restorer(*drive, reporter, spec.procedure);

  if (auto r = prepareForDiagnostics(*drive, reporter, restorer); !r) {
    return std::unexpected(r.error());
  }
  if (stop.stop_requested()) {
    return std::unexpected(std::format("{} was cancelled", spec.what));
  }

  if (spec.releaseBrake) {
    if (auto r = releaseBrakeForDiagnostics(*drive, reporter, restorer); !r) {
      return std::unexpected(r.error());
    }
    if (stop.stop_requested()) {
      return std::unexpected(std::format("{} was cancelled", spec.what));
    }
  }

  reporter.start(spec.step);
  auto result = measure(*drive, OsCommandConfig{.timeout = spec.timeout,
                                                .pollInterval = std::chrono::milliseconds(100),
                                                .stop = std::move(stop)});
  if (!result) {
    reporter.fail(spec.step, result.error());
    return std::unexpected(result.error());
  }
  reporter.succeed(spec.step, *result);
  return {};
}

}  // namespace

std::expected<OsCommandRequest, std::string> parseOsCommandRequest(const nlohmann::json& body) {
  if (!body.is_object()) {
    return std::unexpected("the request body must be a JSON object");
  }
  auto command = body.find("command");
  if (command == body.end() || !command->is_array()) {
    return std::unexpected(
        std::format("'command' must be an array of {} byte values", kOsCommandSize));
  }
  if (command->size() != kOsCommandSize) {
    return std::unexpected(std::format("'command' must hold exactly {} bytes, got {}",
                                       kOsCommandSize, command->size()));
  }

  OsCommandRequest request;
  request.command.reserve(kOsCommandSize);
  for (const auto& byte : *command) {
    if (!byte.is_number_unsigned() || byte.get<uint64_t>() > 0xFF) {
      return std::unexpected("every entry of 'command' must be a byte value (0-255)");
    }
    request.command.push_back(static_cast<uint8_t>(byte.get<uint64_t>()));
  }

  auto timeout = readMillis(body, "timeoutMs", request.timeout, kMinTimeout, kMaxTimeout);
  if (!timeout) {
    return std::unexpected(timeout.error());
  }
  request.timeout = *timeout;

  auto pollInterval =
      readMillis(body, "pollIntervalMs", request.pollInterval, kMinPollInterval, kMaxPollInterval);
  if (!pollInterval) {
    return std::unexpected(pollInterval.error());
  }
  request.pollInterval = *pollInterval;
  return request;
}

std::vector<ProcedureParameter> osCommandParameters() {
  const OsCommandRequest defaults;
  return {
      byteArrayParameter("command", "Command bytes",
                         "The request written to 0x1023:01. Byte 0 is the OS command ID; bytes 1-7 "
                         "are that command's parameters.",
                         kOsCommandSize),
      integerParameter("timeoutMs", "Timeout (ms)",
                       "Ceiling on the whole command. Reaching it aborts the command on the drive, "
                       "so size it for the command being run — milliseconds for a register read, "
                       "tens of seconds for a measurement.",
                       defaults.timeout.count(), kMinTimeout.count(),
                       std::chrono::duration_cast<std::chrono::milliseconds>(kMaxTimeout).count()),
      integerParameter(
          "pollIntervalMs", "Poll interval (ms)",
          "How long to wait between reads of the drive's response object while the command runs.",
          defaults.pollInterval.count(), kMinPollInterval.count(),
          std::chrono::duration_cast<std::chrono::milliseconds>(kMaxPollInterval).count()),
  };
}

void to_json(nlohmann::json& j, const OsCommandResult& result) {
  j = nlohmann::json{{"status", result.status}, {"data", result.data}};
  if (result.errorCode) {
    j["errorCode"] = *result.errorCode;
  }
}

std::vector<ProgressStep> osCommandSteps() { return stepsFrom({kOsCommandStep}); }

std::expected<void, std::string> runOsCommandProcedure(Device& device, ProgressReporter& reporter,
                                                       std::stop_token stop,
                                                       const OsCommandRequest& request) {
  auto drive = createSomanetDrive(device);
  if (!drive) {
    // Before the step starts: this is not the command failing, it is the device turning out not to
    // be one that has OS commands, so nothing is reported against the step.
    return std::unexpected(drive.error());
  }

  reporter.start(kOsCommandStep);
  auto response = drive->runOsCommand(
      request.command,
      {.timeout = request.timeout, .pollInterval = request.pollInterval, .stop = std::move(stop)});
  if (!response) {
    reporter.fail(kOsCommandStep, response.error());
    return std::unexpected(response.error());
  }

  if (response->failed()) {
    // The drive answered with a refusal. Name the general codes; a command-specific one (counting
    // up from 0) can only be named by a typed procedure that knows which command it issued, so the
    // raw path reports the number and lets the caller look it up.
    std::string reason;
    if (response->errorCode) {
      auto name = osCommandErrorName(*response->errorCode);
      reason = name ? std::format("OS error {} ({})", *response->errorCode, *name)
                    : std::format("OS error {} (command-specific)", *response->errorCode);
    } else {
      reason = "the drive reported an error with no code";
    }
    reporter.fail(kOsCommandStep, reason);
    return std::unexpected(reason);
  }

  reporter.succeed(kOsCommandStep, OsCommandResult{.status = static_cast<uint8_t>(response->status),
                                                   .data = response->data,
                                                   .errorCode = response->errorCode});
  return {};
}

std::expected<EncoderRegisterRequest, std::string> parseEncoderRegisterRequest(
    const nlohmann::json& body) {
  if (!body.is_object()) {
    return std::unexpected("the request body must be a JSON object");
  }
  EncoderRegisterRequest request;

  auto encoder = readByte(body, "encoder", static_cast<uint8_t>(request.encoder));
  if (!encoder) {
    return std::unexpected(encoder.error());
  }
  // Only the two configured slots exist. A third ordinal is rejected rather than passed through:
  // the drive would refuse it anyway, and refusing here says which values are real.
  if (*encoder != static_cast<uint8_t>(somanet::EncoderOrdinal::kEncoder1) &&
      *encoder != static_cast<uint8_t>(somanet::EncoderOrdinal::kEncoder2)) {
    return std::unexpected(std::format("'encoder' must be 1 or 2, got {}", *encoder));
  }
  request.encoder = static_cast<somanet::EncoderOrdinal>(*encoder);

  auto write = readBool(body, "write", request.write);
  if (!write) {
    return std::unexpected(write.error());
  }
  request.write = *write;

  auto registerAddress = readByte(body, "registerAddress", std::nullopt);
  if (!registerAddress) {
    return std::unexpected(registerAddress.error());
  }
  request.registerAddress = *registerAddress;

  auto value = readByte(body, "value", request.value);
  if (!value) {
    return std::unexpected(value.error());
  }
  request.value = *value;
  return request;
}

std::vector<ProcedureParameter> encoderRegisterParameters() {
  const EncoderRegisterRequest defaults;
  return {
      enumParameter(
          "encoder", "Encoder",
          "Which of the drive's encoders to address. Encoder 1 is whatever 0x2110 configures and "
          "encoder 2 whatever 0x2112 does, so the ordinal picks a configured slot rather than a "
          "kind of encoder.",
          static_cast<uint8_t>(defaults.encoder),
          {
              ParameterOption{.value = static_cast<uint8_t>(somanet::EncoderOrdinal::kEncoder1),
                              .title = "Encoder 1"},
              ParameterOption{.value = static_cast<uint8_t>(somanet::EncoderOrdinal::kEncoder2),
                              .title = "Encoder 2"},
          }),
      booleanParameter("write", "Write",
                       "Off reads the register; on writes the value into it. Either way the drive "
                       "reports what the register holds afterwards, so a write confirms itself.",
                       defaults.write),
      integerParameter("registerAddress", "Register address",
                       "The register to access. Required — there is no register worth defaulting "
                       "to, and the map belongs to the encoder chip rather than to this firmware.",
                       nullptr, kMinByte, kMaxByte),
      integerParameter("value", "Value",
                       "The byte to write into the register. Ignored when "
                       "reading.",
                       defaults.value, kMinByte, kMaxByte),
  };
}

std::vector<ProgressStep> encoderRegisterSteps() { return stepsFrom({kEncoderRegisterStep}); }

std::expected<void, std::string> runEncoderRegisterProcedure(
    Device& device, ProgressReporter& reporter, std::stop_token stop,
    const EncoderRegisterRequest& request) {
  auto drive = createSomanetDrive(device);
  if (!drive) {
    // Before the step starts: the device turning out not to be a SOMANET drive is not the access
    // failing, so nothing is reported against the step.
    return std::unexpected(drive.error());
  }

  reporter.start(kEncoderRegisterStep);
  const OsCommandConfig config{.timeout = kEncoderRegisterTimeout,
                               .pollInterval = kEncoderRegisterPollInterval,
                               .stop = std::move(stop)};
  auto result = request.write
                    ? drive->writeEncoderRegister(request.encoder, request.registerAddress,
                                                  request.value, config)
                    : drive->readEncoderRegister(request.encoder, request.registerAddress, config);
  if (!result) {
    reporter.fail(kEncoderRegisterStep, result.error());
    return std::unexpected(result.error());
  }
  reporter.succeed(kEncoderRegisterStep, *result);
  return {};
}

std::vector<ProgressStep> openPhaseDetectionSteps() {
  return stepsFrom({kPrepareStep, kOpenPhaseDetectionStep, kRestoreStep});
}

std::expected<void, std::string> runOpenPhaseDetectionProcedure(Device& device,
                                                                ProgressReporter& reporter,
                                                                std::stop_token stop) {
  auto drive = createSomanetDrive(device);
  if (!drive) {
    return std::unexpected(drive.error());
  }

  // Inert until the preparation arms it, and armed change by change from there.
  DiagnosticsRestorer restorer(*drive, reporter, kOpenPhaseDetectionProcedure);

  if (auto r = prepareForDiagnostics(*drive, reporter, restorer); !r) {
    return std::unexpected(r.error());
  }
  if (stop.stop_requested()) {
    return std::unexpected("open phase detection was cancelled");
  }

  // No brake step, deliberately. This command's restrictions do not include a disengaged brake, and
  // the specification says only that it "might rotate the motor if there is no brake, or if it's
  // disengaged" — so an engaged brake does not stop the check, it keeps the shaft still while it
  // runs. Releasing it would drop whatever it holds and gain nothing.
  reporter.start(kOpenPhaseDetectionStep);
  auto result = drive->runOpenPhaseDetection({.timeout = std::chrono::seconds(10),
                                              .pollInterval = std::chrono::milliseconds(100),
                                              .stop = std::move(stop)});
  if (!result) {
    reporter.fail(kOpenPhaseDetectionStep, result.error());
    return std::unexpected(result.error());
  }
  if (result->phaseOpened) {
    // The check ran and found a fault. That fails the step: a green step next to an unconnected
    // motor terminal would be a worse lie than any loss of nuance here, and the run's overall
    // status is what tells a user this drive has a problem.
    const auto reason = result->describe();
    reporter.fail(kOpenPhaseDetectionStep, reason);
    return std::unexpected(reason);
  }
  reporter.succeed(kOpenPhaseDetectionStep, *result);
  return {};
}

std::vector<ProgressStep> polePairDetectionSteps() {
  return stepsFrom({kPrepareStep, kReleaseBrakeStep, kPolePairDetectionStep, kRestoreStep});
}

std::expected<void, std::string> runPolePairDetectionProcedure(Device& device,
                                                               ProgressReporter& reporter,
                                                               std::stop_token stop) {
  // The one measurement here that *does* release the brake, because its restrictions say so — and
  // the only one whose command is documented as needing to turn the rotor rather than merely being
  // able to, which is also why it gets a longer ceiling.
  return runMeasurementProcedure(device, reporter, std::move(stop),
                                 {.procedure = kPolePairDetectionProcedure,
                                  .step = kPolePairDetectionStep,
                                  .what = "pole pair detection",
                                  .releaseBrake = true,
                                  .timeout = std::chrono::seconds(60)},
                                 [](SomanetDrive& drive, const OsCommandConfig& config) {
                                   return drive.runPolePairDetection(config);
                                 });
}

std::vector<ProgressStep> motorPhaseOrderDetectionSteps() {
  return stepsFrom({kPrepareStep, kReleaseBrakeStep, kMotorPhaseOrderDetectionStep, kRestoreStep});
}

std::expected<void, std::string> runMotorPhaseOrderDetectionProcedure(Device& device,
                                                                      ProgressReporter& reporter,
                                                                      std::stop_token stop) {
  // Releases the brake and turns the rotor, both because the command requires it. Note what the
  // restore does *not* undo: the phase order the firmware wrote into 0x2003:05 is the result, not a
  // side effect, so only the mode and the brake go back.
  return runMeasurementProcedure(device, reporter, std::move(stop),
                                 {.procedure = kMotorPhaseOrderDetectionProcedure,
                                  .step = kMotorPhaseOrderDetectionStep,
                                  .what = "motor phase order detection",
                                  .releaseBrake = true,
                                  .timeout = std::chrono::seconds(60)},
                                 [](SomanetDrive& drive, const OsCommandConfig& config) {
                                   return drive.runMotorPhaseOrderDetection(config);
                                 });
}

std::vector<ProgressStep> offsetDetectionSteps() {
  return stepsFrom({kPrepareStep, kOpenPhaseDetectionStep, kPhaseResistanceMeasurementStep,
                    kPhaseInductanceMeasurementStep, kReleaseBrakeStep, kPolePairDetectionStep,
                    kMotorPhaseOrderDetectionStep, kSetBrakeStep, kCommutationOffsetMeasurementStep,
                    kRestoreStep});
}

std::expected<void, std::string> runOffsetDetectionProcedure(Device& device,
                                                             ProgressReporter& reporter,
                                                             std::stop_token stop) {
  static constexpr std::string_view kWhat = "offset detection";

  auto drive = createSomanetDrive(device);
  if (!drive) {
    return std::unexpected(drive.error());
  }

  // Read before anything is touched, for the same reason as in commutation offset detection: the
  // method decides which way the brake goes at the end, and a run that cannot tell must not start.
  auto method = drive->commutationOffsetMethod();
  if (!method) {
    return std::unexpected(method.error());
  }

  DiagnosticsRestorer restorer(*drive, reporter, kOffsetDetectionProcedure);

  if (auto r = prepareForDiagnostics(*drive, reporter, restorer); !r) {
    return std::unexpected(r.error());
  }

  // Open phase detection first, because every measurement after it assumes the three phases are
  // actually connected — each would otherwise fail in a way that points at the wrong thing.
  if (stop.stop_requested()) {
    return std::unexpected(std::format("{} was cancelled", kWhat));
  }
  reporter.start(kOpenPhaseDetectionStep);
  if (auto ready = confirmStillEnabled(*drive); !ready) {
    reporter.fail(kOpenPhaseDetectionStep, ready.error());
    return std::unexpected(ready.error());
  }
  auto openPhase = drive->runOpenPhaseDetection({.timeout = std::chrono::seconds(10),
                                                 .pollInterval = std::chrono::milliseconds(100),
                                                 .stop = stop});
  if (!openPhase) {
    reporter.fail(kOpenPhaseDetectionStep, openPhase.error());
    return std::unexpected(openPhase.error());
  }
  if (openPhase->phaseOpened) {
    const auto reason = openPhase->describe();
    reporter.fail(kOpenPhaseDetectionStep, reason);
    return std::unexpected(reason);
  }
  reporter.succeed(kOpenPhaseDetectionStep, *openPhase);

  // The two winding measurements, which need no brake handling — so they run while the brake is
  // still where it was found, and the load stays held for as long as possible.
  if (auto r = compositeStep(reporter, *drive, kPhaseResistanceMeasurementStep, kWhat, stop,
                             [&drive, &stop] {
                               return drive->runPhaseResistanceMeasurement(
                                   {.timeout = std::chrono::seconds(30),
                                    .pollInterval = std::chrono::milliseconds(100),
                                    .stop = stop});
                             });
      !r) {
    return std::unexpected(r.error());
  }
  if (auto r = compositeStep(reporter, *drive, kPhaseInductanceMeasurementStep, kWhat, stop,
                             [&drive, &stop] {
                               return drive->runPhaseInductanceMeasurement(
                                   {.timeout = std::chrono::seconds(30),
                                    .pollInterval = std::chrono::milliseconds(100),
                                    .stop = stop});
                             });
      !r) {
    return std::unexpected(r.error());
  }

  // Released once, here — as late as the sequence allows. Pole pair and motor phase order detection
  // both require a disengaged brake, and everything before this point did not.
  if (auto r = releaseBrakeForDiagnostics(*drive, reporter, restorer); !r) {
    return std::unexpected(r.error());
  }

  if (auto r = compositeStep(reporter, *drive, kPolePairDetectionStep, kWhat, stop,
                             [&drive, &stop] {
                               return drive->runPolePairDetection(
                                   {.timeout = std::chrono::seconds(60),
                                    .pollInterval = std::chrono::milliseconds(100),
                                    .stop = stop});
                             });
      !r) {
    return std::unexpected(r.error());
  }
  if (auto r = compositeStep(reporter, *drive, kMotorPhaseOrderDetectionStep, kWhat, stop,
                             [&drive, &stop] {
                               return drive->runMotorPhaseOrderDetection(
                                   {.timeout = std::chrono::seconds(60),
                                    .pollInterval = std::chrono::milliseconds(100),
                                    .stop = stop});
                             });
      !r) {
    return std::unexpected(r.error());
  }

  // The brake goes where the offset method needs it: left released for the rotating methods,
  // engaged for the stationary one. The restore is not re-armed — it holds the status found before
  // any of this, which is the only status worth putting back.
  if (stop.stop_requested()) {
    return std::unexpected(std::format("{} was cancelled", kWhat));
  }
  reporter.start(kSetBrakeStep);
  auto brake =
      somanet::requiresBrakeReleased(*method) ? drive->releaseBrake() : drive->engageBrake();
  if (!brake) {
    reporter.fail(kSetBrakeStep, brake.error());
    return std::unexpected(brake.error());
  }
  reporter.succeed(kSetBrakeStep, *brake);

  return compositeStep(reporter, *drive, kCommutationOffsetMeasurementStep, kWhat, stop,
                       [&drive, &method, &stop] {
                         return drive->runCommutationOffsetMeasurement(
                             *method, {.timeout = std::chrono::seconds(60),
                                       .pollInterval = std::chrono::milliseconds(100),
                                       .stop = stop});
                       });
}

std::vector<ProgressStep> commutationOffsetDetectionSteps() {
  return stepsFrom({kPrepareStep, kReleaseBrakeStep, kMotorPhaseOrderDetectionStep, kSetBrakeStep,
                    kCommutationOffsetMeasurementStep, kRestoreStep});
}

std::expected<void, std::string> runCommutationOffsetDetectionProcedure(Device& device,
                                                                        ProgressReporter& reporter,
                                                                        std::stop_token stop) {
  auto drive = createSomanetDrive(device);
  if (!drive) {
    return std::unexpected(drive.error());
  }

  // Read before anything is changed, and reported as a run-level failure rather than against a
  // step: the method decides which way the brake has to go, so not knowing it means there is no
  // safe way to proceed — and refusing here leaves the drive untouched instead of enabled in
  // diagnostics mode.
  auto method = drive->commutationOffsetMethod();
  if (!method) {
    return std::unexpected(method.error());
  }

  DiagnosticsRestorer restorer(*drive, reporter, kCommutationOffsetDetectionProcedure);

  if (auto r = prepareForDiagnostics(*drive, reporter, restorer); !r) {
    return std::unexpected(r.error());
  }
  if (stop.stop_requested()) {
    return std::unexpected("commutation offset detection was cancelled");
  }

  // Motor phase order detection first, and the brake released for it: command 4 requires a
  // disengaged brake unconditionally, whatever the offset method turns out to need afterwards.
  if (auto r = releaseBrakeForDiagnostics(*drive, reporter, restorer); !r) {
    return std::unexpected(r.error());
  }
  if (stop.stop_requested()) {
    return std::unexpected("commutation offset detection was cancelled");
  }

  reporter.start(kMotorPhaseOrderDetectionStep);
  if (auto ready = confirmStillEnabled(*drive); !ready) {
    reporter.fail(kMotorPhaseOrderDetectionStep, ready.error());
    return std::unexpected(ready.error());
  }
  auto phaseOrder =
      drive->runMotorPhaseOrderDetection({.timeout = std::chrono::seconds(60),
                                          .pollInterval = std::chrono::milliseconds(100),
                                          .stop = stop});
  if (!phaseOrder) {
    reporter.fail(kMotorPhaseOrderDetectionStep, phaseOrder.error());
    return std::unexpected(phaseOrder.error());
  }
  reporter.succeed(kMotorPhaseOrderDetectionStep, *phaseOrder);

  if (stop.stop_requested()) {
    return std::unexpected("commutation offset detection was cancelled");
  }

  // Now put the brake where the *offset method* needs it: still released for the rotating methods,
  // but engaged for the stationary one, which cannot hold the load. Not a shortcut for "leave it
  // alone" — the brake was released for command 4 a moment ago, so method 2 has to undo that.
  //
  // The restore is deliberately *not* re-armed here: it already holds the status the brake had
  // before any of this, and re-arming would overwrite that with the released state and put the
  // brake back wrong.
  reporter.start(kSetBrakeStep);
  auto brake =
      somanet::requiresBrakeReleased(*method) ? drive->releaseBrake() : drive->engageBrake();
  if (!brake) {
    reporter.fail(kSetBrakeStep, brake.error());
    return std::unexpected(brake.error());
  }
  reporter.succeed(kSetBrakeStep, *brake);

  if (stop.stop_requested()) {
    return std::unexpected("commutation offset detection was cancelled");
  }

  reporter.start(kCommutationOffsetMeasurementStep);
  if (auto ready = confirmStillEnabled(*drive); !ready) {
    reporter.fail(kCommutationOffsetMeasurementStep, ready.error());
    return std::unexpected(ready.error());
  }
  auto result = drive->runCommutationOffsetMeasurement(
      *method, {.timeout = std::chrono::seconds(60),
                .pollInterval = std::chrono::milliseconds(100),
                .stop = std::move(stop)});
  if (!result) {
    reporter.fail(kCommutationOffsetMeasurementStep, result.error());
    return std::unexpected(result.error());
  }
  reporter.succeed(kCommutationOffsetMeasurementStep, *result);
  return {};
}

std::vector<ProgressStep> phaseResistanceMeasurementSteps() {
  return stepsFrom({kPrepareStep, kPhaseResistanceMeasurementStep, kRestoreStep});
}

std::expected<void, std::string> runPhaseResistanceMeasurementProcedure(Device& device,
                                                                        ProgressReporter& reporter,
                                                                        std::stop_token stop) {
  return runMeasurementProcedure(device, reporter, std::move(stop),
                                 {.procedure = kPhaseResistanceMeasurementProcedure,
                                  .step = kPhaseResistanceMeasurementStep,
                                  .what = "phase resistance measurement"},
                                 [](SomanetDrive& drive, const OsCommandConfig& config) {
                                   return drive.runPhaseResistanceMeasurement(config);
                                 });
}

std::vector<ProgressStep> phaseInductanceMeasurementSteps() {
  return stepsFrom({kPrepareStep, kPhaseInductanceMeasurementStep, kRestoreStep});
}

std::expected<void, std::string> runPhaseInductanceMeasurementProcedure(Device& device,
                                                                        ProgressReporter& reporter,
                                                                        std::stop_token stop) {
  return runMeasurementProcedure(device, reporter, std::move(stop),
                                 {.procedure = kPhaseInductanceMeasurementProcedure,
                                  .step = kPhaseInductanceMeasurementStep,
                                  .what = "phase inductance measurement"},
                                 [](SomanetDrive& drive, const OsCommandConfig& config) {
                                   return drive.runPhaseInductanceMeasurement(config);
                                 });
}

}  // namespace mm::node
