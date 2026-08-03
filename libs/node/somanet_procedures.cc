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

// A step template from ids in order, all idle.
std::vector<ProgressStep> stepsFrom(std::initializer_list<std::string_view> ids) {
  std::vector<ProgressStep> steps;
  steps.reserve(ids.size());
  for (auto id : ids) {
    ProgressStep step;
    step.id = std::string(id);
    steps.push_back(std::move(step));
  }
  return steps;
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
    // Brake first, then the mode, then the state: the brake is only the master's to command while
    // the drive is still enabled and in diagnostics mode, so putting it back has to happen before
    // either of those is undone.
    if (brake_) {
      if (auto r = drive_.setBrakeStatus(*brake_); !r) {
        note(std::format("failed to restore the brake: {}", r.error()));
      }
    }
    if (mode_) {
      if (auto r = drive_.setOperationModeValue(*mode_); !r) {
        note(std::format("failed to restore operation mode {}: {}", *mode_, r.error()));
      }
      if (auto r = drive_.disable(); !r) {
        note(std::format("failed to return the drive to Switch On Disabled: {}", r.error()));
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

std::vector<ProgressStep> commutationOffsetMeasurementSteps() {
  return stepsFrom({kPrepareStep, kSetBrakeStep, kCommutationOffsetMeasurementStep, kRestoreStep});
}

std::expected<void, std::string> runCommutationOffsetMeasurementProcedure(
    Device& device, ProgressReporter& reporter, std::stop_token stop) {
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

  DiagnosticsRestorer restorer(*drive, reporter, kCommutationOffsetMeasurementProcedure);

  if (auto r = prepareForDiagnostics(*drive, reporter, restorer); !r) {
    return std::unexpected(r.error());
  }
  if (stop.stop_requested()) {
    return std::unexpected("commutation offset measurement was cancelled");
  }

  // Released for the rotating methods, engaged for the stationary one — which is not a shortcut for
  // "leave it alone": a brake found released would let a load move under a method that cannot hold
  // it, so it is commanded either way. Armed before the write, like every other brake change here.
  reporter.start(kSetBrakeStep);
  auto savedBrake = drive->brakeStatus();
  if (!savedBrake) {
    reporter.fail(kSetBrakeStep, savedBrake.error());
    return std::unexpected(savedBrake.error());
  }
  restorer.restoreBrake(*savedBrake);

  auto brake =
      somanet::requiresBrakeReleased(*method) ? drive->releaseBrake() : drive->engageBrake();
  if (!brake) {
    reporter.fail(kSetBrakeStep, brake.error());
    return std::unexpected(brake.error());
  }
  reporter.succeed(kSetBrakeStep, *brake);

  if (stop.stop_requested()) {
    return std::unexpected("commutation offset measurement was cancelled");
  }

  reporter.start(kCommutationOffsetMeasurementStep);
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
