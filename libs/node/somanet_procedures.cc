#include "node/somanet_procedures.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "comm/al_status_codes.h"
#include "comm/foe_error.h"
#include "comm/sii.h"
#include "core/base64.h"
#include "core/platform.h"
#include "core/user_cache.h"

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

// Reading a counter takes one control cycle — a successful run finishes in about 20 ms — so this
// ceiling is not about how long the work takes. It clears the drive's own ~20 s reception timeout
// (measured at 20.04 s on an Integro), so a firmware that does not run the addressed service is
// allowed to answer for itself: OS error 253, which this decodes as "no such service running".
// Anything shorter aborts first and reports that this master gave up, which is true and useless.
constexpr auto kSkippedCyclesTimeout = std::chrono::seconds(30);

// Same reasoning for the BiSS service commands whose precondition is "this encoder exists and is
// BiSS": only the service instance owning the addressed encoder answers, so an encoder that is
// neither is answered by nothing and the drive's own ~20 s timeout is the report. Clear it, and a
// 253 can be decoded into what actually went wrong.
constexpr auto kBissServiceTimeout = std::chrono::seconds(30);

// How long to wait before concluding a service that was told to stop has stopped. Short on purpose:
// for the four types that stop a service, no answer is the intended outcome, so this is the cost of
// establishing it rather than a ceiling on real work. The types that do answer answer at once.
constexpr auto kTriggerErrorTimeout = std::chrono::seconds(3);

// What HRD streaming is sized for. Configuring is dominated by deleting the previous recording's
// files, which the firmware specification allows around 5 s for in the worst case. Recording
// occupies the whole requested duration, so its ceiling is that duration plus this margin for the
// poll cadence and the drive's wrap-up. The poll is coarse on purpose: a command that runs for
// seconds polled every 20 ms would be hundreds of mailbox reads answering "still going".
constexpr auto kHrdConfigureTimeout = std::chrono::seconds(10);
constexpr auto kHrdRecordMargin = std::chrono::seconds(5);
constexpr auto kHrdPollInterval = std::chrono::milliseconds(100);

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
  // is_number_integer accepts signed as well as unsigned, for the reason readByte gives below: a
  // body built in C++ from an int literal is signed, and rejecting that would be an accident of how
  // the object was made rather than anything about the value. The range check rejects a negative.
  if (!it->is_number_integer()) {
    return std::unexpected(
        std::format("'{}' must be a non-negative whole number of milliseconds", field));
  }
  const std::chrono::milliseconds value{it->get<int64_t>()};
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

// Reads the "encoder" field, which both encoder commands take and describe identically — the
// ordinal picks one of the drive's two configured slots and nothing else.
std::expected<somanet::EncoderOrdinal, std::string> readEncoderOrdinal(
    const nlohmann::json& body, somanet::EncoderOrdinal fallback) {
  auto ordinal = readByte(body, "encoder", static_cast<uint8_t>(fallback));
  if (!ordinal) {
    return std::unexpected(ordinal.error());
  }
  // Only the two configured slots exist. A third ordinal is rejected rather than passed through:
  // the drive would refuse it anyway, and refusing here says which values are real.
  if (*ordinal != static_cast<uint8_t>(somanet::EncoderOrdinal::kEncoder1) &&
      *ordinal != static_cast<uint8_t>(somanet::EncoderOrdinal::kEncoder2)) {
    return std::unexpected(std::format("'encoder' must be 1 or 2, got {}", *ordinal));
  }
  return static_cast<somanet::EncoderOrdinal>(*ordinal);
}

// The "encoder" parameter as every encoder command advertises it — one description, so two
// procedures cannot end up explaining the same field differently.
ProcedureParameter encoderParameter(somanet::EncoderOrdinal defaultValue) {
  return enumParameter(
      "encoder", "Encoder",
      "Which of the drive's encoders to address. Encoder 1 is whatever 0x2110 configures and "
      "encoder 2 whatever 0x2112 does, so the ordinal picks a configured slot rather than a kind "
      "of encoder.",
      static_cast<uint8_t>(defaultValue),
      {
          ParameterOption{.value = static_cast<uint8_t>(somanet::EncoderOrdinal::kEncoder1),
                          .title = "Encoder 1"},
          ParameterOption{.value = static_cast<uint8_t>(somanet::EncoderOrdinal::kEncoder2),
                          .title = "Encoder 2"},
      });
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

  // A destructor is implicitly noexcept, so this is a statement about the standard library rather
  // than about this code: the restore path builds strings and can therefore raise bad_alloc. There
  // is no better answer inside a destructor whose whole job is to put the drive back — swallowing
  // it would hide a failed restore.
  // NOLINTNEXTLINE(bugprone-exception-escape)
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

// ── Firmware installation helpers ──────────────────────────────────────────────────────────────

using mm::comm::EtherCatState;
using mm::comm::FoeError;
using mm::comm::Retry;

/// The result of borrowing a device to perform one FoE transfer: the **outer** error is the borrow
/// itself (a string, which is what @c withDevice requires so it can report "device not found"), and
/// the **inner** one is the transfer. Nesting them keeps the FoE error kind intact across the
/// borrow, which is the whole reason this procedure can tell a transient failure worth retrying
/// from a permanent one worth aborting on.
template <typename T>
using Borrowed = std::expected<std::expected<T, FoeError>, std::string>;

constexpr std::string_view kPackageStep = "package";
constexpr std::string_view kCacheStep = "cache";
constexpr std::string_view kBootStep = "boot";
constexpr std::string_view kExtraFilesStep = "extra-files";
constexpr std::string_view kSiiStep = "sii";
constexpr std::string_view kAppFirmwareStep = "app-firmware";
constexpr std::string_view kComFirmwareStep = "com-firmware";
constexpr std::string_view kFinalStateStep = "final-state";

/// The names the bootloader is written under, regardless of what the package calls them.
///
/// Not a simplification: the bootloader has a fixed idea of the firmware filenames it accepts, and
/// a package's real name ("app_8500-04_motion-drive_v5.6.10_2332.bin") is both long and variable.
/// Sending the short constant name avoids filename-length handling in the bootloader entirely.
constexpr std::string_view kAppFirmwareFoeName = "app_firmware.bin";
constexpr std::string_view kComFirmwareFoeName = "com_firmware.bin";

/// FoE read of this pseudo-file, with the target appended, deletes a file from the drive's flash.
constexpr std::string_view kRemoveFilePrefix = "fs-remove=";

/// Ceiling on an ordinary AL transition, which is a mailbox round trip and a slave acknowledgement.
constexpr auto kStateTimeout = std::chrono::seconds(10);

/// Ceiling on the transition out of BOOT. Far longer than an ordinary one because it is not an
/// ordinary one: the SOMANET bootloader hands over to the application by **restarting the device**,
/// so this has to cover a reboot, plus the drive's own internal timeout for deciding no valid
/// application started (after which it answers AL status 0x0014 rather than nothing).
constexpr auto kBootExitTimeout = std::chrono::seconds(30);

/// How many times a transient FoE failure is re-issued before giving up, and the pause between
/// attempts. Only kinds that @c FoeError classifies as transient are retried; a missing file or an
/// undersized buffer will fail identically however many times it is asked.
constexpr int kFoeAttempts = 5;
constexpr auto kFoeRetryDelay = std::chrono::milliseconds(100);

/// Sleeps, but notices a cancellation. std::this_thread::sleep_for cannot be interrupted, and
/// request_stop does not wake a plain condition_variable, so this checks in slices instead.
/// @return false if the sleep was cut short by a stop request.
bool sleepUnlessStopped(std::chrono::milliseconds duration, const std::stop_token& stop) {
  constexpr auto kSlice = std::chrono::milliseconds(50);
  auto remaining = duration;
  while (remaining > std::chrono::milliseconds::zero()) {
    if (stop.stop_requested()) {
      return false;
    }
    const auto slice = std::min(kSlice, remaining);
    std::this_thread::sleep_for(slice);
    remaining -= slice;
  }
  return !stop.stop_requested();
}

std::string cancelled(std::string_view what) {
  return std::format("firmware installation was cancelled {}", what);
}

/// Writes one file over FoE, re-issuing transient failures.
///
/// The device is borrowed per attempt rather than once around the loop: the borrow must not be held
/// across the back-off sleep, and re-resolving is also what makes an interleaved rescan surface as
/// a clean "device not found" instead of a stale reference.
std::expected<void, std::string> writeFileWithRetry(DeviceManager& deviceManager,
                                                    uint16_t devicePosition,
                                                    const std::string& filename,
                                                    std::span<const uint8_t> content,
                                                    const std::stop_token& stop) {
  std::string lastError;
  for (int attempt = 1; attempt <= kFoeAttempts; ++attempt) {
    if (stop.stop_requested()) {
      return std::unexpected(cancelled(std::format("before writing '{}'", filename)));
    }
    auto written = deviceManager.withDevice(devicePosition, [&](Device& device) -> Borrowed<void> {
      return device.writeFile(filename, content);
    });
    // The outer expected reports a borrow failure (a string); the inner one the transfer itself.
    if (!written) {
      return std::unexpected(written.error());
    }
    if (*written) {
      return {};
    }
    const FoeError& error = written->error();
    lastError = error.message;
    if (error.retry == Retry::Permanent) {
      return std::unexpected(lastError);
    }
    spdlog::warn("FoE write of '{}' failed ({}); attempt {} of {}", filename, lastError, attempt,
                 kFoeAttempts);
    if (attempt < kFoeAttempts && !sleepUnlessStopped(kFoeRetryDelay, stop)) {
      return std::unexpected(cancelled(std::format("while writing '{}'", filename)));
    }
  }
  return std::unexpected(std::format("{} (gave up after {} attempts)", lastError, kFoeAttempts));
}

/// Deletes a file from the drive's flash, treating "there was no such file" as success.
///
/// Removal is issued as an FoE *read* of a pseudo-file, which is why this branches on the error
/// kind rather than the message: a @c FileNotFound means the removal had nothing to do, which is
/// precisely the outcome the caller wanted.
std::expected<void, std::string> removeFile(DeviceManager& deviceManager, uint16_t devicePosition,
                                            const std::string& filename) {
  auto removed = deviceManager.withDevice(
      devicePosition, [&](Device& device) -> Borrowed<std::vector<uint8_t>> {
        return device.readFile(std::string(kRemoveFilePrefix) + filename);
      });
  if (!removed) {
    return std::unexpected(removed.error());
  }
  if (*removed || removed->error().kind == mm::comm::FoeErrorKind::FileNotFound) {
    return {};
  }
  return std::unexpected(removed->error().message);
}

std::expected<void, std::string> transitionTo(DeviceManager& deviceManager, uint16_t devicePosition,
                                              EtherCatState target,
                                              std::chrono::steady_clock::duration timeout) {
  auto states = deviceManager.transitionToState({devicePosition}, target, timeout);
  if (!states) {
    return std::unexpected(states.error());
  }
  if (states->empty()) {
    return std::unexpected(std::format("no state reported for device {}", devicePosition));
  }
  const auto& reached = states->front();
  if (reached.alState != static_cast<uint16_t>(target)) {
    // The AL status code is what makes this diagnosable rather than merely disappointing: a device
    // with no application answers a PRE-OP request with 0x0014, "No valid firmware", which says
    // exactly what happened instead of "it did not get there".
    std::string detail =
        std::format("the device reported {} rather than {}",
                    toString(static_cast<EtherCatState>(reached.alState)), toString(target));
    if (reached.alStatusCode != 0) {
      detail += std::format(" (AL status 0x{:04X}: {})", reached.alStatusCode,
                            mm::comm::alStatusCodeName(reached.alStatusCode));
    }
    return std::unexpected(detail);
  }
  return {};
}

/// Takes the device to BOOT. INIT first because the AL state machine only allows BOOT paired with
/// INIT — a drive in OP cannot jump straight there.
std::expected<void, std::string> enterBoot(DeviceManager& deviceManager, uint16_t devicePosition,
                                           std::chrono::milliseconds warmUp,
                                           const std::stop_token& stop) {
  if (auto r = transitionTo(deviceManager, devicePosition, EtherCatState::Init, kStateTimeout);
      !r) {
    return std::unexpected(std::format("could not reach INIT: {}", r.error()));
  }
  if (auto r = transitionTo(deviceManager, devicePosition, EtherCatState::Boot, kStateTimeout);
      !r) {
    return std::unexpected(std::format("could not reach BOOT: {}", r.error()));
  }
  // A freshly entered bootloader takes a moment before it services file operations. Issuing the
  // first FoE request too early fails *and* desynchronises the mailbox, so this is a wait rather
  // than something to retry into. The driver's mailbox drain recovers from that state now, but
  // waiting first is what avoids paying for it.
  if (!sleepUnlessStopped(warmUp, stop)) {
    return std::unexpected(cancelled("while the bootloader started"));
  }
  return {};
}

/// Leaves BOOT for @p finalState, one legal AL step at a time.
///
/// The state machine only pairs BOOT with INIT and only climbs one step at a time, so the walk is
/// fixed: INIT, then PRE-OP, then SAFE-OP, then OP, stopping wherever it was asked to.
std::expected<void, std::string> leaveBoot(DeviceManager& deviceManager, uint16_t devicePosition,
                                           EtherCatState finalState) {
  if (finalState == EtherCatState::Boot) {
    return {};
  }
  if (auto r = transitionTo(deviceManager, devicePosition, EtherCatState::Init, kStateTimeout);
      !r) {
    return std::unexpected(std::format("could not reach INIT: {}", r.error()));
  }
  if (finalState == EtherCatState::Init) {
    return {};
  }
  // The long one: this transition restarts the device into the firmware just written.
  if (auto r = transitionTo(deviceManager, devicePosition, EtherCatState::PreOp, kBootExitTimeout);
      !r) {
    return std::unexpected(std::format("could not reach PRE-OP: {}", r.error()));
  }
  if (finalState == EtherCatState::PreOp) {
    return {};
  }
  // Climbing further re-maps the whole bus, which DeviceManager handles — including re-reading this
  // device's PDO mapping, which the firmware just written may well have changed.
  if (auto r = transitionTo(deviceManager, devicePosition, EtherCatState::SafeOp, kStateTimeout);
      !r) {
    return std::unexpected(std::format("could not reach SAFE-OP: {}", r.error()));
  }
  if (finalState == EtherCatState::SafeOp) {
    return {};
  }
  if (auto r = transitionTo(deviceManager, devicePosition, EtherCatState::Op, kStateTimeout); !r) {
    return std::unexpected(std::format("could not reach OP: {}", r.error()));
  }
  return {};
}

std::expected<EtherCatState, std::string> parseFinalState(const nlohmann::json& value) {
  if (!value.is_number_unsigned()) {
    return std::unexpected(
        "'finalState' must be an AL state number — the same encoding POST /api/devices/state uses");
  }
  constexpr std::array<EtherCatState, 5> kStates = {EtherCatState::Init, EtherCatState::PreOp,
                                                    EtherCatState::Boot, EtherCatState::SafeOp,
                                                    EtherCatState::Op};
  const auto requested = static_cast<EtherCatState>(value.get<uint16_t>());
  if (std::ranges::find(kStates, requested) == kStates.end()) {
    return std::unexpected(
        std::format("'finalState' must be 1 (INIT), 2 (PRE-OP), 3 (BOOT), 4 (SAFE-OP) or 8 (OP)"));
  }
  return requested;
}

std::vector<std::string> defaultSkipFiles() {
  std::vector<std::string> files;
  files.reserve(kDefaultSkippedFirmwareFiles.size());
  for (std::string_view name : kDefaultSkippedFirmwareFiles) {
    files.emplace_back(name);
  }
  return files;
}

/// Resolves a client-supplied package filename to its place in the firmware cache.
///
/// This is the only thing standing between a caller and the rest of the filesystem, and the API in
/// front of it is unauthenticated — so it matters that the SOMANET name grammar is **not** a
/// substitute for it: `package_../../x_b_c_v1.zip` splits into five perfectly legal
/// underscore-separated fields. @c UserCache::resolve does the platform-specific half (absolute
/// paths, drive letters, `..` spelled with trailing dots or spaces, Windows device names) and is
/// already audited for exactly this; the separator check on top keeps the cache flat, since a
/// package is one file and never a tree.
std::expected<std::filesystem::path, std::string> resolveCachedPackage(std::string_view filename) {
  if (filename.find('/') != std::string_view::npos ||
      filename.find('\\') != std::string_view::npos) {
    return std::unexpected(std::format(
        "'{}' is not a plain filename — a firmware package is cached by name, not by path",
        filename));
  }
  return mm::core::UserCache(firmwareCacheDir()).resolve(filename);
}

std::expected<std::vector<uint8_t>, std::string> readCachedPackage(
    const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) || ec) {
    return std::unexpected(
        std::format("no package content was given and '{}' is not in the firmware cache",
                    path.filename().string()));
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::unexpected(std::format("could not read the cached package '{}'", path.string()));
  }
  return std::vector<uint8_t>{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
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

  auto encoder = readEncoderOrdinal(body, request.encoder);
  if (!encoder) {
    return std::unexpected(encoder.error());
  }
  request.encoder = *encoder;

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
      encoderParameter(defaults.encoder),
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

std::expected<IcMuCalibrationModeRequest, std::string> parseIcMuCalibrationModeRequest(
    const nlohmann::json& body) {
  if (!body.is_object()) {
    return std::unexpected("the request body must be a JSON object");
  }
  IcMuCalibrationModeRequest request;

  auto encoder = readEncoderOrdinal(body, request.encoder);
  if (!encoder) {
    return std::unexpected(encoder.error());
  }
  request.encoder = *encoder;

  auto mode = body.find("mode");
  if (mode == body.end() || mode->is_null()) {
    return std::unexpected("'mode' is required");
  }
  if (!mode->is_string()) {
    return std::unexpected("'mode' must be a string");
  }
  auto parsed = somanet::parseIcMuCalibrationMode(mode->get<std::string>());
  if (!parsed) {
    return std::unexpected(
        std::format("'mode' must be one of {}, {} or {}",
                    somanet::toString(somanet::IcMuCalibrationMode::kConfiguration),
                    somanet::toString(somanet::IcMuCalibrationMode::kRaw),
                    somanet::toString(somanet::IcMuCalibrationMode::kStandard)));
  }
  request.mode = *parsed;
  return request;
}

std::vector<ProcedureParameter> icMuCalibrationModeParameters() {
  const IcMuCalibrationModeRequest defaults;
  return {
      encoderParameter(defaults.encoder),
      enumParameter(
          "mode", "Mode",
          "Which mode to put the encoder in. Standard is normal operation; configuration stops "
          "position updating so the encoder's registers can be changed without a CRC fault; raw "
          "additionally averages the raw data into 0x2704. Required — the mode is the instruction.",
          nullptr,
          {
              ParameterOption{.value = std::string(
                                  somanet::toString(somanet::IcMuCalibrationMode::kConfiguration)),
                              .title = "Configuration"},
              ParameterOption{
                  .value = std::string(somanet::toString(somanet::IcMuCalibrationMode::kRaw)),
                  .title = "Raw"},
              ParameterOption{
                  .value = std::string(somanet::toString(somanet::IcMuCalibrationMode::kStandard)),
                  .title = "Standard"},
          }),
  };
}

std::vector<ProgressStep> icMuCalibrationModeSteps() {
  return stepsFrom({kIcMuCalibrationModeStep});
}

std::expected<void, std::string> runIcMuCalibrationModeProcedure(
    Device& device, ProgressReporter& reporter, std::stop_token stop,
    const IcMuCalibrationModeRequest& request) {
  auto drive = createSomanetDrive(device);
  if (!drive) {
    return std::unexpected(drive.error());
  }

  reporter.start(kIcMuCalibrationModeStep);
  auto result = drive->setIcMuCalibrationMode(request.encoder, request.mode,
                                              {.timeout = kEncoderRegisterTimeout,
                                               .pollInterval = kEncoderRegisterPollInterval,
                                               .stop = std::move(stop)});
  if (!result) {
    reporter.fail(kIcMuCalibrationModeStep, result.error());
    return std::unexpected(result.error());
  }
  // The command reports nothing, so the step records what was asked for: a snapshot read later has
  // to say which encoder is now in which mode, since nothing here restores it.
  reporter.succeed(kIcMuCalibrationModeStep,
                   nlohmann::json{{"encoder", static_cast<uint8_t>(request.encoder)},
                                  {"mode", somanet::toString(request.mode)}});
  return {};
}

std::expected<HrdStreamingRequest, std::string> parseHrdStreamingRequest(
    const nlohmann::json& body) {
  if (!body.is_object()) {
    return std::unexpected("the request body must be a JSON object");
  }
  HrdStreamingRequest request;

  auto data = body.find("data");
  if (data == body.end() || data->is_null()) {
    return std::unexpected("'data' is required");
  }
  if (!data->is_string()) {
    return std::unexpected("'data' must be a string");
  }
  auto parsed = somanet::parseHrdData(data->get<std::string>());
  if (!parsed) {
    return std::unexpected(std::format(
        "'data' must be one of {} or {}", somanet::toString(somanet::HrdData::kEncoderRawData),
        somanet::toString(somanet::HrdData::kSystemIdentificationData)));
  }
  request.data = *parsed;

  auto duration = body.find("durationMs");
  if (duration == body.end() || duration->is_null()) {
    return std::unexpected("'durationMs' is required");
  }
  // Bounded by the chosen format's own limit, not by one shared ceiling — the same two limits the
  // drive applies, checked here so the answer is a 400 naming the limit for the data chosen rather
  // than a run that fails on its first step.
  auto milliseconds = readMillis(body, "durationMs", request.duration, std::chrono::milliseconds(1),
                                 somanet::maxHrdStreamDuration(request.data));
  if (!milliseconds) {
    return std::unexpected(milliseconds.error());
  }
  request.duration = *milliseconds;
  return request;
}

std::vector<ProcedureParameter> hrdStreamingParameters() {
  return {
      enumParameter(
          "data", "Data",
          "Which signal to record. Encoder raw data is the position word an iC-MU encoder reports, "
          "and records zeros unless the encoder was put into raw mode first. System identification "
          "data is the velocity and torque actual values, and records an unexcited drive unless a "
          "system identification run was started first. Required — it also decides how the "
          "recording decodes when it is read back.",
          nullptr,
          {
              ParameterOption{
                  .value = std::string(somanet::toString(somanet::HrdData::kEncoderRawData)),
                  .title = "Encoder raw data"},
              ParameterOption{.value = std::string(
                                  somanet::toString(somanet::HrdData::kSystemIdentificationData)),
                              .title = "System identification data"},
          }),
      integerParameter(
          "durationMs", "Duration (ms)",
          "How long to record for. One sample is written every millisecond into at most five "
          "8032-byte files, so the ceiling is whatever fills them: 10000 ms of encoder raw data, "
          "but only 6000 ms of system identification data. Required.",
          nullptr, 1, somanet::maxHrdStreamDuration(somanet::HrdData::kEncoderRawData).count()),
  };
}

std::vector<ProgressStep> hrdStreamingSteps() {
  return stepsFrom({kHrdConfigureStep, kHrdRecordStep});
}

std::expected<void, std::string> runHrdStreamingProcedure(Device& device,
                                                          ProgressReporter& reporter,
                                                          std::stop_token stop,
                                                          const HrdStreamingRequest& request) {
  auto drive = createSomanetDrive(device);
  if (!drive) {
    return std::unexpected(drive.error());
  }

  reporter.start(kHrdConfigureStep);
  auto configured = drive->configureHrdStream(
      request.data, request.duration,
      {.timeout = kHrdConfigureTimeout, .pollInterval = kHrdPollInterval, .stop = stop});
  if (!configured) {
    reporter.fail(kHrdConfigureStep, configured.error());
    return std::unexpected(configured.error());
  }
  // What the recording will hold, reported before it is made rather than after: reading it back
  // takes this same selection, and nothing on the drive remembers it.
  reporter.succeed(kHrdConfigureStep, nlohmann::json{{"data", somanet::toString(request.data)},
                                                     {"durationMs", request.duration.count()}});

  reporter.start(kHrdRecordStep);
  // The drive holds the command for the whole recording, so the ceiling is the duration plus room
  // for the poll cadence and the drive's own wrap-up — not a fixed figure.
  auto recorded = drive->startHrdStream({.timeout = request.duration + kHrdRecordMargin,
                                         .pollInterval = kHrdPollInterval,
                                         .stop = std::move(stop)});
  if (!recorded) {
    reporter.fail(kHrdRecordStep, recorded.error());
    return std::unexpected(recorded.error());
  }
  reporter.succeed(kHrdRecordStep);
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

namespace {

// Reads a required unsigned 32-bit field, bounded. Shared by the four chirp numbers, whose limits
// differ but whose failure messages should not.
std::expected<uint32_t, std::string> readBoundedUint32(const nlohmann::json& body,
                                                       const char* field, uint32_t minValue,
                                                       uint32_t maxValue) {
  auto it = body.find(field);
  if (it == body.end() || it->is_null()) {
    return std::unexpected(std::format("'{}' is required", field));
  }
  if (!it->is_number_integer()) {
    return std::unexpected(
        std::format("'{}' must be a whole number ({}-{})", field, minValue, maxValue));
  }
  const int64_t raw = it->get<int64_t>();
  if (raw < static_cast<int64_t>(minValue) || raw > static_cast<int64_t>(maxValue)) {
    return std::unexpected(
        std::format("'{}' must be {}-{}, got {}", field, minValue, maxValue, raw));
  }
  return static_cast<uint32_t>(raw);
}

}  // namespace

std::expected<KueblerRegisterRequest, std::string> parseKueblerRegisterRequest(
    const nlohmann::json& body) {
  if (!body.is_object()) {
    return std::unexpected("the request body must be a JSON object");
  }
  KueblerRegisterRequest request;

  auto address = readByte(body, "address", std::nullopt);
  if (!address) {
    return std::unexpected(address.error());
  }
  request.address = *address;

  auto length = readByte(body, "length", std::nullopt);
  if (!length) {
    return std::unexpected(length.error());
  }
  if (*length < 1 || *length > somanet::kMaxKueblerRegisterBytes) {
    return std::unexpected(
        std::format("'length' must be 1 to {} bytes, got {} — the command's length byte caps "
                    "there, so a 64-bit "
                    "register cannot be transferred in one command",
                    somanet::kMaxKueblerRegisterBytes, *length));
  }
  request.length = *length;

  if (auto write = body.find("write"); write != body.end() && !write->is_null()) {
    if (!write->is_boolean()) {
      return std::unexpected("'write' must be a boolean");
    }
    request.write = write->get<bool>();
  }

  if (auto value = body.find("value"); value != body.end() && !value->is_null()) {
    if (!value->is_number_integer()) {
      return std::unexpected("'value' must be a whole number");
    }
    const int64_t raw = value->get<int64_t>();
    if (raw < 0 || raw > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
      return std::unexpected(std::format("'value' must be 0 to {}, got {}",
                                         std::numeric_limits<uint32_t>::max(), raw));
    }
    request.value = static_cast<uint32_t>(raw);
  }

  // Checked here as well as in the request, because a value wider than the register is a caller
  // mistake the encoder cannot report: it takes the low bytes and echoes them, so the run would
  // look like it succeeded at writing something else.
  if (request.write && request.length < 4 &&
      request.value >= (uint32_t{1} << (8U * request.length))) {
    return std::unexpected(
        std::format("'value' {} does not fit {} byte{} — it would be silently truncated to {}",
                    request.value, request.length, request.length == 1 ? "" : "s",
                    request.value & ((uint32_t{1} << (8U * request.length)) - 1)));
  }
  return request;
}

std::vector<ProcedureParameter> kueblerRegisterParameters() {
  return {
      integerParameter("address", "Register address",
                       "Which register of the encoder to access. The map is at "
                       "GET /api/meta/kuebler-registers, which also gives each register's width.",
                       nullptr, 0, 0xFF),
      integerParameter("length", "Length (bytes)",
                       "Width of the register in bytes, 1 to 4. It must match the register's real "
                       "width — the encoder refuses a mismatch rather than truncating. A 64-bit "
                       "register cannot be transferred in one command.",
                       nullptr, 1, somanet::kMaxKueblerRegisterBytes),
      booleanParameter(
          "write", "Write",
          "Off reads the register; on writes the value into it. Either way the encoder "
          "answers with the register's value, so a write confirms itself.",
          false),
      integerParameter("value", "Value",
                       "The value to write, ignored when reading. It must fit the chosen length; a "
                       "wider one is refused rather than truncated.",
                       0, 0, std::numeric_limits<uint32_t>::max()),
  };
}

std::vector<ProgressStep> kueblerRegisterSteps() { return stepsFrom({kKueblerRegisterStep}); }

std::expected<void, std::string> runKueblerRegisterProcedure(
    Device& device, ProgressReporter& reporter, std::stop_token stop,
    const KueblerRegisterRequest& request) {
  auto drive = createSomanetDrive(device);
  if (!drive) {
    return std::unexpected(drive.error());
  }

  reporter.start(kKueblerRegisterStep);
  auto result =
      drive->accessKueblerRegister(request.address, request.length, request.write, request.value,
                                   {.timeout = kEncoderRegisterTimeout,
                                    .pollInterval = kEncoderRegisterPollInterval,
                                    .stop = std::move(stop)});
  if (!result) {
    reporter.fail(kKueblerRegisterStep, result.error());
    return std::unexpected(result.error());
  }
  reporter.succeed(kKueblerRegisterStep, *result);
  return {};
}

std::expected<VelocitySourceRequest, std::string> parseVelocitySourceRequest(
    const nlohmann::json& body) {
  if (!body.is_object()) {
    return std::unexpected("the request body must be a JSON object");
  }
  auto source = body.find("source");
  if (source == body.end() || source->is_null()) {
    return std::unexpected("'source' is required");
  }
  if (!source->is_string()) {
    return std::unexpected("'source' must be a string");
  }
  auto parsed = somanet::parseVelocitySource(source->get<std::string>());
  if (!parsed) {
    return std::unexpected(std::format("'source' must be {} or {}",
                                       somanet::toString(somanet::VelocitySource::kFirmware),
                                       somanet::toString(somanet::VelocitySource::kEncoder)));
  }
  return VelocitySourceRequest{.source = *parsed};
}

std::vector<ProcedureParameter> velocitySourceParameters() {
  return {
      enumParameter(
          "source", "Velocity source",
          "Which velocity the control loop should use. Firmware differentiates it from encoder "
          "position; encoder takes what the encoder integrated itself, which only the Integro's "
          "internal encoder provides. Required — which one is already active depends on the "
          "product, so there is no safe default to assume.",
          nullptr,
          {
              ParameterOption{
                  .value = std::string(somanet::toString(somanet::VelocitySource::kFirmware)),
                  .title = "Firmware — differentiated from position"},
              ParameterOption{
                  .value = std::string(somanet::toString(somanet::VelocitySource::kEncoder)),
                  .title = "Encoder — reported by the encoder itself"},
          }),
  };
}

std::vector<ProgressStep> velocitySourceSteps() { return stepsFrom({kVelocitySourceStep}); }

std::expected<void, std::string> runVelocitySourceProcedure(Device& device,
                                                            ProgressReporter& reporter,
                                                            std::stop_token stop,
                                                            const VelocitySourceRequest& request) {
  auto drive = createSomanetDrive(device);
  if (!drive) {
    return std::unexpected(drive.error());
  }

  reporter.start(kVelocitySourceStep);
  auto result =
      drive->setVelocitySource(request.source, {.timeout = kEncoderRegisterTimeout,
                                                .pollInterval = kEncoderRegisterPollInterval,
                                                .stop = std::move(stop)});
  if (!result) {
    reporter.fail(kVelocitySourceStep, result.error());
    return std::unexpected(result.error());
  }
  // Nothing on the drive reports the choice back, so the step's record is the only account of it.
  reporter.succeed(kVelocitySourceStep,
                   nlohmann::json{{"source", somanet::toString(request.source)}});
  return {};
}

std::expected<TriggerErrorRequest, std::string> parseTriggerErrorRequest(
    const nlohmann::json& body) {
  if (!body.is_object()) {
    return std::unexpected("the request body must be a JSON object");
  }
  TriggerErrorRequest request;

  if (auto service = body.find("service"); service != body.end() && !service->is_null()) {
    if (!service->is_string()) {
      return std::unexpected("'service' must be a string");
    }
    auto parsed = somanet::parseFirmwareService(service->get<std::string>());
    if (!parsed) {
      return std::unexpected(std::format(
          "'service' must be {} or {}", somanet::toString(somanet::FirmwareService::kDriveControl),
          somanet::toString(somanet::FirmwareService::kMotionControl)));
    }
    request.service = *parsed;
  }

  auto errorType = body.find("errorType");
  if (errorType == body.end() || errorType->is_null()) {
    return std::unexpected("'errorType' is required");
  }
  if (!errorType->is_string()) {
    return std::unexpected("'errorType' must be a string");
  }
  auto parsed = somanet::parseFirmwareErrorType(errorType->get<std::string>());
  if (!parsed) {
    return std::unexpected(
        std::format("'errorType' is not one this firmware defines; use one of {}",
                    somanet::toString(somanet::FirmwareErrorType::kLoadStore)));
  }
  request.errorType = *parsed;
  return request;
}

std::vector<ProcedureParameter> triggerErrorParameters() {
  const TriggerErrorRequest defaults;
  std::vector<ParameterOption> types;
  for (const auto type : somanet::kFirmwareErrorTypes) {
    // The title carries what the type actually does, because the name does not: seven of them are
    // named after an exception the firmware never raises.
    std::string title(somanet::toString(type));
    switch (somanet::effectOf(type)) {
      case somanet::FirmwareErrorEffect::kNotImplemented:
        title += " — not implemented, does nothing";
        break;
      case somanet::FirmwareErrorEffect::kStopsService:
        title += " — STOPS the service until power cycle";
        break;
      case somanet::FirmwareErrorEffect::kRaisesResettableError:
        title += " — resettable fault";
        break;
    }
    types.push_back(
        ParameterOption{.value = std::string(somanet::toString(type)), .title = std::move(title)});
  }
  return {
      enumParameter(
          "service", "Firmware service", "Which control loop to provoke the error in.",
          std::string(somanet::toString(defaults.service)),
          {
              ParameterOption{
                  .value = std::string(somanet::toString(somanet::FirmwareService::kDriveControl)),
                  .title = "Drive control"},
              ParameterOption{
                  .value = std::string(somanet::toString(somanet::FirmwareService::kMotionControl)),
                  .title = "Motion control"},
          }),
      enumParameter("errorType", "Error type",
                    "Which error to raise. Required, and chosen with care: four of these stop the "
                    "service for good, seven do nothing at all, and one is recoverable.",
                    nullptr, std::move(types)),
  };
}

std::vector<ProgressStep> triggerErrorSteps() { return stepsFrom({kTriggerErrorStep}); }

std::expected<void, std::string> runTriggerErrorProcedure(Device& device,
                                                          ProgressReporter& reporter,
                                                          std::stop_token stop,
                                                          const TriggerErrorRequest& request) {
  auto drive = createSomanetDrive(device);
  if (!drive) {
    return std::unexpected(drive.error());
  }

  reporter.start(kTriggerErrorStep);
  auto result = drive->triggerError(request.service, request.errorType,
                                    {.timeout = kTriggerErrorTimeout,
                                     .pollInterval = kEncoderRegisterPollInterval,
                                     .stop = std::move(stop)});
  if (!result) {
    reporter.fail(kTriggerErrorStep, result.error());
    return std::unexpected(result.error());
  }
  reporter.succeed(kTriggerErrorStep, *result);
  return {};
}

std::expected<SystemIdentificationRequest, std::string> parseSystemIdentificationRequest(
    const nlohmann::json& body) {
  if (!body.is_object()) {
    return std::unexpected("the request body must be a JSON object");
  }
  SystemIdentificationRequest request;

  auto startFrequency =
      readBoundedUint32(body, "startFrequencyMilliHz", somanet::kMinChirpFrequencyMilliHz,
                        somanet::kMaxChirpFrequencyMilliHz);
  if (!startFrequency) {
    return std::unexpected(startFrequency.error());
  }
  request.startFrequencyMilliHz = *startFrequency;

  auto targetFrequency =
      readBoundedUint32(body, "targetFrequencyMilliHz", somanet::kMinChirpFrequencyMilliHz,
                        somanet::kMaxChirpFrequencyMilliHz);
  if (!targetFrequency) {
    return std::unexpected(targetFrequency.error());
  }
  request.targetFrequencyMilliHz = *targetFrequency;

  // The firmware's fourth rule, and the one a caller is most likely to trip: it sweeps upwards, so
  // a descending pair is rejected rather than swept backwards.
  if (request.startFrequencyMilliHz > request.targetFrequencyMilliHz) {
    return std::unexpected(std::format(
        "'startFrequencyMilliHz' ({}) must not be above 'targetFrequencyMilliHz' ({}) — the sweep "
        "runs upwards",
        request.startFrequencyMilliHz, request.targetFrequencyMilliHz));
  }

  auto transitionTime =
      readBoundedUint32(body, "transitionTimeMs", somanet::kMinChirpTransitionTimeMs,
                        somanet::kMaxChirpTransitionTimeMs);
  if (!transitionTime) {
    return std::unexpected(transitionTime.error());
  }
  request.transitionTimeMs = *transitionTime;

  // Bounded only by the wire, deliberately: the firmware does not check the amplitude, and a limit
  // invented here would refuse a value some machine legitimately needs. It is a torque command.
  auto amplitude =
      readBoundedUint32(body, "targetAmplitudePermil", 0, std::numeric_limits<uint32_t>::max());
  if (!amplitude) {
    return std::unexpected(amplitude.error());
  }
  request.targetAmplitudePermil = *amplitude;

  if (auto signalType = body.find("signalType");
      signalType != body.end() && !signalType->is_null()) {
    if (!signalType->is_string()) {
      return std::unexpected("'signalType' must be a string");
    }
    auto parsed = somanet::parseChirpSignalType(signalType->get<std::string>());
    if (!parsed) {
      return std::unexpected(std::format("'signalType' must be {} or {}",
                                         somanet::toString(somanet::ChirpSignalType::kLogarithmic),
                                         somanet::toString(somanet::ChirpSignalType::kLinear)));
    }
    request.signalType = *parsed;
  }

  if (auto start = body.find("start"); start != body.end() && !start->is_null()) {
    if (!start->is_string()) {
      return std::unexpected("'start' must be a string");
    }
    auto parsed = somanet::parseSystemIdentificationStart(start->get<std::string>());
    if (!parsed) {
      return std::unexpected(
          std::format("'start' must be {}, {} or {}",
                      somanet::toString(somanet::SystemIdentificationStart::kNone),
                      somanet::toString(somanet::SystemIdentificationStart::kImmediately),
                      somanet::toString(somanet::SystemIdentificationStart::kAfterHrdStreamStart)));
    }
    request.start = *parsed;
  }

  return request;
}

std::vector<ProcedureParameter> systemIdentificationParameters() {
  const SystemIdentificationRequest defaults;
  return {
      integerParameter("startFrequencyMilliHz", "Start frequency (mHz)",
                       "Where the sweep begins, in millihertz. Must not be above the target "
                       "frequency — the sweep runs upwards.",
                       nullptr, somanet::kMinChirpFrequencyMilliHz,
                       somanet::kMaxChirpFrequencyMilliHz),
      integerParameter("targetFrequencyMilliHz", "Target frequency (mHz)",
                       "Where the sweep ends, in millihertz.", nullptr,
                       somanet::kMinChirpFrequencyMilliHz, somanet::kMaxChirpFrequencyMilliHz),
      integerParameter("targetAmplitudePermil", "Target amplitude (‰ of rated torque)",
                       "Peak excitation, in per-mille of rated torque. The logarithmic chirp "
                       "starts at half this and rises to it; the linear one holds it throughout. "
                       "Neither this server nor the drive checks this value — it is a torque "
                       "command, so choose it for the machine.",
                       nullptr, 0, std::numeric_limits<uint32_t>::max()),
      integerParameter("transitionTimeMs", "Transition time (ms)",
                       "How long the sweep takes, in milliseconds.", nullptr,
                       somanet::kMinChirpTransitionTimeMs, somanet::kMaxChirpTransitionTimeMs),
      enumParameter(
          "signalType", "Signal type",
          "Logarithmic sweeps the frequency logarithmically and raises the amplitude "
          "with it, from half the target; linear sweeps the frequency linearly at a "
          "constant amplitude.",
          std::string(somanet::toString(defaults.signalType)),
          {
              ParameterOption{
                  .value = std::string(somanet::toString(somanet::ChirpSignalType::kLogarithmic)),
                  .title = "Logarithmic, rising amplitude"},
              ParameterOption{
                  .value = std::string(somanet::toString(somanet::ChirpSignalType::kLinear)),
                  .title = "Linear, constant amplitude"},
          }),
      enumParameter(
          "start", "Start",
          "Whether to arm the run. None configures the drive and excites "
          "nothing. Immediately starts on the next control cycle if the drive is "
          "enabled. After HRD stream start waits for a high resolution "
          "recording to begin, which is the pairing that captures the response.",
          std::string(somanet::toString(defaults.start)),
          {
              ParameterOption{.value = std::string(
                                  somanet::toString(somanet::SystemIdentificationStart::kNone)),
                              .title = "None — configure only"},
              ParameterOption{.value = std::string(somanet::toString(
                                  somanet::SystemIdentificationStart::kImmediately)),
                              .title = "Immediately"},
              ParameterOption{.value = std::string(somanet::toString(
                                  somanet::SystemIdentificationStart::kAfterHrdStreamStart)),
                              .title = "After HRD stream start"},
          }),
  };
}

std::vector<ProgressStep> systemIdentificationSteps() {
  return stepsFrom({kSystemIdConfigureStep, kSystemIdArmStep});
}

std::expected<void, std::string> runSystemIdentificationProcedure(
    Device& device, ProgressReporter& reporter, std::stop_token stop,
    const SystemIdentificationRequest& request) {
  auto drive = createSomanetDrive(device);
  if (!drive) {
    return std::unexpected(drive.error());
  }

  const OsCommandConfig config{.timeout = kEncoderRegisterTimeout,
                               .pollInterval = kEncoderRegisterPollInterval,
                               .stop = std::move(stop)};

  using Parameter = somanet::SystemIdentificationParameter;
  // Disarm first, then the five settings. The disarm is what makes the arm below a rising edge,
  // which is the only thing the firmware acts on.
  const std::array<std::pair<Parameter, uint32_t>, 6> settings{{
      {Parameter::kStartProcedure,
       static_cast<uint32_t>(somanet::SystemIdentificationStart::kNone)},
      {Parameter::kStartFrequency, request.startFrequencyMilliHz},
      {Parameter::kTargetFrequency, request.targetFrequencyMilliHz},
      {Parameter::kTargetAmplitude, request.targetAmplitudePermil},
      {Parameter::kTransitionTime, request.transitionTimeMs},
      {Parameter::kSignalType, static_cast<uint32_t>(request.signalType)},
  }};

  reporter.start(kSystemIdConfigureStep);
  for (const auto& [parameter, value] : settings) {
    if (config.stop.stop_requested()) {
      const auto reason =
          std::format("system identification was cancelled after {}", somanet::toString(parameter));
      reporter.fail(kSystemIdConfigureStep, reason);
      return std::unexpected(reason);
    }
    if (auto r = drive->setSystemIdentificationParameter(parameter, value, config); !r) {
      reporter.fail(kSystemIdConfigureStep, r.error());
      return std::unexpected(r.error());
    }
  }
  // Nothing reads these back, so the step's record is the only account of what the drive holds.
  reporter.succeed(kSystemIdConfigureStep,
                   nlohmann::json{{"startFrequencyMilliHz", request.startFrequencyMilliHz},
                                  {"targetFrequencyMilliHz", request.targetFrequencyMilliHz},
                                  {"targetAmplitudePermil", request.targetAmplitudePermil},
                                  {"transitionTimeMs", request.transitionTimeMs},
                                  {"signalType", somanet::toString(request.signalType)}});

  reporter.start(kSystemIdArmStep);
  if (auto r = drive->setSystemIdentificationParameter(
          Parameter::kStartProcedure, static_cast<uint32_t>(request.start), config);
      !r) {
    reporter.fail(kSystemIdArmStep, r.error());
    return std::unexpected(r.error());
  }
  reporter.succeed(kSystemIdArmStep, nlohmann::json{{"start", somanet::toString(request.start)}});
  return {};
}

std::expected<IgnoreBissStatusBitsRequest, std::string> parseIgnoreBissStatusBitsRequest(
    const nlohmann::json& body) {
  if (!body.is_object()) {
    return std::unexpected("the request body must be a JSON object");
  }
  IgnoreBissStatusBitsRequest request;

  auto encoder = readEncoderOrdinal(body, request.encoder);
  if (!encoder) {
    return std::unexpected(encoder.error());
  }
  request.encoder = *encoder;

  auto ignore = body.find("ignore");
  if (ignore == body.end() || ignore->is_null()) {
    return std::unexpected("'ignore' is required");
  }
  if (!ignore->is_boolean()) {
    return std::unexpected("'ignore' must be a boolean");
  }
  request.ignore = ignore->get<bool>();
  return request;
}

std::vector<ProcedureParameter> ignoreBissStatusBitsParameters() {
  const IgnoreBissStatusBitsRequest defaults;
  return {
      encoderParameter(defaults.encoder),
      booleanParameter("ignore", "Ignore",
                       "On stops the firmware acting on the encoder's status bits; off restores "
                       "the default. Required — the direction is the instruction, and nothing "
                       "here restores it afterwards.",
                       nullptr),
  };
}

std::vector<ProgressStep> ignoreBissStatusBitsSteps() {
  return stepsFrom({kIgnoreBissStatusBitsStep});
}

std::expected<void, std::string> runIgnoreBissStatusBitsProcedure(
    Device& device, ProgressReporter& reporter, std::stop_token stop,
    const IgnoreBissStatusBitsRequest& request) {
  auto drive = createSomanetDrive(device);
  if (!drive) {
    return std::unexpected(drive.error());
  }

  reporter.start(kIgnoreBissStatusBitsStep);
  auto result = drive->setIgnoreBissStatusBits(request.encoder, request.ignore,
                                               {.timeout = kBissServiceTimeout,
                                                .pollInterval = kEncoderRegisterPollInterval,
                                                .stop = std::move(stop)});
  if (!result) {
    reporter.fail(kIgnoreBissStatusBitsStep, result.error());
    return std::unexpected(result.error());
  }
  // The command answers with no payload, so the step records what was asked for — which is also
  // what a later reader needs, since nothing on the drive reports the flag back.
  reporter.succeed(kIgnoreBissStatusBitsStep,
                   nlohmann::json{{"encoder", static_cast<uint8_t>(request.encoder)},
                                  {"ignore", request.ignore}});
  return {};
}

std::expected<SkippedCyclesRequest, std::string> parseSkippedCyclesRequest(
    const nlohmann::json& body) {
  if (!body.is_object()) {
    return std::unexpected("the request body must be a JSON object");
  }
  SkippedCyclesRequest request;

  auto service = body.find("service");
  if (service == body.end() || service->is_null()) {
    return request;
  }
  if (!service->is_string()) {
    return std::unexpected("'service' must be a string");
  }
  auto parsed = somanet::parseFirmwareService(service->get<std::string>());
  if (!parsed) {
    return std::unexpected(std::format(
        "'service' must be {} or {}", somanet::toString(somanet::FirmwareService::kDriveControl),
        somanet::toString(somanet::FirmwareService::kMotionControl)));
  }
  request.service = *parsed;
  return request;
}

std::vector<ProcedureParameter> skippedCyclesParameters() {
  const SkippedCyclesRequest defaults;
  return {
      enumParameter(
          "service", "Firmware service",
          "Which control loop to ask. The two are scheduled independently and counted separately, "
          "so a reading from one says nothing about the other: drive control is the fast "
          "current/torque loop, motion control the position/velocity loop above it.",
          std::string(somanet::toString(defaults.service)),
          {
              ParameterOption{
                  .value = std::string(somanet::toString(somanet::FirmwareService::kDriveControl)),
                  .title = "Drive control"},
              ParameterOption{
                  .value = std::string(somanet::toString(somanet::FirmwareService::kMotionControl)),
                  .title = "Motion control"},
          }),
  };
}

std::vector<ProgressStep> skippedCyclesSteps() { return stepsFrom({kSkippedCyclesStep}); }

std::expected<void, std::string> runSkippedCyclesProcedure(Device& device,
                                                           ProgressReporter& reporter,
                                                           std::stop_token stop,
                                                           const SkippedCyclesRequest& request) {
  auto drive = createSomanetDrive(device);
  if (!drive) {
    return std::unexpected(drive.error());
  }

  reporter.start(kSkippedCyclesStep);
  auto result =
      drive->readSkippedCycles(request.service, {.timeout = kSkippedCyclesTimeout,
                                                 .pollInterval = kEncoderRegisterPollInterval,
                                                 .stop = std::move(stop)});
  if (!result) {
    reporter.fail(kSkippedCyclesStep, result.error());
    return std::unexpected(result.error());
  }
  reporter.succeed(kSkippedCyclesStep, *result);
  return {};
}

std::vector<ProgressStep> torqueConstantMeasurementSteps() {
  return stepsFrom({kPrepareStep, kReleaseBrakeStep, kTorqueConstantMeasurementStep, kRestoreStep});
}

std::expected<void, std::string> runTorqueConstantMeasurementProcedure(Device& device,
                                                                       ProgressReporter& reporter,
                                                                       std::stop_token stop) {
  // Releases the brake, like pole pair and motor phase order detection, because this command's
  // restrictions require it — and takes their longer ceiling rather than the winding measurements'
  // for its own reason: the drive spends about ten seconds ramping the motor up to speed before it
  // measures anything.
  return runMeasurementProcedure(device, reporter, std::move(stop),
                                 {.procedure = kTorqueConstantMeasurementProcedure,
                                  .step = kTorqueConstantMeasurementStep,
                                  .what = "torque constant measurement",
                                  .releaseBrake = true,
                                  .timeout = std::chrono::seconds(60)},
                                 [](SomanetDrive& drive, const OsCommandConfig& config) {
                                   return drive.runTorqueConstantMeasurement(config);
                                 });
}

std::filesystem::path firmwareCacheDir() { return mm::core::userCacheDir() / "firmwares"; }

std::vector<ProcedureParameter> firmwareInstallationParameters() {
  // Built from the same helper the parser defaults to, so the advertised default and the applied
  // one cannot drift.
  nlohmann::json skipDefault = defaultSkipFiles();
  return {
      fileParameter(
          "packageContent", "Firmware package",
          "The .zip firmware package, base64-encoded. May be omitted when 'packageFilename' names "
          "a package already in the firmware cache — which is how a package is re-installed "
          "without uploading it again, and also the way to avoid base64 entirely: PUT the raw "
          "bytes to /api/user-cache/firmwares/<name>.zip first, then start this with only the "
          "filename.",
          nullptr),
      stringParameter("packageFilename", "Package filename",
                      "The package's filename, e.g. "
                      "package_SOMANET-Circulo-7_8500-04-2332_motion-drive_v5.6.10.zip. Optional: "
                      "it names the package for caching and identifies a cached one to re-install. "
                      "A name that does not follow the SOMANET convention still installs, it is "
                      "just not cached.",
                      ""),
      stringArrayParameter(
          "skipFiles", "Files to skip",
          "Entries inside the package that are not written to the device. The defaults are the ESI "
          "and the stack image, which are descriptive extras nothing on the drive reads and which "
          "are slow to write. Any entry can be listed, including the SII and the firmware binaries "
          "themselves.",
          std::move(skipDefault)),
      booleanParameter("cachePackage", "Cache the package",
                       "Keep a copy of the package on the server so it can be re-installed without "
                       "uploading it again. Only possible when a filename is given and it follows "
                       "the SOMANET package naming convention.",
                       true),
      enumParameter(
          "finalState", "Final state",
          "Where the device is left. PRE-OP is the confirmation that the install worked: the "
          "bootloader hands over to the newly written firmware on this transition, so reaching it "
          "means the new firmware booted. Choose BOOT when no application will be present — after "
          "erasing one, or between two installs — since there is then nothing to hand over to. "
          "SAFE-OP and OP climb through PRE-OP and re-map the whole bus on the way, briefly "
          "pausing "
          "every other device. The values are AL state numbers, the same encoding "
          "POST /api/devices/state takes.",
          // Unsigned literals on purpose: nlohmann stores a non-negative integer built from a C++
          // `int` as *signed*, and the validator checks is_number_unsigned() because that is what a
          // parsed request carries. A signed default would be a value the descriptor advertises and
          // the server then rejects.
          static_cast<unsigned>(EtherCatState::PreOp),
          {
              ParameterOption{static_cast<unsigned>(EtherCatState::PreOp),
                              "PRE-OP — loads and confirms the new firmware"},
              ParameterOption{static_cast<unsigned>(EtherCatState::Init),
                              "INIT — still on the bootloader"},
              ParameterOption{static_cast<unsigned>(EtherCatState::Boot),
                              "BOOT — stay in the bootloader"},
              ParameterOption{static_cast<unsigned>(EtherCatState::SafeOp),
                              "SAFE-OP — re-maps the bus and resumes inputs"},
              ParameterOption{static_cast<unsigned>(EtherCatState::Op),
                              "OP — re-maps the bus and resumes full exchange"},
          }),
  };
}

std::vector<ProgressStep> firmwareInstallationSteps() {
  return stepsFrom({kPackageStep, kCacheStep, kBootStep, kExtraFilesStep, kSiiStep,
                    kAppFirmwareStep, kComFirmwareStep, kFinalStateStep});
}

std::expected<FirmwareInstallationRequest, std::string> parseFirmwareInstallationRequest(
    const nlohmann::json& body) {
  if (!body.is_object()) {
    return std::unexpected("the request body must be a JSON object");
  }

  FirmwareInstallationRequest request;
  request.skipFiles = defaultSkipFiles();

  if (auto filename = body.find("packageFilename"); filename != body.end()) {
    if (!filename->is_string()) {
      return std::unexpected("'packageFilename' must be a string");
    }
    request.filename = filename->get<std::string>();
  }
  if (auto skip = body.find("skipFiles"); skip != body.end()) {
    if (!skip->is_array()) {
      return std::unexpected("'skipFiles' must be an array of entry names");
    }
    request.skipFiles.clear();
    for (const auto& name : *skip) {
      if (!name.is_string()) {
        return std::unexpected("every entry of 'skipFiles' must be a string");
      }
      request.skipFiles.push_back(name.get<std::string>());
    }
  }
  if (auto cache = body.find("cachePackage"); cache != body.end()) {
    if (!cache->is_boolean()) {
      return std::unexpected("'cachePackage' must be true or false");
    }
    request.cachePackage = cache->get<bool>();
  }
  if (auto finalState = body.find("finalState"); finalState != body.end()) {
    auto parsed = parseFinalState(*finalState);
    if (!parsed) {
      return std::unexpected(parsed.error());
    }
    request.finalState = *parsed;
  }

  // Decoded before the bytes are resolved, because a cached package is addressed by it — and
  // because a name that does not parse is not an error, only a package that will not be cached.
  if (!request.filename.empty()) {
    if (auto name = parseFirmwarePackageName(request.filename)) {
      request.name = std::move(*name);
    } else {
      spdlog::debug("firmware package name not recognised, it will not be cached: {}",
                    name.error());
    }
  }

  auto content = body.find("packageContent");
  const bool hasContent = content != body.end() && !content->is_null();
  if (hasContent) {
    if (!content->is_string()) {
      return std::unexpected("'packageContent' must be a base64-encoded string");
    }
    auto decoded = mm::core::base64Decode(content->get<std::string>());
    if (!decoded) {
      return std::unexpected(
          std::format("'packageContent' is not valid base64: {}", decoded.error()));
    }
    request.package = std::move(*decoded);
  } else {
    if (request.filename.empty()) {
      return std::unexpected(
          "the request must carry 'packageContent', or a 'packageFilename' naming a package "
          "already in the firmware cache");
    }
    // Deliberately not gated on the name parsing. Whether a name follows the SOMANET convention
    // decides whether a package is *written* to the cache; a package already there — put there by
    // an earlier install or uploaded straight to `firmwares/` through /api/user-cache — is read
    // back under whatever it is called.
    auto path = resolveCachedPackage(request.filename);
    if (!path) {
      return std::unexpected(path.error());
    }
    auto cached = readCachedPackage(*path);
    if (!cached) {
      return std::unexpected(cached.error());
    }
    request.package = std::move(*cached);
  }
  if (request.package.empty()) {
    return std::unexpected("the firmware package is empty");
  }
  return request;
}

std::expected<void, std::string> runFirmwareInstallationProcedure(
    DeviceManager& deviceManager, uint16_t devicePosition, ProgressReporter& reporter,
    std::stop_token stop, const FirmwareInstallationRequest& request) {
  // ── package ────────────────────────────────────────────────────────────────────────────────
  reporter.start(kPackageStep);
  auto package = openFirmwarePackage(request.package, request.skipFiles);
  if (!package) {
    reporter.fail(kPackageStep, package.error());
    return std::unexpected(package.error());
  }
  if (package->sii) {
    // Checked before anything is written, because an EEPROM write is the one operation here that
    // can leave a device unidentifiable, and a malformed image is discovered only after a power
    // cycle — long past the point where it could have been declined.
    if (auto valid = mm::comm::validateSiiImage(package->sii->content); !valid) {
      const std::string reason = std::format("the package's '{}' is not a valid SII image: {}",
                                             package->sii->name, valid.error());
      reporter.fail(kPackageStep, reason);
      return std::unexpected(reason);
    }
  }
  {
    nlohmann::json manifest{
        {"filename", request.filename},
        {"appBinary", package->appBinary ? nlohmann::json(package->appBinary->name) : nullptr},
        {"comBinary", package->comBinary ? nlohmann::json(package->comBinary->name) : nullptr},
        {"sii", package->sii ? nlohmann::json(package->sii->name) : nullptr},
        {"skipped", package->skipped},
        {"finalState", toString(request.finalState)},
    };
    auto extras = nlohmann::json::array();
    for (const auto& extra : package->extras) {
      extras.push_back(extra.name);
    }
    manifest["extras"] = std::move(extras);
    if (request.name) {
      manifest["softwareVersion"] = request.name->softwareVersion;
      manifest["fullFirmwareDescriptor"] = request.name->fullFirmwareDescriptor;
    }
    reporter.succeed(kPackageStep, std::move(manifest));
  }

  const bool writesAnything =
      package->appBinary || package->comBinary || package->sii || !package->extras.empty();
  if (!writesAnything) {
    const std::string reason = "every entry in the package is in the skip list — nothing to write";
    reporter.fail(kPackageStep, reason);
    return std::unexpected(reason);
  }

  // ── cache ──────────────────────────────────────────────────────────────────────────────────
  reporter.start(kCacheStep);
  if (!request.cachePackage) {
    reporter.succeed(kCacheStep, "not cached: caching was not requested");
  } else if (!request.name) {
    reporter.succeed(kCacheStep,
                     request.filename.empty()
                         ? "not cached: no filename was given"
                         : std::format("not cached: '{}' is not a SOMANET firmware package name",
                                       request.filename));
  } else if (auto path = resolveCachedPackage(request.filename); !path) {
    // Resolved rather than joined, because this one *writes*: the filename comes from an
    // unauthenticated request, and a name that escapes the cache directory would put
    // attacker-chosen bytes at an attacker-chosen path. A name that parses as a SOMANET package can
    // still do that — the underscore grammar happily accepts `..` in a field — so the check is
    // separate from it.
    reporter.succeed(kCacheStep, std::format("not cached: {}", path.error()));
  } else {
    std::error_code ec;
    std::filesystem::create_directories(path->parent_path(), ec);
    std::ofstream out(*path, std::ios::binary | std::ios::trunc);
    if (ec || !out ||
        !out.write(reinterpret_cast<const char*>(request.package.data()),
                   static_cast<std::streamsize>(request.package.size()))) {
      // Best effort by design: a full or read-only cache directory has nothing to do with whether
      // the firmware can be installed, so this records the miss and carries on.
      reporter.succeed(kCacheStep, std::format("not cached: could not write '{}'", path->string()));
    } else {
      reporter.succeed(kCacheStep, path->string());
    }
  }

  if (stop.stop_requested()) {
    return std::unexpected(cancelled("before the device was touched"));
  }

  // ── boot ───────────────────────────────────────────────────────────────────────────────────
  reporter.start(kBootStep);
  if (auto entered = enterBoot(deviceManager, devicePosition, request.bootWarmUp, stop); !entered) {
    reporter.fail(kBootStep, entered.error());
    return std::unexpected(entered.error());
  }
  reporter.succeed(kBootStep);

  // From here on the device is in BOOT, so every exit runs through this: whatever happened, the
  // device is taken to the requested final state rather than left in the bootloader by accident.
  std::optional<std::string> failure;
  auto finish = [&]() -> std::expected<void, std::string> {
    reporter.start(kFinalStateStep);
    auto left = leaveBoot(deviceManager, devicePosition, request.finalState);
    if (!left) {
      // Worded so it cannot be mistaken for a write failure: the bytes are already on the drive.
      const std::string reason =
          failure ? std::format("the install did not complete, and the device was left in BOOT: {}",
                                left.error())
                  : std::format("the firmware was written; the device did not reach {}: {}",
                                toString(request.finalState), left.error());
      reporter.fail(kFinalStateStep, reason);
      return std::unexpected(failure.value_or(reason));
    }
    reporter.succeed(kFinalStateStep, std::string(toString(request.finalState)));
    if (failure) {
      return std::unexpected(*failure);
    }
    return {};
  };

  // ── extra-files ────────────────────────────────────────────────────────────────────────────
  reporter.start(kExtraFilesStep);
  if (package->extras.empty()) {
    reporter.succeed(kExtraFilesStep, "no extra files to write");
  } else {
    auto results = nlohmann::json::object();
    for (const auto& extra : package->extras) {
      if (stop.stop_requested()) {
        failure = cancelled(std::format("before writing '{}'", extra.name));
        reporter.fail(kExtraFilesStep, *failure);
        return finish();
      }
      // Removed first so a rewrite cannot mix with whatever was there before.
      if (auto removed = removeFile(deviceManager, devicePosition, extra.name); !removed) {
        results[extra.name] = std::format("not removed: {}", removed.error());
      }
      auto written =
          writeFileWithRetry(deviceManager, devicePosition, extra.name, extra.content, stop);
      // Best effort: these are descriptive extras, and aborting a firmware update over a picture
      // would be a worse outcome than not having the picture.
      results[extra.name] = written ? "written" : std::format("not written: {}", written.error());
      if (!written) {
        spdlog::warn("device {}: could not write '{}' ({}); continuing the installation",
                     devicePosition, extra.name, written.error());
      }
    }
    reporter.succeed(kExtraFilesStep, std::move(results));
  }

  // ── sii ────────────────────────────────────────────────────────────────────────────────────
  reporter.start(kSiiStep);
  if (!package->sii) {
    reporter.succeed(kSiiStep, "the package carries no SII image");
  } else {
    auto written = deviceManager.withDevice(
        devicePosition, [&](Device& device) -> std::expected<void, std::string> {
          return device.writeSii(package->sii->content);
        });
    if (!written) {
      failure = std::format("writing the SII failed: {}", written.error());
      reporter.fail(kSiiStep, *failure);
      return finish();
    }
    // Unlike the firmware, an EEPROM change is not adopted by leaving BOOT: the ESC reads its
    // EEPROM at reset, so this one really does need a power cycle.
    reporter.succeed(kSiiStep,
                     std::format("{} written — power-cycle the device for it to take effect",
                                 package->sii->name));
  }

  // ── app-firmware / com-firmware ────────────────────────────────────────────────────────────
  // The two binaries differ only in which step they report against and what the bootloader calls
  // them, so one closure covers both. It returns false when the run must stop, which the caller
  // turns into the common exit — `failure` is already set by then.
  auto writeBinary = [&](std::string_view step, const std::optional<FirmwarePackageFile>& binary,
                         std::string_view foeName, std::string_view absent) {
    reporter.start(step);
    if (!binary) {
      reporter.succeed(step, std::string(absent));
      return true;
    }
    auto written = writeFileWithRetry(deviceManager, devicePosition, std::string(foeName),
                                      binary->content, stop);
    if (!written) {
      failure = std::format("writing {} failed: {}", binary->name, written.error());
      reporter.fail(step, *failure);
      return false;
    }
    reporter.succeed(step, std::format("{} written as {} ({} bytes)", binary->name, foeName,
                                       binary->content.size()));
    return true;
  };

  if (!writeBinary(kAppFirmwareStep, package->appBinary, kAppFirmwareFoeName,
                   "the package carries no application firmware, or it was skipped")) {
    return finish();
  }
  if (!writeBinary(kComFirmwareStep, package->comBinary, kComFirmwareFoeName,
                   "the package carries no COM firmware")) {
    return finish();
  }

  return finish();
}

}  // namespace mm::node
