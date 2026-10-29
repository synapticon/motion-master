#include "node/somanet_procedures.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "comm/al_status_codes.h"
#include "comm/foe_error.h"
#include "comm/sii.h"
#include "core/base64.h"
#include "core/platform.h"
#include "core/user_cache.h"
#include "node/firmware_version.h"
#include "node/ic_mu_bridge.h"
#include "node/ic_mu_encoder.h"
#ifdef MM_HAVE_MU_3SL
#include "node/ic_mu_calibration_engine.h"
#endif
#include "node/somanet_control.h"
#include "node/somanet_drive_objects.h"

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

// A bound on the requested ring velocity, which catches a typo; the plan caps it by 0x6080.
constexpr double kIcMuMaxRingVelocityRpm = 1000.0;

// The battery mode token that leaves the choice to the shaft the encoder measures.
constexpr std::string_view kDefaultBatteryMode = "default";

// Reading a counter takes one control cycle — a successful run finishes in about 20 ms — so this
// ceiling is not about how long the work takes. It clears the drive's own ~20 s reception timeout
// (measured at 20.04 s on an Integro), so a firmware that does not run the addressed service is
// allowed to answer for itself: OS error 253, which this decodes as "no such service running".
// Anything shorter aborts first and reports that this master gave up, which is true and useless.
constexpr auto kSkippedCyclesTimeout = std::chrono::seconds(30);

// Reading the whole object dictionary moves about 2 KB, and the drive writes 100 bytes per control
// cycle, so the transfer itself is over in around 25 ms. This ceiling clears the drive's own ~20 s
// reception timeout for the same reason kSkippedCyclesTimeout does: firmware older than 5.2 cannot
// hand over data while a command runs, and letting the drive answer for itself gives a real reason
// instead of "this master gave up".
constexpr auto kReadObjectDictionaryTimeout = std::chrono::seconds(30);
// Barely used: the transfer only finishes once the drive has sent everything, so the first poll
// after it normally reads a terminal status already.
constexpr auto kReadObjectDictionaryPollInterval = std::chrono::milliseconds(20);

// Same reasoning for the BiSS service commands whose precondition is "this encoder exists and is
// BiSS": only the service instance owning the addressed encoder answers, so an encoder that is
// neither is answered by nothing and the drive's own ~20 s timeout is the report. Clear it, and a
// 253 can be decoded into what actually went wrong.
constexpr auto kBissServiceTimeout = std::chrono::seconds(30);

// Each of command 22's three actions is a handful of assignments inside the drive control cycle, so
// a successful one finishes in about a cycle. The ceiling clears the drive's own ~20 s reception
// timeout for the same reason the counter read does: only the drive control service implements this
// command, so a firmware without it running is answered by nothing at all, and letting the drive
// report that itself is the difference between "no such service" and "this master gave up".
constexpr auto kFirmwareLatencyTimeout = std::chrono::seconds(30);

// Bounds on the observation window of a composite measurement. The floor is a handful of drive
// control cycles — anything shorter measures almost nothing — and the ceiling is what a single
// polled procedure run can reasonably occupy; a longer window is the start/read pair's job.
// Milliseconds because that is the unit both bounds are published in: the descriptor advertises
// .count() directly, so a ceiling declared in seconds would advertise 60 against a parse that
// accepts 60000.
constexpr auto kMinFirmwareLatencyDuration = std::chrono::milliseconds(100);
constexpr auto kMaxFirmwareLatencyDuration = std::chrono::milliseconds(60'000);

// How long to wait before a service told to stop counts as stopped. Short on purpose:
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

// Reads the "encoderOrdinal" field, which both encoder commands take and describe identically — the
// ordinal picks one of the drive's two encoder configuration objects and nothing else.
std::expected<somanet::EncoderOrdinal, std::string> readEncoderOrdinal(
    const nlohmann::json& body, somanet::EncoderOrdinal fallback) {
  auto ordinal = readByte(body, "encoderOrdinal", static_cast<uint8_t>(fallback));
  if (!ordinal) {
    return std::unexpected(ordinal.error());
  }
  // Only the two ordinals exist. A third one is rejected rather than passed through:
  // the drive would refuse it anyway, and refusing here says which values are real.
  if (*ordinal != static_cast<uint8_t>(somanet::EncoderOrdinal::kEncoder1) &&
      *ordinal != static_cast<uint8_t>(somanet::EncoderOrdinal::kEncoder2)) {
    return std::unexpected(std::format("'encoderOrdinal' must be 1 or 2, got {}", *ordinal));
  }
  return static_cast<somanet::EncoderOrdinal>(*ordinal);
}

// The "encoderOrdinal" parameter as every encoder command advertises it — one description, so two
// procedures cannot end up explaining the same field differently.
ProcedureParameter encoderParameter(std::optional<somanet::EncoderOrdinal> defaultValue) {
  return enumParameter(
      "encoderOrdinal", "Encoder ordinal",
      "Which of the drive's encoders to address. Encoder 1 is the encoder configured in 0x2110 and "
      "encoder 2 the one configured in 0x2112, so the ordinal picks a configuration object rather "
      "than a kind of encoder.",
      defaultValue ? nlohmann::json(static_cast<uint8_t>(*defaultValue)) : nlohmann::json(nullptr),
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

// Puts back everything a procedure changed on the drive, on every path out of the body — an early
// return, a failure, a cancellation — because a procedure that leaves the brake released and the
// drive in a mode it chose leaves the machine in a worse state than it found it.
//
// Inert until told what to restore, so a body that failed before changing anything restores nothing
// and does not claim a restore it never performed. Each restore is armed *before* the change it
// undoes, so a change that fails half-way is still unwound.
//
// It reports through kRestoreStep rather than only logging, because a restore that itself fails is
// precisely what a user needs to see.
class DriveRestorer {
 public:
  // @param procedure  Names the procedure in the log line a failed restore emits.
  DriveRestorer(SomanetDrive& drive, ProgressReporter& reporter, std::string_view procedure)
      : drive_(drive), reporter_(reporter), procedure_(procedure) {}

  DriveRestorer(const DriveRestorer&) = delete;
  DriveRestorer& operator=(const DriveRestorer&) = delete;

  // A destructor is implicitly noexcept, so this is a statement about the standard library rather
  // than about this code: the restore path builds strings and can therefore raise bad_alloc. There
  // is no better answer inside a destructor whose whole job is to put the drive back — swallowing
  // it would hide a failed restore.
  // NOLINTNEXTLINE(bugprone-exception-escape)
  ~DriveRestorer() {
    if (!mode_ && !brake_ && !disarmChirp_ && !profile_ && !targetVelocity_) {
      return;
    }
    reporter_.start(kRestoreStep);
    std::string failures;
    const auto note = [&failures](const std::string& what) {
      failures += failures.empty() ? what : "; " + what;
    };
    // The chirp first, so the drive holds no armed chirp for the rest of the restore or for a
    // later recording. Without a stop token: a cancelled run is the case this exists for, and its
    // token is already set.
    if (disarmChirp_) {
      if (auto r = drive_.setSystemIdentificationParameter(
              somanet::SystemIdentificationParameter::kStartProcedure,
              static_cast<uint32_t>(somanet::SystemIdentificationStart::kNone));
          !r) {
        note(std::format("failed to disarm the chirp: {}", r.error()));
      }
    }
    // Brake, then the state, then the mode — and each position is load-bearing.
    //
    // The brake has to go back before the state because it is only the master's to command while
    // the drive is still enabled, so undoing that first would strand it.
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
      if (resting_ == cia402::State::kSwitchOnDisabled) {
        if (auto r = drive_.disable(); !r) {
          note(std::format("failed to return the drive to Switch On Disabled: {}", r.error()));
        }
      } else if (auto r = drive_.transitionToState(resting_); !r) {
        note(std::format("failed to return the drive to {}: {}", cia402::toString(resting_),
                         r.error()));
      }
      if (auto r = drive_.setOperationModeValue(*mode_); !r) {
        note(std::format("failed to restore operation mode {}: {}", *mode_, r.error()));
      }
    }
    // The setpoints after the drive is disabled, so that none of them is followed on the way out.
    if (targetVelocity_) {
      if (auto r = drive_.setTargetVelocity(*targetVelocity_); !r) {
        note(std::format("failed to restore the target velocity: {}", r.error()));
      }
    }
    if (profile_) {
      if (auto r = drive_.setProfileAcceleration(profile_->first); !r) {
        note(std::format("failed to restore the profile acceleration: {}", r.error()));
      }
      if (auto r = drive_.setProfileDeceleration(profile_->second); !r) {
        note(std::format("failed to restore the profile deceleration: {}", r.error()));
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

  // Arms the restore of operation mode @p mode — and with it the return to @p resting, since the
  // only reason a procedure changes the mode is to enable the drive in it. The diagnostics
  // procedures rest in Switch On Disabled; plant identification rests in Switched On.
  void restoreMode(int8_t mode, cia402::State resting = cia402::State::kSwitchOnDisabled) {
    mode_ = mode;
    resting_ = resting;
  }

  // Arms the restore of brake status @p status.
  void restoreBrake(somanet::BrakeStatus status) { brake_ = status; }

  // Arms the disarm of the chirp.
  void disarmChirp() { disarmChirp_ = true; }

  // Arms the restore of the profile acceleration and deceleration, 0x6083 and 0x6084.
  void restoreProfile(uint32_t acceleration, uint32_t deceleration) {
    profile_ = std::pair{acceleration, deceleration};
  }

  // Arms the restore of the target velocity, 0x60FF.
  void restoreTargetVelocity(int32_t velocity) { targetVelocity_ = velocity; }

 private:
  SomanetDrive& drive_;
  ProgressReporter& reporter_;
  std::string_view procedure_;
  std::optional<int8_t> mode_;
  cia402::State resting_{cia402::State::kSwitchOnDisabled};
  std::optional<somanet::BrakeStatus> brake_;
  bool disarmChirp_ = false;
  std::optional<std::pair<uint32_t, uint32_t>> profile_;
  std::optional<int32_t> targetVelocity_;
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
                                                       DriveRestorer& restorer) {
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
                                                            DriveRestorer& restorer) {
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

  DriveRestorer restorer(*drive, reporter, spec.procedure);

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

constexpr std::string_view kPackageStep = "package";
constexpr std::string_view kCacheStep = "cache";
constexpr std::string_view kBootStep = "boot";
constexpr std::string_view kExtraFilesStep = "extra-files";
constexpr std::string_view kSiiStep = "sii";
constexpr std::string_view kAppFirmwareStep = "app-firmware";
constexpr std::string_view kComFirmwareStep = "com-firmware";
constexpr std::string_view kActivateFirmwareStep = "activate-firmware";
constexpr std::string_view kFinalStateStep = "final-state";

/// The names the bootloader is written under, regardless of what the package calls them.
///
/// Not a simplification: the bootloader has a fixed idea of the firmware filenames it accepts, and
/// a package's real name ("app_8500-04_motion-drive_v5.6.10_2332.bin") is both long and variable.
/// Sending the short constant name avoids filename-length handling in the bootloader entirely.
constexpr std::string_view kAppFirmwareFoeName = "app_firmware.bin";
constexpr std::string_view kComFirmwareFoeName = "com_firmware.bin";

/// Ceiling on an ordinary AL transition, which is a mailbox round trip and a slave acknowledgement.
constexpr auto kStateTimeout = std::chrono::seconds(10);

/// Ceiling on each of the two transitions out of BOOT. Far longer than an ordinary one because
/// neither is an ordinary one. The step to PRE-OP makes the SOMANET bootloader hand over to the
/// application by **restarting the device**, so it has to cover a reboot, plus the drive's own
/// internal timeout for deciding no valid application started (after which it answers AL status
/// 0x0014 rather than nothing). The step to INIT restarts the netX of an Integro that got a new
/// com binary: its boot code asks the netX for a reset 500 ms away and only then reports INIT, so
/// the master can miss INIT and must wait for the netX to come back (somanet_software,
/// AppECS_Functions_Boot.c, Boot_Handle_ESM_AL_Control_Changed_Ind).
constexpr auto kBootExitTimeout = std::chrono::seconds(30);

/// Ceiling on a firmware activation that restarts the device, as SPoE does: the restart loads the
/// COM firmware from flash before the drive answers again. A ceiling, not a wait: the activation
/// returns as soon as the drive answers.
constexpr auto kActivationTimeout = std::chrono::seconds(120);

/// How many times a transient FoE failure is re-issued before giving up, and the pause between
/// attempts. Only kinds that @c FoeError classifies as transient are retried; a missing file or an
/// undersized buffer will fail identically however many times it is asked. Motion Master v5 uses
/// the same 3 attempts and the same pause.
constexpr int kFoeAttempts = 3;
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
    // Resolved per attempt, so a rescan mid-install ends the run at the next step rather than
    // writing to a device that moved.
    const auto device = deviceManager.deviceAt(devicePosition);
    if (!device) {
      return std::unexpected(std::format("device {} not found", devicePosition));
    }
    const std::expected<void, FoeError> written = device->writeFile(filename, content);
    if (written) {
      return {};
    }
    const FoeError& error = written.error();
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
  // A freshly entered bootloader takes a moment before it services file operations. A first FoE
  // request sent too early waits out its timeout and is aborted, so this waits first. Motion
  // Master v5 waits the same 2 s.
  if (!sleepUnlessStopped(warmUp, stop)) {
    return std::unexpected(cancelled("while the bootloader started"));
  }
  return {};
}

/// Leaves BOOT for @p finalState, one legal AL step at a time.
///
/// The state machine only pairs BOOT with INIT and only climbs one step at a time, so the walk is
/// fixed: INIT, then PRE-OP, then SAFE-OP, then OP, stopping wherever it was asked to. The first
/// two steps each get the long timeout, because either can restart the device.
std::expected<void, std::string> leaveBoot(DeviceManager& deviceManager, uint16_t devicePosition,
                                           EtherCatState finalState) {
  if (finalState == EtherCatState::Boot) {
    return {};
  }
  // Long: on an Integro that got a com binary, this transition restarts the netX.
  if (auto r = transitionTo(deviceManager, devicePosition, EtherCatState::Init, kBootExitTimeout);
      !r) {
    return std::unexpected(std::format("could not reach INIT: {}", r.error()));
  }
  if (finalState == EtherCatState::Init) {
    return {};
  }
  // Long: this transition restarts the device into the firmware just written.
  if (auto r = transitionTo(deviceManager, devicePosition, EtherCatState::PreOp, kBootExitTimeout);
      !r) {
    return std::unexpected(std::format("could not reach PRE-OP: {}", r.error()));
  }
  if (finalState == EtherCatState::PreOp) {
    return {};
  }
  // Climbing further re-maps the whole bus, which DeviceManager handles — including re-reading this
  // device's PDO mapping, which the firmware just written can change.
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

  auto transfer = body.find("fsBuffer");
  if (transfer != body.end() && !transfer->is_null()) {
    if (!transfer->is_string()) {
      return std::unexpected("'fsBuffer' must be a string");
    }
    auto parsed = parseFsBufferTransfer(transfer->get<std::string>());
    if (!parsed) {
      return std::unexpected(
          std::format("'fsBuffer' must be {}, {} or {}", toString(FsBufferTransfer::kNone),
                      toString(FsBufferTransfer::kRead), toString(FsBufferTransfer::kWrite)));
    }
    request.fsBuffer.transfer = *parsed;
  }

  auto payload = body.find("fsBufferData");
  // An empty string counts as absent, not as an empty payload. A client that renders a file picker
  // for every parameter sends the field either way, and "" means the user picked nothing.
  const bool hasPayload = payload != body.end() && !payload->is_null() &&
                          !(payload->is_string() && payload->get<std::string>().empty());
  if (request.fsBuffer.transfer == FsBufferTransfer::kWrite) {
    if (!hasPayload) {
      return std::unexpected(std::format("'fsBufferData' is required when 'fsBuffer' is {}",
                                         toString(FsBufferTransfer::kWrite)));
    }
    if (!payload->is_string()) {
      return std::unexpected("'fsBufferData' must be a base64 string");
    }
    auto decoded = core::base64Decode(payload->get<std::string>());
    if (!decoded) {
      return std::unexpected(
          std::format("'fsBufferData' is not valid base64: {}", decoded.error()));
    }
    if (decoded->empty()) {
      return std::unexpected("'fsBufferData' decoded to no bytes at all");
    }
    request.fsBuffer.payload = std::move(*decoded);
  } else if (hasPayload) {
    // Refused rather than ignored: a caller who supplied bytes and left the direction at its
    // default asked for a transfer, and running the command without one looks like it worked.
    return std::unexpected(std::format("'fsBufferData' only applies when 'fsBuffer' is {}",
                                       toString(FsBufferTransfer::kWrite)));
  }
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
      enumParameter(
          "fsBuffer", "Bulk data",
          "Whether this command moves bulk data through the drive's fs-buffer, and which way. Most "
          "commands move none. A command that does is documented as doing so, and moving data it "
          "does not expect makes it wait for a transfer that never comes.",
          std::string(toString(defaults.fsBuffer.transfer)),
          {
              ParameterOption{.value = std::string(toString(FsBufferTransfer::kNone)),
                              .title = "None"},
              ParameterOption{.value = std::string(toString(FsBufferTransfer::kRead)),
                              .title = "Read from the drive"},
              ParameterOption{.value = std::string(toString(FsBufferTransfer::kWrite)),
                              .title = "Write to the drive"},
          }),
      fileParameter("fsBufferData", "Bulk data to write",
                    "The bytes to send, for a command that reads bulk data from the master. Leave "
                    "it empty for anything else.",
                    nlohmann::json("")),
  };
}

void to_json(nlohmann::json& j, const OsCommandResult& result) {
  j = nlohmann::json{{"status", result.status}, {"data", result.data}};
  if (result.errorCode) {
    j["errorCode"] = *result.errorCode;
  }
  if (!result.fsBuffer.empty()) {
    j["fsBuffer"] = result.fsBuffer;
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
      {.timeout = request.timeout, .pollInterval = request.pollInterval, .stop = std::move(stop)},
      request.fsBuffer);
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
                                                   .errorCode = response->errorCode,
                                                   .fsBuffer = response->fsBuffer});
  return {};
}

std::expected<EncoderRegisterRequest, std::string> parseEncoderRegisterRequest(
    const nlohmann::json& body) {
  if (!body.is_object()) {
    return std::unexpected("the request body must be a JSON object");
  }
  EncoderRegisterRequest request;

  auto encoder = readEncoderOrdinal(body, request.encoderOrdinal);
  if (!encoder) {
    return std::unexpected(encoder.error());
  }
  request.encoderOrdinal = *encoder;

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
      encoderParameter(defaults.encoderOrdinal),
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
  auto result =
      request.write
          ? drive->writeEncoderRegister(request.encoderOrdinal, request.registerAddress,
                                        request.value, config)
          : drive->readEncoderRegister(request.encoderOrdinal, request.registerAddress, config);
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

  auto encoder = readEncoderOrdinal(body, request.encoderOrdinal);
  if (!encoder) {
    return std::unexpected(encoder.error());
  }
  request.encoderOrdinal = *encoder;

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
      encoderParameter(defaults.encoderOrdinal),
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
  auto result = drive->setIcMuCalibrationMode(request.encoderOrdinal, request.mode,
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
                   nlohmann::json{{"encoderOrdinal", static_cast<uint8_t>(request.encoderOrdinal)},
                                  {"mode", somanet::toString(request.mode)}});
  return {};
}

namespace {

// Why the iC-MU procedures refuse @p version, or nothing when they can run on it.
std::optional<std::string> icMuFirmwareError(std::string_view version) {
  switch (somanet::icMuFirmwareSupport(version)) {
    case somanet::IcMuFirmwareSupport::kSupported:
    case somanet::IcMuFirmwareSupport::kJasper:
      return std::nullopt;
    case somanet::IcMuFirmwareSupport::kTooOld:
      return std::format(
          "firmware {} is too old: the iC-MU encoder procedures need 5.0.0-rc.5 or newer, or "
          "calibration firmware from 0.4.0",
          version);
    case somanet::IcMuFirmwareSupport::kUnreadable:
      break;
  }
  return std::format("firmware version '{}' in 0x100A is not a version", version);
}

// A restore file as it was written, and read back.
struct IcMuWrittenRestoreFile {
  std::string name;
  std::vector<uint8_t> bytes;
};

// Writes the file the drive writes to the encoder when the iC-MU reports a corrupted EEPROM, reads
// it back, and fails when the bytes differ. Requires somanet::isIntegratedSensorPort.
std::expected<IcMuWrittenRestoreFile, std::string> writeIcMuRestoreFile(
    Device& device, SomanetDrive& drive, somanet::EncoderOrdinal encoder, uint8_t sensorPort,
    const std::stop_token& stop) {
  std::vector<uint8_t> values;
  for (const uint8_t address : somanet::icMuRestoreRegisters()) {
    auto value = readIcMuRegister(drive, encoder, address, stop);
    if (!value) {
      return std::unexpected(value.error());
    }
    values.push_back(*value);
  }
  IcMuWrittenRestoreFile file{.name = somanet::icMuRestoreFileName(sensorPort),
                              .bytes = somanet::icMuRestoreFile(values)};
  if (auto written = device.writeFile(file.name, file.bytes); !written) {
    return std::unexpected(
        std::format("could not write '{}': {}", file.name, written.error().message));
  }
  auto readBack = device.readFile(file.name);
  if (!readBack) {
    return std::unexpected(
        std::format("could not read '{}' back: {}", file.name, readBack.error().message));
  }
  if (*readBack != file.bytes) {
    return std::unexpected(
        std::format("'{}' reads back {} bytes that differ from the {} bytes written", file.name,
                    readBack->size(), file.bytes.size()));
  }
  return file;
}

}  // namespace

std::vector<ProgressStep> icMuEncoderStatusSteps() {
  return stepsFrom({kIcMuEncoderStatusEncodersStep, kIcMuEncoderStatusRegistersStep});
}

std::expected<void, std::string> runIcMuEncoderStatusProcedure(Device& device,
                                                               ProgressReporter& reporter,
                                                               const std::stop_token& stop) {
  auto drive = createSomanetDrive(device);
  if (!drive) {
    return std::unexpected(drive.error());
  }

  reporter.start(kIcMuEncoderStatusEncodersStep);
  auto fail = [&reporter](std::string_view step, std::string error) {
    reporter.fail(step, error);
    return std::unexpected(std::move(error));
  };
  auto rawVersion = drive->manufacturerSoftwareVersion();
  if (!rawVersion) {
    return fail(kIcMuEncoderStatusEncodersStep, rawVersion.error());
  }
  const auto version = trimFirmwareVersion(
      std::span(reinterpret_cast<const uint8_t*>(rawVersion->data()), rawVersion->size()));
  if (auto error = icMuFirmwareError(version)) {
    return fail(kIcMuEncoderStatusEncodersStep, std::move(*error));
  }
  auto configurations = drive->encoders();
  if (!configurations) {
    return fail(kIcMuEncoderStatusEncodersStep, configurations.error());
  }
  std::vector<EncoderConfiguration> listed;
  std::ranges::copy_if(configurations->encoders, std::back_inserter(listed),
                       somanet::isIcMuEncoder);
  reporter.succeed(kIcMuEncoderStatusEncodersStep, listed);

  reporter.start(kIcMuEncoderStatusRegistersStep);
  std::vector<somanet::IcMuEncoderStatus> statuses;
  for (const auto& encoder : listed) {
    auto hardRev =
        readIcMuRegister(*drive, encoder.encoderOrdinal, somanet::kIcMuHardRevRegister, stop);
    if (!hardRev) {
      return fail(kIcMuEncoderStatusRegistersStep, hardRev.error());
    }
    auto output =
        readIcMuRegister(*drive, encoder.encoderOrdinal, somanet::kIcMuOutputRegister, stop);
    if (!output) {
      return fail(kIcMuEncoderStatusRegistersStep, output.error());
    }
    // An SDO read of what the drive latched, not a read of the chips: a read of STATUS1 clears the
    // iC-MU errors it would show.
    auto encoderStatus = encoder.encoderOrdinal == somanet::EncoderOrdinal::kEncoder1
                             ? device.readValue(somanet::objects::kEncoder1FeedbackEncoderStatus)
                             : device.readValue(somanet::objects::kEncoder2FeedbackEncoderStatus);
    if (!encoderStatus) {
      return fail(kIcMuEncoderStatusRegistersStep, encoderStatus.error());
    }
    auto status = somanet::icMuEncoderStatus(encoder, *hardRev, *output);
    status.encoderStatus = *encoderStatus;
    statuses.push_back(std::move(status));
  }
  reporter.succeed(kIcMuEncoderStatusRegistersStep, statuses);
  return {};
}

std::expected<IcMuEncoderConfigurationRequest, std::string> parseIcMuEncoderConfigurationRequest(
    const nlohmann::json& body) {
  if (!body.is_object()) {
    return std::unexpected("the request body must be a JSON object");
  }
  IcMuEncoderConfigurationRequest request;

  if (auto it = body.find("encoderOrdinal"); it == body.end() || it->is_null()) {
    return std::unexpected("'encoderOrdinal' is required");
  }
  auto encoder = readEncoderOrdinal(body, request.encoderOrdinal);
  if (!encoder) {
    return std::unexpected(encoder.error());
  }
  request.encoderOrdinal = *encoder;

  auto restoreFile = readBool(body, "writeRestoreFile", request.writeRestoreFile);
  if (!restoreFile) {
    return std::unexpected(restoreFile.error());
  }
  request.writeRestoreFile = *restoreFile;

  auto battery = body.find("batteryMode");
  if (battery == body.end() || battery->is_null()) {
    return request;
  }
  if (!battery->is_string()) {
    return std::unexpected("'batteryMode' must be a string");
  }
  const auto name = battery->get<std::string>();
  if (name == kDefaultBatteryMode) {
    return request;
  }
  const auto modes = somanet::icPvlBatteryModes();
  const auto mode = std::ranges::find(modes, name, &somanet::IcPvlBatteryMode::name);
  if (mode == modes.end()) {
    std::string names(kDefaultBatteryMode);
    for (const auto& m : modes) {
      names += std::format(", {}", m.name);
    }
    return std::unexpected(std::format("'batteryMode' must be one of {}, got '{}'", names, name));
  }
  request.batteryMode = mode->code;
  return request;
}

std::vector<ProcedureParameter> icMuEncoderConfigurationParameters() {
  std::vector<ParameterOption> batteryModes = {
      ParameterOption{.value = std::string(kDefaultBatteryMode), .title = "Default"}};
  for (const auto& mode : somanet::icPvlBatteryModes()) {
    batteryModes.push_back(ParameterOption{
        .value = std::string(mode.name), .title = std::format("{} µA", mode.maxCurrentMicroamps)});
  }
  return {
      encoderParameter(std::nullopt),
      enumParameter(
          "batteryMode", "Battery mode",
          "The current the iC-PVL draws from the battery while the drive has no power. A higher "
          "current lets it count revolutions at a higher acceleration. Default takes 0x5D for an "
          "encoder on the motor shaft and 0x68 for one on the driving shaft. Only the multiturn "
          "path takes a battery mode, and a ring of 16 master pole pairs takes only the default.",
          std::string(kDefaultBatteryMode), std::move(batteryModes)),
      booleanParameter("writeRestoreFile", "Write the restore file",
                       "Writes the configured registers to the restore file of the sensor port. "
                       "The drive writes that file back into the encoder when the iC-MU reports a "
                       "corrupted EEPROM. Leave it off to keep the file there now. The drive keeps "
                       "a restore file only for sensor ports 1 and 2.",
                       true),
  };
}

std::vector<ProgressStep> icMuEncoderConfigurationSteps() {
  return stepsFrom({kIcMuValidateStep, kIcMuIgnoreStatusBitsStep, kIcMuRegistersStep,
                    kIcPvlRegistersStep, kIcMuEepromWriteStep, kIcMuFaultResetStep,
                    kIcMuSynchronisationStep, kIcMuVerifyStep, kIcMuRestoreFileStep,
                    kIcMuRestoreStatusBitsStep});
}

namespace {

std::expected<void, std::string> waitForEncoder(std::chrono::milliseconds duration,
                                                const std::stop_token& stop) {
  if (!sleepUnlessStopped(duration, stop)) {
    return std::unexpected("the configuration was cancelled");
  }
  return {};
}

// What the validate step decides before anything is written.
struct IcMuPlan {
  EncoderConfiguration encoder;
  somanet::IcMuConfiguration configuration;
  std::vector<std::string> warnings;
};

std::expected<IcMuPlan, std::string> planIcMuConfiguration(
    Device& device, SomanetDrive& drive, const IcMuEncoderConfigurationRequest& request,
    const std::stop_token& stop) {
  auto rawVersion = drive.manufacturerSoftwareVersion();
  if (!rawVersion) {
    return std::unexpected(rawVersion.error());
  }
  const auto version = trimFirmwareVersion(
      std::span(reinterpret_cast<const uint8_t*>(rawVersion->data()), rawVersion->size()));
  if (auto error = icMuFirmwareError(version)) {
    return std::unexpected(std::move(*error));
  }
  const bool jasper =
      somanet::icMuFirmwareSupport(version) == somanet::IcMuFirmwareSupport::kJasper;

  auto configurations = drive.encoders();
  if (!configurations) {
    return std::unexpected(configurations.error());
  }
  const auto found = std::ranges::find(configurations->encoders, request.encoderOrdinal,
                                       &EncoderConfiguration::encoderOrdinal);
  if (found == configurations->encoders.end() || !somanet::isIcMuEncoder(*found)) {
    return std::unexpected(
        std::format("{} is not a BiSS or internal encoder with singleturn bits on a sensor port",
                    somanet::toString(request.encoderOrdinal)));
  }
  if (auto error = somanet::icMuSingleturnBitsError(found->singleturnBits)) {
    return std::unexpected(std::move(*error));
  }
  if (auto bank = checkIcMuBank(drive, request.encoderOrdinal, stop); !bank) {
    return std::unexpected(bank.error());
  }
  if (!somanet::isIntegratedSensorPort(found->sensorPort)) {
    auto hardRev =
        readIcMuRegister(drive, request.encoderOrdinal, somanet::kIcMuHardRevRegister, stop);
    if (!hardRev) {
      return std::unexpected(hardRev.error());
    }
    if (auto error = somanet::icMuPartError(found->sensorPort, *hardRev)) {
      return std::unexpected(std::move(*error));
    }
  }

  auto state = drive.state();
  if (!state) {
    return std::unexpected(state.error());
  }
  if (*state == cia402::State::kOperationEnabled) {
    return std::unexpected(
        "the drive is in Operation Enabled; disable it first, because the encoder stops reporting "
        "position while it is configured");
  }

  IcMuPlan plan{.encoder = *found, .configuration = {}, .warnings = {}};
  auto& configuration = plan.configuration;
  configuration.path = somanet::icMuPath(found->sensorPort, found->multiturnBits);
  configuration.singleturnBits = found->singleturnBits;
  configuration.multiturnBits = found->multiturnBits;
  if (auto warning = somanet::icMuMultiturnBitsWarning(configuration.path, found->multiturnBits)) {
    plan.warnings.push_back(std::move(*warning));
  }
  configuration.safeMotion = device
                                 .parameter(somanet::objects::kEncoderSourceTypeCount.index,
                                            somanet::objects::kEncoderSourceTypeCount.subindex)
                                 .has_value();

  if (configuration.path == somanet::IcMuPath::kSingleturn) {
    if (request.batteryMode) {
      return std::unexpected("'batteryMode' applies only to the multiturn path");
    }
    return plan;
  }
  if (jasper) {
    return std::unexpected(std::format(
        "firmware {} is Jasper firmware, which has no iC-PVL, so it cannot take the multiturn path",
        version));
  }
  if (request.batteryMode) {
    if (!somanet::icPvlMaxAcceleration(somanet::icPvlBatteryModes().front(),
                                       found->singleturnBits)) {
      return std::unexpected(
          std::format("a ring of {} master pole pairs takes only the default battery mode",
                      somanet::icMuMasterPolePairs(found->singleturnBits)));
    }
    configuration.batteryMode = *request.batteryMode;
    return plan;
  }
  auto gearRatio = drive.gearRatio();
  if (!gearRatio) {
    return std::unexpected(gearRatio.error());
  }
  const auto shaft = somanet::encoderShaft(*configurations, request.encoderOrdinal, *gearRatio);
  if (!shaft) {
    return std::unexpected(std::format(
        "{} serves no controller, so its shaft and its default battery mode are unknown; assign it "
        "to the commutation, position or velocity controller first, or name a battery mode",
        somanet::toString(request.encoderOrdinal)));
  }
  configuration.batteryMode = somanet::icPvlDefaultBatteryMode(*shaft);
  return plan;
}

nlohmann::json toJson(const IcMuPlan& plan) {
  const auto& c = plan.configuration;
  nlohmann::json j{
      {"encoderOrdinal", static_cast<int>(plan.encoder.encoderOrdinal)},
      {"sensorPort", plan.encoder.sensorPort},
      {"path", c.path == somanet::IcMuPath::kMultiturn ? "multiturn" : "singleturn"},
      {"singleturnBits", c.singleturnBits},
      {"multiturnBits", c.multiturnBits},
      {"outputPorts", somanet::icMuOutputPorts(c.safeMotion)},
      {"warnings", plan.warnings},
  };
  j["batteryMode"] = c.path == somanet::IcMuPath::kMultiturn ? nlohmann::json(c.batteryMode)
                                                             : nlohmann::json(nullptr);
  return j;
}

}  // namespace

std::expected<void, std::string> runIcMuEncoderConfigurationProcedure(
    Device& device, ProgressReporter& reporter, const std::stop_token& stop,
    const IcMuEncoderConfigurationRequest& request) {
  auto drive = createSomanetDrive(device);
  if (!drive) {
    return std::unexpected(drive.error());
  }
  auto fail = [&reporter](std::string_view step, std::string error) {
    reporter.fail(step, error);
    return std::unexpected(std::move(error));
  };
  const auto encoder = request.encoderOrdinal;
  const auto& waits = request.waits;
  const OsCommandConfig bissConfig{
      .timeout = kBissServiceTimeout, .pollInterval = kEncoderRegisterPollInterval, .stop = stop};

  reporter.start(kIcMuValidateStep);
  auto plan = planIcMuConfiguration(device, *drive, request, stop);
  if (!plan) {
    return fail(kIcMuValidateStep, plan.error());
  }
  reporter.succeed(kIcMuValidateStep, toJson(*plan));
  const auto& configuration = plan->configuration;
  const bool multiturn = configuration.path == somanet::IcMuPath::kMultiturn;

  reporter.start(kIcMuIgnoreStatusBitsStep);
  if (auto ignored = drive->setIgnoreBissStatusBits(encoder, true, bissConfig); !ignored) {
    return fail(kIcMuIgnoreStatusBitsStep, ignored.error());
  }
  reporter.succeed(kIcMuIgnoreStatusBitsStep);

  auto write = [&](std::span<const somanet::IcMuRegisterWrite> writes) {
    return writeIcMuRegisters(*drive, encoder, writes, stop);
  };
  auto command = [&](somanet::IcMuCommand c) {
    return writeIcMuRegister(*drive, encoder, icMuCommand(c), stop);
  };
  auto transfer = [&](std::span<const somanet::IcMuRegisterWrite> writes) {
    return runIcMuI2cTransfer(*drive, encoder, writes, stop);
  };
  auto save = [&]() { return saveIcMuEeprom(*drive, encoder, stop).transform([](uint8_t) {}); };
  auto read = [&](uint8_t address) { return readIcMuRegister(*drive, encoder, address, stop); };
  // SCLR clears the iC-PVL status, and ABS_RESET has the iC-MU read the revolution count again.
  auto clearAndReset = [&]() -> std::expected<void, std::string> {
    if (auto r = transfer(somanet::icPvlCommand(somanet::IcPvlCommand::kSclr)); !r) {
      return r;
    }
    return command(somanet::IcMuCommand::kAbsReset);
  };

  // Gives SPO_MT the offset between the revolution of the iC-MU and the SYNC bits of the iC-PVL,
  // then stores it like the first configuration.
  auto synchronise = [&]() -> std::expected<nlohmann::json, std::string> {
    auto mpc = read(somanet::kIcMuMpcRegister);
    if (!mpc) {
      return std::unexpected(mpc.error());
    }
    const auto cleared = static_cast<uint8_t>(*mpc & 0x0F);
    if (auto r = write(std::array{somanet::IcMuRegisterWrite{somanet::kIcMuMpcRegister, cleared}})
                     .and_then([&] { return command(somanet::IcMuCommand::kAbsReset); })
                     .and_then([&] { return waitForEncoder(waits.absReset, stop); });
        !r) {
      return std::unexpected(r.error());
    }
    auto rawPosition = encoder == somanet::EncoderOrdinal::kEncoder1
                           ? device.readValue(somanet::objects::kEncoder1FeedbackRawPosition)
                           : device.readValue(somanet::objects::kEncoder2FeedbackRawPosition);
    if (!rawPosition) {
      return std::unexpected(rawPosition.error());
    }
    if (auto r = transfer(somanet::icPvlRegisterWrite(0x05, somanet::kIcPvlPositionOutputOn))
                     .and_then([&] {
                       return transfer(somanet::icPvlRegisterRead(somanet::kIcPvlSyncRegister));
                     });
        !r) {
      return std::unexpected(r.error());
    }
    auto sync = read(somanet::kIcMuExchangeRegister);
    if (!sync) {
      return std::unexpected(sync.error());
    }
    const uint8_t spoMt =
        somanet::icMuSpoMtRegister(*rawPosition, configuration.singleturnBits, *sync);
    if (auto r = write(std::array{somanet::IcMuRegisterWrite{somanet::kIcMuMpcRegister, spoMt}})
                     .and_then([&] {
                       return transfer(
                           somanet::icPvlRegisterWrite(0x05, somanet::kIcPvlPositionOutputOff));
                     })
                     .and_then(save)
                     .and_then(clearAndReset);
        !r) {
      return std::unexpected(r.error());
    }
    return nlohmann::json{
        {"rawPosition", *rawPosition}, {"icPvlSync", *sync & 0x07}, {"mpcRegister", spoMt}};
  };

  // Reads back the RAM, which WRITE_ALL copied into the EEPROM without an error. A ring the chip
  // does not have shows in MPC, and an output word the drive does not expect shows in OUT_MSB.
  auto verify = [&]() -> std::expected<somanet::IcMuEncoderStatus, std::string> {
    auto mpc = read(somanet::kIcMuMpcRegister);
    if (!mpc) {
      return std::unexpected(mpc.error());
    }
    const uint8_t expectedMpc = somanet::icMuMasterPeriodCount(configuration.singleturnBits);
    if ((*mpc & 0x0F) != expectedMpc) {
      return std::unexpected(std::format(
          "register 0x0F holds MPC {} after the configuration, not {}", *mpc & 0x0F, expectedMpc));
    }
    auto hardRev = read(somanet::kIcMuHardRevRegister);
    if (!hardRev) {
      return std::unexpected(hardRev.error());
    }
    auto output = read(somanet::kIcMuOutputRegister);
    if (!output) {
      return std::unexpected(output.error());
    }
    auto status = somanet::icMuEncoderStatus(plan->encoder, *hardRev, *output);
    if (status.state != somanet::IcMuEncoderState::kConfigured) {
      return std::unexpected(
          std::format("register 0x11 holds 0x{:02X} after the configuration, not 0x{:02X}", *output,
                      status.expectedOutputRegister.value_or(0)));
    }
    return status;
  };

  // Every step that writes the encoder, so that the status bits come back on whatever it returns.
  auto configure = [&]() -> std::expected<void, std::string> {
    reporter.start(kIcMuRegistersStep);
    if (auto r = write(somanet::icMuRamWrites(configuration)); !r) {
      return fail(kIcMuRegistersStep, r.error());
    }
    reporter.succeed(kIcMuRegistersStep);

    if (multiturn) {
      reporter.start(kIcPvlRegistersStep);
      auto pvl = transfer(somanet::icPvlEepromWrites(configuration))
                     .and_then([&] { return waitForEncoder(waits.icPvlEeprom, stop); })
                     .and_then([&] {
                       return transfer(somanet::icPvlCommand(somanet::IcPvlCommand::kReboot));
                     })
                     .and_then([&] { return waitForEncoder(waits.icPvlReboot, stop); });
      if (!pvl) {
        return fail(kIcPvlRegistersStep, pvl.error());
      }
      reporter.succeed(kIcPvlRegistersStep);
    }

    reporter.start(kIcMuEepromWriteStep);
    auto eeprom = save().and_then([&]() -> std::expected<void, std::string> {
      return multiturn ? clearAndReset() : command(somanet::IcMuCommand::kAbsReset);
    });
    if (!eeprom) {
      return fail(kIcMuEepromWriteStep, eeprom.error());
    }
    reporter.succeed(kIcMuEepromWriteStep);

    if (multiturn) {
      // The iC-PVL reboot and the cleared status can leave the drive in a fault, and the
      // synchronisation reads the position through the drive.
      reporter.start(kIcMuFaultResetStep);
      auto statusword = drive->statusword();
      if (!statusword) {
        return fail(kIcMuFaultResetStep, statusword.error());
      }
      const bool faulted = cia402::isFaulted(*statusword);
      if (faulted) {
        if (auto reset = drive->faultReset(); !reset) {
          return fail(kIcMuFaultResetStep, reset.error());
        }
      }
      reporter.succeed(kIcMuFaultResetStep, nlohmann::json{{"faulted", faulted}});

      reporter.start(kIcMuSynchronisationStep);
      auto synchronised = synchronise();
      if (!synchronised) {
        return fail(kIcMuSynchronisationStep, synchronised.error());
      }
      reporter.succeed(kIcMuSynchronisationStep, *synchronised);
    }

    reporter.start(kIcMuVerifyStep);
    auto verified = verify();
    if (!verified) {
      return fail(kIcMuVerifyStep, verified.error());
    }
    reporter.succeed(kIcMuVerifyStep, *verified);

    reporter.start(kIcMuRestoreFileStep);
    if (!request.writeRestoreFile) {
      reporter.succeed(kIcMuRestoreFileStep,
                       nlohmann::json{{"file", nullptr}, {"reason", "the request left it out"}});
      return {};
    }
    if (!somanet::isIntegratedSensorPort(plan->encoder.sensorPort)) {
      // The drive reads a restore file only for ports 1 and 2, so a file for this port would never
      // be used.
      reporter.succeed(kIcMuRestoreFileStep,
                       nlohmann::json{{"file", nullptr},
                                      {"reason",
                                       "the drive keeps a restore file only for sensor "
                                       "ports 1 and 2"}});
      return {};
    }
    auto restored = writeIcMuRestoreFile(device, *drive, encoder, plan->encoder.sensorPort, stop);
    if (!restored) {
      return fail(kIcMuRestoreFileStep, restored.error());
    }
    reporter.succeed(kIcMuRestoreFileStep, nlohmann::json{{"file", restored->name}});
    return {};
  };

  auto configured = configure();

  reporter.start(kIcMuRestoreStatusBitsStep);
  // A cancelled run still turns the status bits on, so this command takes no stop token.
  auto restoredBits = drive->setIgnoreBissStatusBits(
      encoder, false,
      {.timeout = kBissServiceTimeout, .pollInterval = kEncoderRegisterPollInterval});
  if (!restoredBits) {
    reporter.fail(kIcMuRestoreStatusBitsStep, restoredBits.error());
  } else {
    reporter.succeed(kIcMuRestoreStatusBitsStep);
  }
  if (!configured) {
    return configured;
  }
  if (!restoredBits) {
    return std::unexpected(restoredBits.error());
  }
  return {};
}

std::expected<IcMuEncoderRequest, std::string> parseIcMuEncoderRequest(const nlohmann::json& body) {
  if (!body.is_object()) {
    return std::unexpected("the request body must be a JSON object");
  }
  if (auto it = body.find("encoderOrdinal"); it == body.end() || it->is_null()) {
    return std::unexpected("'encoderOrdinal' is required");
  }
  auto encoder = readEncoderOrdinal(body, somanet::EncoderOrdinal::kEncoder1);
  if (!encoder) {
    return std::unexpected(encoder.error());
  }
  return IcMuEncoderRequest{.encoderOrdinal = *encoder};
}

std::vector<ProcedureParameter> icMuEncoderParameters() { return {encoderParameter(std::nullopt)}; }

std::vector<ProgressStep> icMuKeyRegistersSteps() {
  return stepsFrom({kIcMuValidateStep, kIcMuKeyRegistersStep});
}

std::vector<ProgressStep> icMuEepromExportSteps() {
  return stepsFrom({kIcMuValidateStep, kIcMuIgnoreStatusBitsStep, kIcMuEepromReadStep,
                    kIcMuRestoreStatusBitsStep});
}

std::expected<IcMuI2cRegisterRequest, std::string> parseIcMuI2cRegisterRequest(
    const nlohmann::json& body) {
  auto encoder = parseIcMuEncoderRequest(body);
  if (!encoder) {
    return std::unexpected(encoder.error());
  }
  IcMuI2cRegisterRequest request{.encoderOrdinal = encoder->encoderOrdinal};

  auto target = body.find("target");
  if (target == body.end() || target->is_null()) {
    return std::unexpected("'target' is required");
  }
  const auto parsed =
      target->is_string() ? somanet::parseIcMuI2cTarget(target->get<std::string>()) : std::nullopt;
  if (!parsed) {
    return std::unexpected("'target' must be one of eeprom, ic-pvl, ic-pvl-status");
  }
  request.target = *parsed;

  auto write = readBool(body, "write", request.write);
  if (!write) {
    return std::unexpected(write.error());
  }
  request.write = *write;
  auto address = readByte(body, "registerAddress", std::nullopt);
  if (!address) {
    return std::unexpected(address.error());
  }
  request.address = *address;
  // A write needs its byte; a read ignores one.
  auto value = readByte(body, "value", request.write ? std::nullopt : std::optional<uint8_t>(0));
  if (!value) {
    return std::unexpected(value.error());
  }
  request.value = *value;
  return request;
}

std::vector<ProcedureParameter> icMuI2cRegisterParameters() {
  return {
      encoderParameter(std::nullopt),
      enumParameter("target", "Target",
                    "What the transfer addresses. eeprom is the EEPROM that holds the "
                    "configuration of the iC-MU and the iC-PVL. ic-pvl is every register of the "
                    "iC-PVL. ic-pvl-status is only the status and command registers of the "
                    "iC-PVL, where the command register is 0x01.",
                    nlohmann::json(nullptr),
                    {ParameterOption{.value = "eeprom", .title = "EEPROM"},
                     ParameterOption{.value = "ic-pvl", .title = "iC-PVL"},
                     ParameterOption{.value = "ic-pvl-status", .title = "iC-PVL status"}}),
      booleanParameter("write", "Write", "Writes the byte instead of reading it.", false),
      integerParameter("registerAddress", "Register address",
                       "The address on the target: an EEPROM address, or an iC-PVL register.",
                       nlohmann::json(nullptr), 0, 255),
      integerParameter("value", "Value", "The byte to write. A read ignores it.", 0, 0, 255),
  };
}

std::vector<ProgressStep> icMuI2cRegisterSteps() {
  return stepsFrom({kIcMuValidateStep, kIcMuIgnoreStatusBitsStep, kIcMuI2cTransferStep,
                    kIcMuRestoreStatusBitsStep});
}

namespace {

// Checks what every EEPROM tool needs, and returns the configuration of the encoder. The encoder
// must be one that ic-mu-encoder-status lists, and on an external port its register 0x74 must name
// an iC-MU part. A tool that uses the I²C bridge also refuses a drive in Operation Enabled,
// because the encoder reports no position while the status bits are off.
std::expected<EncoderConfiguration, std::string> planIcMuTool(SomanetDrive& drive,
                                                              somanet::EncoderOrdinal encoder,
                                                              bool bridge,
                                                              const std::stop_token& stop) {
  auto rawVersion = drive.manufacturerSoftwareVersion();
  if (!rawVersion) {
    return std::unexpected(rawVersion.error());
  }
  const auto version = trimFirmwareVersion(
      std::span(reinterpret_cast<const uint8_t*>(rawVersion->data()), rawVersion->size()));
  if (auto error = icMuFirmwareError(version)) {
    return std::unexpected(std::move(*error));
  }
  auto configurations = drive.encoders();
  if (!configurations) {
    return std::unexpected(configurations.error());
  }
  const auto found =
      std::ranges::find(configurations->encoders, encoder, &EncoderConfiguration::encoderOrdinal);
  if (found == configurations->encoders.end() || !somanet::isIcMuEncoder(*found)) {
    return std::unexpected(
        std::format("{} is not a BiSS or internal encoder with singleturn bits on a sensor port",
                    somanet::toString(encoder)));
  }
  auto hardRev = readIcMuRegister(drive, encoder, somanet::kIcMuHardRevRegister, stop);
  if (!hardRev) {
    return std::unexpected(hardRev.error());
  }
  if (auto error = somanet::icMuPartError(found->sensorPort, *hardRev)) {
    return std::unexpected(std::move(*error));
  }
  if (bridge) {
    auto state = drive.state();
    if (!state) {
      return std::unexpected(state.error());
    }
    if (*state == cia402::State::kOperationEnabled) {
      return std::unexpected(
          "the drive is in Operation Enabled; disable it first, because the encoder stops "
          "reporting position while its I²C bridge is in use");
    }
  }
  return *found;
}

// Runs @p body with the BiSS status bits of @p encoder off, because the drive refuses the bridge
// registers 0x5B-0x6F otherwise, and turns them on again on every way out.
template <typename Body>
std::expected<void, std::string> withStatusBitsIgnored(SomanetDrive& drive,
                                                       ProgressReporter& reporter,
                                                       somanet::EncoderOrdinal encoder,
                                                       const std::stop_token& stop, Body body) {
  reporter.start(kIcMuIgnoreStatusBitsStep);
  if (auto ignored = drive.setIgnoreBissStatusBits(encoder, true,
                                                   {.timeout = kBissServiceTimeout,
                                                    .pollInterval = kEncoderRegisterPollInterval,
                                                    .stop = stop});
      !ignored) {
    reporter.fail(kIcMuIgnoreStatusBitsStep, ignored.error());
    return std::unexpected(ignored.error());
  }
  reporter.succeed(kIcMuIgnoreStatusBitsStep);

  std::expected<void, std::string> result = body();

  reporter.start(kIcMuRestoreStatusBitsStep);
  // A failed I²C transfer leaves EPR_ERR set until a transfer succeeds, and a read of STATUS1 does
  // not clear it. With the status bits on again, the drive takes it for a BiSS error and faults. A
  // one-byte read of the EEPROM clears it. These reads take no stop token, like the restore.
  if (auto status1 = readIcMuRegister(drive, encoder, kIcMuStatus1Register, {});
      status1 && (*status1 & kIcMuEepromError) != 0) {
    if (auto cleared = readIcMuI2c(drive, encoder, somanet::IcMuI2cTarget::kEeprom, 0x00, 1, {});
        !cleared) {
      const auto warning = std::format(
          "EPR_ERR could not be cleared, so the drive may fault when the status bits are on again: "
          "{}",
          cleared.error());
      result = result ? std::unexpected(warning)
                      : std::unexpected(std::format("{}; {}", result.error(), warning));
    }
  }
  // A cancelled run still turns the status bits on, so this command takes no stop token.
  auto restored = drive.setIgnoreBissStatusBits(
      encoder, false,
      {.timeout = kBissServiceTimeout, .pollInterval = kEncoderRegisterPollInterval});
  if (!restored) {
    reporter.fail(kIcMuRestoreStatusBitsStep, restored.error());
  } else {
    reporter.succeed(kIcMuRestoreStatusBitsStep);
  }
  if (!result) {
    return result;
  }
  if (!restored) {
    return std::unexpected(restored.error());
  }
  return {};
}

nlohmann::json toolPlan(const EncoderConfiguration& encoder) {
  return nlohmann::json{{"encoderOrdinal", static_cast<int>(encoder.encoderOrdinal)},
                        {"sensorPort", encoder.sensorPort}};
}

}  // namespace

std::expected<void, std::string> runIcMuKeyRegistersProcedure(Device& device,
                                                              ProgressReporter& reporter,
                                                              const std::stop_token& stop,
                                                              const IcMuEncoderRequest& request) {
  auto drive = createSomanetDrive(device);
  if (!drive) {
    return std::unexpected(drive.error());
  }
  auto fail = [&reporter](std::string_view step, std::string error) {
    reporter.fail(step, error);
    return std::unexpected(std::move(error));
  };
  const auto encoder = request.encoderOrdinal;

  reporter.start(kIcMuValidateStep);
  auto plan = planIcMuTool(*drive, encoder, false, stop);
  if (!plan) {
    return fail(kIcMuValidateStep, plan.error());
  }
  // The key registers 0x00-0x18 are read through the bank BANKSEL selects.
  if (auto bank = checkIcMuBank(*drive, encoder, stop); !bank) {
    return fail(kIcMuValidateStep, bank.error());
  }
  reporter.succeed(kIcMuValidateStep, toolPlan(*plan));

  reporter.start(kIcMuKeyRegistersStep);
  std::vector<somanet::IcMuKeyRegister> registers;
  for (const uint8_t ram : somanet::icMuRestoreRegisters()) {
    auto value = readIcMuRegister(*drive, encoder, ram, stop);
    if (!value) {
      return fail(kIcMuKeyRegistersStep, value.error());
    }
    registers.push_back({.ramAddress = ram,
                         .eepromAddress = somanet::icMuEepromAddress(ram).value_or(0),
                         .value = *value});
  }
  reporter.succeed(kIcMuKeyRegistersStep, registers);
  return {};
}

std::expected<void, std::string> runIcMuEepromExportProcedure(Device& device,
                                                              ProgressReporter& reporter,
                                                              const std::stop_token& stop,
                                                              const IcMuEncoderRequest& request) {
  auto drive = createSomanetDrive(device);
  if (!drive) {
    return std::unexpected(drive.error());
  }
  const auto encoder = request.encoderOrdinal;

  reporter.start(kIcMuValidateStep);
  auto plan = planIcMuTool(*drive, encoder, true, stop);
  if (!plan) {
    reporter.fail(kIcMuValidateStep, plan.error());
    return std::unexpected(plan.error());
  }
  reporter.succeed(kIcMuValidateStep, toolPlan(*plan));

  return withStatusBitsIgnored(
      *drive, reporter, encoder, stop, [&]() -> std::expected<void, std::string> {
        reporter.start(kIcMuEepromReadStep);
        std::vector<somanet::IcMuEepromByte> bytes;
        for (const auto& chunk :
             somanet::icMuEepromReadChunks(0x00, somanet::kIcMuEepromConfigurationEnd)) {
          auto read = readIcMuI2c(*drive, encoder, somanet::IcMuI2cTarget::kEeprom,
                                  chunk.deviceStart, chunk.count, stop);
          if (!read) {
            reporter.fail(kIcMuEepromReadStep, read.error());
            return std::unexpected(read.error());
          }
          uint8_t address = chunk.deviceStart;
          for (const uint8_t value : *read) {
            bytes.push_back({.address = address++, .value = value});
          }
        }
        reporter.succeed(kIcMuEepromReadStep, bytes);
        return {};
      });
}

std::expected<void, std::string> runIcMuI2cRegisterProcedure(
    Device& device, ProgressReporter& reporter, const std::stop_token& stop,
    const IcMuI2cRegisterRequest& request) {
  auto drive = createSomanetDrive(device);
  if (!drive) {
    return std::unexpected(drive.error());
  }
  const auto encoder = request.encoderOrdinal;

  reporter.start(kIcMuValidateStep);
  auto plan = planIcMuTool(*drive, encoder, true, stop);
  if (!plan) {
    reporter.fail(kIcMuValidateStep, plan.error());
    return std::unexpected(plan.error());
  }
  reporter.succeed(kIcMuValidateStep, toolPlan(*plan));

  return withStatusBitsIgnored(
      *drive, reporter, encoder, stop, [&]() -> std::expected<void, std::string> {
        reporter.start(kIcMuI2cTransferStep);
        uint8_t value = request.value;
        std::expected<void, std::string> transfer;
        if (request.write) {
          transfer = writeIcMuI2c(*drive, encoder, request.target, request.address,
                                  std::span(&value, 1), stop);
        } else {
          transfer = readIcMuI2c(*drive, encoder, request.target, request.address, 1, stop)
                         .transform([&](const std::vector<uint8_t>& bytes) { value = bytes[0]; });
        }
        if (!transfer) {
          reporter.fail(kIcMuI2cTransferStep, transfer.error());
          return transfer;
        }
        reporter.succeed(
            kIcMuI2cTransferStep,
            nlohmann::json{{"target", std::string(somanet::toString(request.target))},
                           {"deviceId", somanet::icMuI2cDeviceId(request.target, request.write)},
                           {"write", request.write},
                           {"registerAddress", request.address},
                           {"value", value}});
        return {};
      });
}

std::vector<ProgressStep> icMuRestoreFileSteps() {
  return stepsFrom({kIcMuValidateStep, kIcMuRestoreFileStep});
}

std::vector<ProgressStep> icMuEepromEraseSteps() {
  return stepsFrom({kIcMuValidateStep, kIcMuIgnoreStatusBitsStep, kIcMuEepromEraseStep,
                    kIcMuEepromReadStep, kIcMuDeleteRestoreFileStep, kIcMuRestoreStatusBitsStep});
}

std::expected<void, std::string> runIcMuRestoreFileProcedure(Device& device,
                                                             ProgressReporter& reporter,
                                                             const std::stop_token& stop,
                                                             const IcMuEncoderRequest& request) {
  auto drive = createSomanetDrive(device);
  if (!drive) {
    return std::unexpected(drive.error());
  }
  auto fail = [&reporter](std::string_view step, std::string error) {
    reporter.fail(step, error);
    return std::unexpected(std::move(error));
  };
  const auto encoder = request.encoderOrdinal;

  reporter.start(kIcMuValidateStep);
  auto plan = planIcMuTool(*drive, encoder, false, stop);
  if (!plan) {
    return fail(kIcMuValidateStep, plan.error());
  }
  if (!somanet::isIntegratedSensorPort(plan->sensorPort)) {
    return fail(kIcMuValidateStep,
                std::format("the drive keeps a restore file only for sensor ports 1 and 2, and {} "
                            "is on sensor port {}",
                            somanet::toString(encoder), plan->sensorPort));
  }
  if (auto bank = checkIcMuBank(*drive, encoder, stop); !bank) {
    return fail(kIcMuValidateStep, bank.error());
  }
  reporter.succeed(kIcMuValidateStep, toolPlan(*plan));

  reporter.start(kIcMuRestoreFileStep);
  auto file = writeIcMuRestoreFile(device, *drive, encoder, plan->sensorPort, stop);
  if (!file) {
    return fail(kIcMuRestoreFileStep, file.error());
  }
  auto entries = somanet::parseIcMuRestoreFile(file->bytes);
  if (!entries) {
    return fail(kIcMuRestoreFileStep, entries.error());
  }
  reporter.succeed(kIcMuRestoreFileStep,
                   nlohmann::json{{"file", file->name}, {"entries", *entries}});
  return {};
}

std::expected<void, std::string> runIcMuEepromEraseProcedure(Device& device,
                                                             ProgressReporter& reporter,
                                                             const std::stop_token& stop,
                                                             const IcMuEncoderRequest& request) {
  auto drive = createSomanetDrive(device);
  if (!drive) {
    return std::unexpected(drive.error());
  }
  const auto encoder = request.encoderOrdinal;

  reporter.start(kIcMuValidateStep);
  auto plan = planIcMuTool(*drive, encoder, true, stop);
  if (!plan) {
    reporter.fail(kIcMuValidateStep, plan.error());
    return std::unexpected(plan.error());
  }
  reporter.succeed(kIcMuValidateStep, toolPlan(*plan));

  return withStatusBitsIgnored(
      *drive, reporter, encoder, stop, [&]() -> std::expected<void, std::string> {
        auto fail = [&reporter](std::string_view step, std::string error) {
          reporter.fail(step, error);
          return std::unexpected(std::move(error));
        };

        reporter.start(kIcMuEepromEraseStep);
        for (const auto& chunk :
             somanet::icMuEepromWriteChunks(0x00, somanet::kIcMuEepromConfigurationEnd)) {
          const std::vector<uint8_t> erased(chunk.count, 0xFF);
          if (auto written = writeIcMuI2c(*drive, encoder, somanet::IcMuI2cTarget::kEeprom,
                                          chunk.deviceStart, erased, stop);
              !written) {
            return fail(kIcMuEepromEraseStep, written.error());
          }
        }
        reporter.succeed(kIcMuEepromEraseStep);

        reporter.start(kIcMuEepromReadStep);
        std::vector<somanet::IcMuEepromByte> bytes;
        for (const auto& chunk :
             somanet::icMuEepromReadChunks(0x00, somanet::kIcMuEepromConfigurationEnd)) {
          auto read = readIcMuI2c(*drive, encoder, somanet::IcMuI2cTarget::kEeprom,
                                  chunk.deviceStart, chunk.count, stop);
          if (!read) {
            return fail(kIcMuEepromReadStep, read.error());
          }
          uint8_t address = chunk.deviceStart;
          for (const uint8_t value : *read) {
            bytes.push_back({.address = address++, .value = value});
          }
        }
        const auto kept =
            std::ranges::find_if(bytes, [](const auto& b) { return b.value != 0xFF; });
        if (kept != bytes.end()) {
          return fail(kIcMuEepromReadStep,
                      std::format("EEPROM 0x{:02X} reads 0x{:02X} after the erase, not 0xFF",
                                  kept->address, kept->value));
        }
        reporter.succeed(kIcMuEepromReadStep, bytes);

        reporter.start(kIcMuDeleteRestoreFileStep);
        if (!somanet::isIntegratedSensorPort(plan->sensorPort)) {
          reporter.succeed(kIcMuDeleteRestoreFileStep,
                           nlohmann::json{{"file", nullptr},
                                          {"reason",
                                           "the drive keeps a restore file only for sensor "
                                           "ports 1 and 2"}});
          return {};
        }
        const auto name = somanet::icMuRestoreFileName(plan->sensorPort);
        if (auto removed = removeDeviceFile(device, name); !removed) {
          return fail(kIcMuDeleteRestoreFileStep, removed.error());
        }
        reporter.succeed(kIcMuDeleteRestoreFileStep, nlohmann::json{{"file", name}});
        return {};
      });
}

std::string icMuIterationStep(int iteration) { return std::format("iteration-{}", iteration); }

std::expected<IcMuCalibrationRequest, std::string> parseIcMuCalibrationRequest(
    const nlohmann::json& body) {
  if (!body.is_object()) {
    return std::unexpected("the request body must be a JSON object");
  }
  IcMuCalibrationRequest request;
  if (auto it = body.find("encoderOrdinal"); it == body.end() || it->is_null()) {
    return std::unexpected("'encoderOrdinal' is required");
  }
  auto encoder = readEncoderOrdinal(body, request.encoderOrdinal);
  if (!encoder) {
    return std::unexpected(encoder.error());
  }
  request.encoderOrdinal = *encoder;

  if (auto it = body.find("ringPosition"); it != body.end() && !it->is_null()) {
    const auto position =
        it->is_string() ? somanet::parseIcMuRingPosition(it->get<std::string>()) : std::nullopt;
    if (!position) {
      return std::unexpected("'ringPosition' must be inner or outer");
    }
    request.ringPosition = position;
  }
  auto measurementOnly = readBool(body, "measurementOnly", request.measurementOnly);
  if (!measurementOnly) {
    return std::unexpected(measurementOnly.error());
  }
  request.measurementOnly = *measurementOnly;
  auto health = readBool(body, "activateHealthMonitoring", request.activateHealthMonitoring);
  if (!health) {
    return std::unexpected(health.error());
  }
  request.activateHealthMonitoring = *health;

  if (auto it = body.find("ringVelocity"); it != body.end() && !it->is_null()) {
    if (!it->is_number() || it->get<double>() <= 0 || it->get<double>() > kIcMuMaxRingVelocityRpm) {
      return std::unexpected(
          std::format("'ringVelocity' must be a number of rpm above 0 and at most {:.0f}",
                      kIcMuMaxRingVelocityRpm));
    }
    request.ringVelocityRpm = it->get<double>();
  }
  return request;
}

std::vector<ProcedureParameter> icMuCalibrationParameters() {
  return {
      encoderParameter(std::nullopt),
      enumParameter("ringPosition", "Ring position",
                    "Where the ring of an external encoder sits, inner or outer. Sensor port 1 is "
                    "always inner and port 2 always outer, so a request for those ports leaves it "
                    "out. The calibration starts from the values measured for that position.",
                    // Empty, not null: the field is optional, and only an external port needs it.
                    nlohmann::json(""),
                    {ParameterOption{.value = "inner", .title = "Inner"},
                     ParameterOption{.value = "outer", .title = "Outer"}}),
      booleanParameter("measurementOnly", "Validation only",
                       "Runs one validation pass, which reports the phase margins of the "
                       "calibration the encoder holds, and writes nothing to the encoder.",
                       false),
      booleanParameter("activateHealthMonitoring", "Health monitoring",
                       "After a full calibration, shows the NON_CTR error of the iC-MU in the "
                       "BiSS error bit, register 0x0C = 0x2C.",
                       false),
      integerParameter("ringVelocity", "Ring velocity (rpm)",
                       "The velocity of the ring of the encoder. Without it a motor-shaft encoder "
                       "runs at the velocity measured for its ring and a driving-shaft encoder at "
                       "15 rpm.",
                       nlohmann::json(""), 1, static_cast<int64_t>(kIcMuMaxRingVelocityRpm)),
  };
}

std::vector<ProgressStep> icMuCalibrationSteps() {
  std::vector<std::string> ids = {std::string(kIcMuValidateStep), std::string(kPrepareStep),
                                  std::string(kIcMuIgnoreStatusBitsStep),
                                  std::string(kIcMuStartValuesStep), std::string(kIcMuRawModeStep)};
  for (int iteration = 1; iteration <= somanet::kIcMuMaxIterations; ++iteration) {
    ids.push_back(icMuIterationStep(iteration));
  }
  for (const auto id : {kIcMuValidationStep, kIcMuStandardModeStep, kIcMuSaveStep,
                        kIcMuAbsResetStep, kIcMuRestoreFileStep, kIcMuReturnStep,
                        kIcMuRestoreEncoderStep, kIcMuRestoreStatusBitsStep, kRestoreStep}) {
    ids.emplace_back(id);
  }
  std::vector<ProgressStep> steps;
  for (auto& id : ids) {
    ProgressStep step;
    step.id = std::move(id);
    steps.push_back(std::move(step));
  }
  return steps;
}

#ifdef MM_HAVE_MU_3SL

namespace {

// Register 0x10 holds the multiturn interface. An inner encoder with multiturn bits has its cyclic
// check, CHK_MT in bit 6, turned off during the calibration; the other fields keep their values.
constexpr uint8_t kIcMuMultiturnInterfaceRegister = 0x10;
constexpr uint8_t kIcMuMultiturnCheck = 0x40;

// Raw mode: the output word ends at bit 27 and carries the two tracks.
constexpr uint8_t kIcMuModeRegister = 0x12;
constexpr uint8_t kIcMuRawOutMsb = 0x0E;
constexpr uint8_t kIcMuRawMode = 0x20;

// Register 0x0C with the NON_CTR error shown in the BiSS error bit.
constexpr uint8_t kIcMuHealthMonitoring = 0x2C;

// The validation fails below this phase margin, in percent.
constexpr double kIcMuMinMarginPercent = 10.0;

// The position may move by this many counts of the position encoder in an ABS_RESET without a
// warning.
constexpr double kIcMuAbsResetJump = 4096.0;

// The recording starts this long before the velocity is reached, because the start command takes
// a mailbox round trip.
constexpr double kIcMuRecordingLead = 0.02;

// A curve in a step value keeps at most this many points, so that a fast recording does not make
// a snapshot of megabytes.
constexpr size_t kIcMuCurvePoints = 720;

// Moves to a position: the velocity profile of the configuration in use today.
constexpr double kMoveAccelerationRpmPerSecond = 30.0;
constexpr double kMoveMinVelocityRpm = 3.0;
constexpr double kMoveMaxVelocityRpm = 120.0;
constexpr auto kMoveStationaryTimeout = std::chrono::seconds(10);

// The units of one drive, and the conversions between them and revolutions of the driving shaft.
struct IcMuUnits {
  double velocityPerRpm = 1;         // 0x60FF units per rpm of the driving shaft.
  double positionPerRevolution = 1;  // 0x6064 units per revolution of the driving shaft.
  uint32_t positionEncoderResolution = 0;
  double feedRatio = 1;

  int32_t velocity(double speedRpm) const {
    return static_cast<int32_t>(std::lround(speedRpm * velocityPerRpm * feedRatio));
  }
  uint32_t acceleration(double rpmPerSecond) const {
    return static_cast<uint32_t>(std::lround(rpmPerSecond * velocityPerRpm * feedRatio));
  }
  double revolutions(int64_t units) const {
    return static_cast<double>(units) / positionPerRevolution;
  }
  int64_t position(double turns) const { return std::llround(turns * positionPerRevolution); }
};

// Everything validate decides before the motor moves.
struct IcMuCalibrationPlan {
  EncoderConfiguration encoder;
  Encoders encoders;
  uint8_t hardRev = 0;
  uint8_t masterPolePairs = 0;
  somanet::IcMuRingPosition ringPosition{somanet::IcMuRingPosition::kInner};
  somanet::IcMuStartValues startValues;
  somanet::EncoderShaft shaft{somanet::EncoderShaft::kMotor};
  IcMuUnits units;
  somanet::IcMuMotionPlan motion;
  SoftwarePositionLimit limits;
  int32_t startPosition = 0;
  double gearRatio = 1;
  double maxVelocityRpm = 0;  // On the driving shaft.
  bool multiturnCheckOff = false;
  std::vector<std::string> warnings;
};

nlohmann::json toJson(const IcMuCalibrationPlan& plan) {
  return nlohmann::json{
      {"encoderOrdinal", static_cast<int>(plan.encoder.encoderOrdinal)},
      {"sensorPort", plan.encoder.sensorPort},
      {"ringPosition", somanet::toString(plan.ringPosition)},
      {"part", somanet::icMuPartName(plan.hardRev).value_or("unknown")},
      {"masterPolePairs", plan.masterPolePairs},
      {"shaft", somanet::toString(plan.shaft)},
      {"startValuesMeasured", plan.startValues.measured},
      {"velocityRpm", plan.motion.velocityRpm},
      {"accelerationRpmPerSecond", plan.motion.accelerationRpmPerSecond},
      {"recordingMs", plan.motion.recording.count()},
      {"limitedRange", plan.motion.limitedRange},
      {"warnings", plan.warnings},
  };
}

std::string refuse(std::string_view why) { return std::string(why); }

std::expected<IcMuCalibrationPlan, std::string> planIcMuCalibration(
    Device& device, SomanetDrive& drive, const IcMuCalibrationRequest& request,
    const std::stop_token& stop) {
  IcMuCalibrationPlan plan;

  auto rawVersion = drive.manufacturerSoftwareVersion();
  if (!rawVersion) {
    return std::unexpected(rawVersion.error());
  }
  const auto version = trimFirmwareVersion(
      std::span(reinterpret_cast<const uint8_t*>(rawVersion->data()), rawVersion->size()));
  if (auto error = icMuFirmwareError(version)) {
    return std::unexpected(std::move(*error));
  }

  auto state = drive.state();
  if (!state) {
    return std::unexpected(state.error());
  }
  if (*state == cia402::State::kOperationEnabled) {
    return std::unexpected(
        refuse("the drive is in Operation Enabled; disable it first, because the calibration "
               "enables it in profile velocity mode"));
  }

  auto configurations = drive.encoders();
  if (!configurations) {
    return std::unexpected(configurations.error());
  }
  plan.encoders = *configurations;
  const auto found = std::ranges::find(configurations->encoders, request.encoderOrdinal,
                                       &EncoderConfiguration::encoderOrdinal);
  if (found == configurations->encoders.end() || !somanet::isIcMuEncoder(*found)) {
    return std::unexpected(
        std::format("{} is not a BiSS or internal encoder with singleturn bits on a sensor port",
                    somanet::toString(request.encoderOrdinal)));
  }
  plan.encoder = *found;
  if (auto error = somanet::icMuSingleturnBitsError(found->singleturnBits)) {
    return std::unexpected(std::move(*error));
  }
  plan.masterPolePairs = somanet::icMuMasterPolePairs(found->singleturnBits);
  if (auto bank = checkIcMuBank(drive, request.encoderOrdinal, stop); !bank) {
    return std::unexpected(bank.error());
  }

  auto hardRev =
      readIcMuRegister(drive, request.encoderOrdinal, somanet::kIcMuHardRevRegister, stop);
  if (!hardRev) {
    return std::unexpected(hardRev.error());
  }
  plan.hardRev = *hardRev;
  if (auto error = somanet::icMuPartError(found->sensorPort, *hardRev)) {
    return std::unexpected(std::move(*error));
  }
  const auto partClass = somanet::icMuPartClass(*hardRev);
  if (!partClass) {
    return std::unexpected(
        std::format("the calibration has no start values for the {} (register 0x74 = 0x{:02X})",
                    somanet::icMuPartName(*hardRev).value_or("part"), *hardRev));
  }

  if (const auto portPosition = somanet::icMuRingPositionOfPort(found->sensorPort)) {
    if (request.ringPosition) {
      return std::unexpected(
          std::format("'ringPosition' is refused for sensor port {}, which is always {}",
                      found->sensorPort, somanet::toString(*portPosition)));
    }
    plan.ringPosition = *portPosition;
  } else if (request.ringPosition) {
    plan.ringPosition = *request.ringPosition;
  } else {
    return std::unexpected(
        std::format("the encoder on external sensor port {} needs 'ringPosition', inner or outer",
                    found->sensorPort));
  }

  const auto startValues =
      somanet::icMuStartValues(plan.masterPolePairs, plan.ringPosition, *partClass);
  if (!startValues) {
    return std::unexpected(
        std::format("no start values exist for an {} with {} master pole pairs at the {} position",
                    somanet::icMuPartName(*hardRev).value_or("iC-MU"), plan.masterPolePairs,
                    somanet::toString(plan.ringPosition)));
  }
  plan.startValues = *startValues;
  if (!startValues->measured) {
    plan.warnings.push_back(std::format(
        "The start values for an {} with {} master pole pairs at the {} position were not "
        "measured; the calibration uses those of another ring.",
        somanet::icMuPartName(*hardRev).value_or("iC-MU"), plan.masterPolePairs,
        somanet::toString(plan.ringPosition)));
  }

  // A safety module that monitors an encoder trips a safe state when the encoder changes.
  if (device.parameter(somanet::objects::kEncoderSourceType.index,
                       somanet::objects::kEncoderSourceType.subindex)) {
    auto source = device.readValue(somanet::objects::kEncoderSourceType);
    if (!source) {
      return std::unexpected(source.error());
    }
    if (*source != 0) {
      return std::unexpected(
          "the safety module monitors an encoder (0x2630:01 is not 0); set its encoder source "
          "type to none before the calibration");
    }
  }

  if (!configurations->commutation) {
    return std::unexpected("no encoder is the commutation source (0x2010:0C)");
  }
  const bool role = configurations->position == request.encoderOrdinal ||
                    configurations->velocity == request.encoderOrdinal;
  if (!role) {
    return std::unexpected(std::format(
        "{} is neither the position source (0x2012:09) nor the velocity source (0x2011:05)",
        somanet::toString(request.encoderOrdinal)));
  }

  auto modes = drive.supportedDriveModes();
  if (!modes) {
    return std::unexpected(modes.error());
  }
  if ((*modes & (1U << 2)) == 0) {
    return std::unexpected("the drive does not support profile velocity mode (0x6502 bit 2)");
  }

  auto gear = drive.gearRatio();
  if (!gear) {
    return std::unexpected(gear.error());
  }
  plan.gearRatio = gear->motorRevolutions == 0 || gear->shaftRevolutions == 0
                       ? 1.0
                       : static_cast<double>(gear->motorRevolutions) / gear->shaftRevolutions;
  plan.gearRatio = std::max(plan.gearRatio, 1.0);
  plan.shaft = somanet::encoderShaft(*configurations, request.encoderOrdinal, *gear)
                   .value_or(somanet::EncoderShaft::kMotor);

  auto feed = drive.feedConstant();
  if (!feed) {
    return std::unexpected(feed.error());
  }
  if (feed->feed == 0 || feed->shaftRevolutions == 0) {
    return std::unexpected("the feed constant (0x6092) is 0");
  }
  plan.units.feedRatio = feed->feed == std::numeric_limits<uint32_t>::max()
                             ? 1.0
                             : static_cast<double>(feed->feed) / feed->shaftRevolutions;
  const auto positionSource = configurations->position.value_or(request.encoderOrdinal);
  const auto positionEncoder = std::ranges::find(configurations->encoders, positionSource,
                                                 &EncoderConfiguration::encoderOrdinal);
  plan.units.positionEncoderResolution =
      positionEncoder == configurations->encoders.end() ? 0 : positionEncoder->resolution;
  plan.units.positionPerRevolution = plan.units.feedRatio == 1.0
                                         ? static_cast<double>(plan.units.positionEncoderResolution)
                                         : plan.units.feedRatio;
  if (plan.units.positionPerRevolution <= 0) {
    return std::unexpected("the position encoder reports no resolution");
  }

  auto siUnit = drive.siUnitVelocity();
  if (!siUnit) {
    return std::unexpected(siUnit.error());
  }
  const auto perRpm = cia402::velocityUnitsPerRpm(*siUnit);
  if (!perRpm) {
    return std::unexpected(std::format(
        "the SI unit velocity 0x60A9 = 0x{:08X} is neither revolutions per minute nor per second",
        *siUnit));
  }
  plan.units.velocityPerRpm = *perRpm;
  if (plan.gearRatio > 1 && configurations->position &&
      configurations->position != configurations->velocity &&
      configurations->position != configurations->commutation &&
      *perRpm * plan.units.feedRatio / plan.gearRatio < 0.1) {
    return std::unexpected(
        "the velocity resolution is too coarse for the driving shaft; choose a finer unit in the "
        "SI unit velocity 0x60A9");
  }

  auto limits = drive.softwarePositionLimit();
  if (!limits) {
    return std::unexpected(limits.error());
  }
  plan.limits = *limits;
  if (limits->min >= limits->max) {
    return std::unexpected(
        "the software position limits (0x607D) are invalid: min is not below max");
  }
  auto position = drive.positionActualValue();
  if (!position) {
    return std::unexpected(position.error());
  }
  plan.startPosition = *position;
  if (*position < limits->min || *position > limits->max) {
    return std::unexpected("the position is outside the software position limits (0x607D)");
  }

  auto maxSpeed = drive.maxMotorSpeed();
  if (!maxSpeed) {
    return std::unexpected(maxSpeed.error());
  }
  const double maxMotorSpeedRpm = *maxSpeed / (*perRpm * plan.units.feedRatio);
  plan.maxVelocityRpm = 0.95 * maxMotorSpeedRpm / plan.gearRatio;

  somanet::IcMuMotionInput input;
  input.gearRatio = plan.gearRatio;
  input.maxMotorSpeedRpm = maxMotorSpeedRpm;
  input.shaft = plan.shaft;
  input.ringVelocityRpm = request.ringVelocityRpm.value_or(
      somanet::icMuDefaultRingVelocity(plan.startValues, plan.shaft));
  input.lowVelocityRpm = plan.shaft == somanet::EncoderShaft::kMotor
                             ? plan.startValues.motorShaftLowVelocityRpm
                             : plan.startValues.drivingShaftLowVelocityRpm;
  input.positionRevolutions = plan.units.revolutions(*position);
  input.limitRevolutions =
      std::pair{plan.units.revolutions(limits->min), plan.units.revolutions(limits->max)};
  auto motion = somanet::planIcMuMotion(input);
  if (!motion) {
    return std::unexpected(motion.error());
  }
  plan.motion = std::move(*motion);
  plan.warnings.insert(plan.warnings.end(), plan.motion.warnings.begin(),
                       plan.motion.warnings.end());

  plan.multiturnCheckOff = plan.ringPosition == somanet::IcMuRingPosition::kInner &&
                           found->sensorPort == 1 && found->multiturnBits != 0;
  return plan;
}

// A curve reduced to at most kIcMuCurvePoints points of [ring angle in degrees, value].
nlohmann::json curveJson(const std::vector<int64_t>& values,
                         std::span<const HrdEncoderSample> samples) {
  nlohmann::json points = nlohmann::json::array();
  if (values.empty()) {
    return points;
  }
  const size_t stride = std::max<size_t>(1, values.size() / kIcMuCurvePoints);
  for (size_t i = 0; i < values.size(); i += stride) {
    double angle = 360.0 * static_cast<double>(i) / static_cast<double>(values.size());
    if (values.size() == samples.size()) {
      const auto difference = (samples[i].masterCount - samples[i].noniusCount) & 0x3FFF;
      angle = 360.0 * difference / 16384.0;
    }
    points.push_back(nlohmann::json::array({angle, values[i]}));
  }
  return points;
}

nlohmann::json residualJson(const somanet::IcMuTrackResidual& residual, bool passed) {
  return nlohmann::json{{"sineOffset", residual.sineOffset},
                        {"cosineOffset", residual.cosineOffset},
                        {"gain", residual.gain},
                        {"phase", residual.phase},
                        {"phaseRange", residual.phaseRange},
                        {"passed", passed}};
}

}  // namespace

std::expected<void, std::string> runIcMuCalibrationProcedure(
    Device& device, ProgressReporter& reporter, const std::stop_token& stop,
    const IcMuCalibrationRequest& request) {
  auto drive = createSomanetDrive(device);
  if (!drive) {
    return std::unexpected(drive.error());
  }
  auto fail = [&reporter](std::string_view step, std::string error) {
    reporter.fail(step, error);
    return std::unexpected(std::move(error));
  };
  const auto encoder = request.encoderOrdinal;

  reporter.start(kIcMuValidateStep);
  auto planned = planIcMuCalibration(device, *drive, request, stop);
  if (!planned) {
    return fail(kIcMuValidateStep, planned.error());
  }
  auto& plan = *planned;
  reporter.succeed(kIcMuValidateStep, toJson(plan));
  const auto& units = plan.units;
  auto& motion = plan.motion;

  auto read = [&](uint8_t address) { return readIcMuRegister(*drive, encoder, address, stop); };
  auto write = [&](std::span<const somanet::IcMuRegisterWrite> writes) {
    return writeIcMuRegisters(*drive, encoder, writes, stop);
  };
  auto writeOne = [&](uint8_t address, uint8_t value) {
    return writeIcMuRegister(*drive, encoder, {address, value}, stop);
  };
  const OsCommandConfig bissConfig{
      .timeout = kBissServiceTimeout, .pollInterval = kEncoderRegisterPollInterval, .stop = stop};

  // What the run has changed, so that the restore path undoes exactly that.
  DriveRestorer restorer(*drive, reporter, kIcMuCalibrationProcedure);
  bool enabled = false;
  bool ignoring = false;
  bool rawMode = false;
  bool wroteEncoder = false;
  bool saved = false;
  bool interrupted = false;  // A fault or a cancel: the axis is not moved again.
  // The registers a full run writes, with the values they held before it.
  std::vector<somanet::IcMuRegisterWrite> original;

  // Fails the run the way the drive ended it: cancelled, faulted, or at a limit.
  auto checkMotion = [&]() -> std::expected<void, std::string> {
    if (stop.stop_requested()) {
      interrupted = true;
      (void)drive->quickStop();
      return std::unexpected("the calibration was cancelled");
    }
    auto statusword = drive->statusword();
    if (!statusword) {
      return std::unexpected(statusword.error());
    }
    if (cia402::isFaulted(*statusword)) {
      interrupted = true;
      auto report = drive->errorReport();
      return std::unexpected(
          std::format("the drive faulted: {}", report ? *report : std::string("no error report")));
    }
    if (cia402::isInternalLimitActive(*statusword)) {
      interrupted = true;
      (void)drive->quickStop();
      return std::unexpected(
          "the drive reached an internal limit, such as a software position limit");
    }
    if (cia402::decodeState(*statusword) != cia402::State::kOperationEnabled) {
      interrupted = true;
      return std::unexpected("the drive left Operation Enabled");
    }
    return {};
  };

  auto setAcceleration = [&](double rpmPerSecond) -> std::expected<void, std::string> {
    const auto value = units.acceleration(rpmPerSecond);
    return drive->setProfileAcceleration(value).and_then(
        [&] { return drive->setProfileDeceleration(value); });
  };

  // Waits until the velocity demand is 0, so that a mode or frame switch never happens in motion.
  auto waitForStandstill = [&]() -> std::expected<void, std::string> {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (true) {
      if (auto r = checkMotion(); !r) {
        return r;
      }
      auto demand = drive->velocityDemandValue();
      if (!demand) {
        return std::unexpected(demand.error());
      }
      if (*demand == 0) {
        return {};
      }
      if (std::chrono::steady_clock::now() > deadline) {
        return std::unexpected("the drive did not come to a standstill within 30 s");
      }
      std::this_thread::sleep_for(request.pollInterval);
    }
  };

  // Moves to @p target in profile velocity mode: slower near the target, stopped as it is crossed.
  auto moveTo = [&](int32_t target) -> std::expected<void, std::string> {
    auto position = drive->positionActualValue();
    if (!position) {
      return std::unexpected(position.error());
    }
    const double distance = std::abs(units.revolutions(int64_t{target} - *position));
    double velocity = 150.0 * (std::sqrt(1.0 + distance / 12.5) - 1.0);
    velocity = std::clamp(std::min(velocity, plan.maxVelocityRpm), kMoveMinVelocityRpm,
                          kMoveMaxVelocityRpm);
    const int direction = target > *position ? 1 : -1;
    if (auto r = setAcceleration(kMoveAccelerationRpmPerSecond); !r) {
      return r;
    }
    if (auto r = drive->setTargetVelocity(units.velocity(direction * velocity)); !r) {
      return r;
    }
    const auto oneDegree = std::abs(units.position(1.0 / 360));
    auto lastMoved = std::chrono::steady_clock::now();
    int32_t lastPosition = *position;
    while (true) {
      if (auto r = checkMotion(); !r) {
        return r;
      }
      position = drive->positionActualValue();
      if (!position) {
        return std::unexpected(position.error());
      }
      const int64_t remaining = int64_t{target} - *position;
      if (remaining * direction <= 0) {
        break;
      }
      const double left = std::abs(units.revolutions(remaining)) / std::max(distance, 1e-9);
      if (left < 0.05) {
        (void)drive->setTargetVelocity(units.velocity(direction * velocity * (0.1 + left)));
      }
      if (std::abs(int64_t{*position} - lastPosition) > oneDegree) {
        lastPosition = *position;
        lastMoved = std::chrono::steady_clock::now();
      } else if (std::chrono::steady_clock::now() - lastMoved > kMoveStationaryTimeout) {
        (void)drive->setTargetVelocity(0);
        return std::unexpected("the axis did not move towards its target for 10 s");
      }
      std::this_thread::sleep_for(request.pollInterval);
    }
    if (auto r = drive->setTargetVelocity(0); !r) {
      return r;
    }
    return waitForStandstill().and_then(
        [&] { return setAcceleration(motion.accelerationRpmPerSecond); });
  };

  auto calibrate = [&]() -> std::expected<void, std::string> {
    // Prepare: remember what the run changes, enable in profile velocity mode, and move to the
    // start the plan asks for.
    reporter.start(kPrepareStep);
    auto mode = drive->operationModeValue();
    auto acceleration = drive->profileAcceleration();
    auto deceleration = drive->profileDeceleration();
    auto targetVelocity = drive->targetVelocity();
    auto state = drive->state();
    for (const auto* error :
         {mode ? nullptr : &mode.error(), acceleration ? nullptr : &acceleration.error(),
          deceleration ? nullptr : &deceleration.error(),
          targetVelocity ? nullptr : &targetVelocity.error(), state ? nullptr : &state.error()}) {
      if (error != nullptr) {
        return fail(kPrepareStep, *error);
      }
    }
    restorer.restoreMode(*mode, *state);
    restorer.restoreProfile(*acceleration, *deceleration);
    restorer.restoreTargetVelocity(*targetVelocity);
    auto prepared = drive->setTargetVelocity(0)
                        .and_then([&] { return setAcceleration(motion.accelerationRpmPerSecond); })
                        .and_then([&] {
                          return drive->setOperationMode(cia402::OperationMode::kProfileVelocity);
                        })
                        .and_then([&] { return drive->enable(); });
    if (!prepared) {
      return fail(kPrepareStep, prepared.error());
    }
    enabled = true;
    if (motion.startRevolutions) {
      if (auto r = moveTo(static_cast<int32_t>(units.position(*motion.startRevolutions))); !r) {
        return fail(kPrepareStep, r.error());
      }
    }
    if (motion.limitedRange) {
      // Plan the recording again from where the axis really stopped.
      auto position = drive->positionActualValue();
      if (!position) {
        return fail(kPrepareStep, position.error());
      }
      const double travel =
          units.revolutions(plan.limits.max) - (3.0 / 360) - units.revolutions(*position);
      auto narrow = somanet::planIcMuNarrowRange(
          travel, motion.accelerationRpmPerSecond,
          somanet::icMuRingTurnsPerRevolution(plan.shaft, plan.gearRatio),
          plan.shaft == somanet::EncoderShaft::kMotor
              ? plan.startValues.motorShaftLowVelocityRpm
              : plan.startValues.drivingShaftLowVelocityRpm);
      if (!narrow) {
        return fail(kPrepareStep, narrow.error());
      }
      motion.velocityRpm = std::min(narrow->velocityRpm, plan.maxVelocityRpm);
      motion.recording = narrow->recording;
    }
    reporter.succeed(kPrepareStep, nlohmann::json{{"velocityRpm", motion.velocityRpm},
                                                  {"recordingMs", motion.recording.count()}});

    reporter.start(kIcMuIgnoreStatusBitsStep);
    if (auto r = drive->setIgnoreBissStatusBits(encoder, true, bissConfig); !r) {
      return fail(kIcMuIgnoreStatusBitsStep, r.error());
    }
    ignoring = true;
    reporter.succeed(kIcMuIgnoreStatusBitsStep);

    // The registers a full run changes, read before it changes any of them.
    std::vector<uint8_t> touched = {somanet::kIcMuOutputRegister, kIcMuModeRegister};
    if (!request.measurementOnly) {
      for (const auto& w : somanet::icMuStartWrites(plan.startValues)) {
        touched.push_back(w.address);
      }
      if (plan.multiturnCheckOff) {
        touched.push_back(kIcMuMultiturnInterfaceRegister);
      }
    }
    for (const uint8_t address : touched) {
      auto value = read(address);
      if (!value) {
        return fail(request.measurementOnly ? kIcMuRawModeStep : kIcMuStartValuesStep,
                    value.error());
      }
      original.push_back({address, *value});
    }

    if (!request.measurementOnly) {
      reporter.start(kIcMuStartValuesStep);
      wroteEncoder = true;
      auto started = write(somanet::icMuStartWrites(plan.startValues));
      if (started && plan.multiturnCheckOff) {
        const auto interface = std::ranges::find(original, kIcMuMultiturnInterfaceRegister,
                                                 &somanet::IcMuRegisterWrite::address);
        started = writeOne(kIcMuMultiturnInterfaceRegister,
                           static_cast<uint8_t>(interface->value & ~kIcMuMultiturnCheck));
      }
      if (!started) {
        return fail(kIcMuStartValuesStep, started.error());
      }
      reporter.succeed(kIcMuStartValuesStep);
    }

    reporter.start(kIcMuRawModeStep);
    rawMode = true;
    auto raw = waitForStandstill()
                   .and_then([&] {
                     return drive->setIcMuCalibrationMode(
                         encoder, somanet::IcMuCalibrationMode::kConfiguration, bissConfig);
                   })
                   .and_then([&] { return writeOne(somanet::kIcMuOutputRegister, kIcMuRawOutMsb); })
                   .and_then([&] { return writeOne(kIcMuModeRegister, kIcMuRawMode); })
                   .and_then([&] {
                     return drive->setIcMuCalibrationMode(
                         encoder, somanet::IcMuCalibrationMode::kRaw, bissConfig);
                   });
    if (!raw) {
      return fail(kIcMuRawModeStep, raw.error());
    }
    reporter.succeed(kIcMuRawModeStep);

    // The model of MU_3SL starts from what the encoder holds now.
    auto engine = somanet::IcMuCalibrationEngine::create(plan.hardRev, plan.masterPolePairs);
    if (!engine) {
      return fail(icMuIterationStep(1), engine.error());
    }
    std::array<somanet::IcMuAnalogAdjustments, 2> current{};
    for (const auto track : {somanet::IcMuTrack::kMaster, somanet::IcMuTrack::kNonius}) {
      std::array<uint8_t, 4> values{};
      const auto registers = somanet::icMuAnalogRegisters(track);
      for (size_t i = 0; i < registers.size(); ++i) {
        auto value = read(registers[i]);
        if (!value) {
          return fail(icMuIterationStep(1), value.error());
        }
        values[i] = *value;
      }
      current[track == somanet::IcMuTrack::kMaster ? 0 : 1] =
          somanet::icMuAnalogFromRegisters(values);
    }
    // The model starts from the table the encoder holds: the one a full run just zeroed, or the
    // calibrated one a validation measures.
    auto modelled = engine->setAdjustments(current[0], current[1]);
    if (modelled) {
      std::array<uint8_t, 8> table{};
      for (size_t i = 0; i < table.size() && modelled; ++i) {
        auto value = read(static_cast<uint8_t>(somanet::kIcMuNoniusTableRegister + i));
        if (!value) {
          modelled = std::unexpected(value.error());
        } else {
          table[i] = *value;
        }
      }
      if (modelled) {
        modelled = engine->setNoniusTable(somanet::icMuNoniusTableFromRegisters(table));
      }
    }
    if (!modelled) {
      return fail(icMuIterationStep(1), modelled.error());
    }

    // One move and one recording.
    auto record = [&](int direction) -> std::expected<std::vector<HrdEncoderSample>, std::string> {
      auto configured =
          drive->configureHrdStream(somanet::HrdData::kEncoderRawData, motion.recording,
                                    {.timeout = std::chrono::seconds(10),
                                     .pollInterval = std::chrono::milliseconds(100),
                                     .stop = stop});
      if (!configured) {
        return std::unexpected(configured.error());
      }
      const double velocity = direction * motion.velocityRpm;
      const int32_t target = units.velocity(velocity);
      const double leadRpm =
          motion.velocityRpm / motion.accelerationRpmPerSecond > kIcMuRecordingLead
              ? motion.velocityRpm - kIcMuRecordingLead * motion.accelerationRpmPerSecond
              : 0.0;
      const int32_t lead = std::abs(units.velocity(leadRpm));
      std::future<std::expected<void, std::string>> recording;
      auto startRecording = [&] {
        if (!recording.valid()) {
          recording = std::async(std::launch::async, [&] {
            return drive->startHrdStream({.timeout = motion.recording + std::chrono::seconds(20),
                                          .pollInterval = std::chrono::milliseconds(100),
                                          .stop = stop});
          });
        }
      };
      auto finish = [&](std::expected<void, std::string> moved)
          -> std::expected<std::vector<HrdEncoderSample>, std::string> {
        if (recording.valid()) {
          auto recorded = recording.get();
          if (moved && !recorded) {
            moved = std::unexpected(recorded.error());
          }
        }
        if (!moved) {
          return std::unexpected(moved.error());
        }
        auto file = drive->readHrdRecording(somanet::HrdData::kEncoderRawData);
        if (!file) {
          return std::unexpected(file.error());
        }
        const auto* samples = std::get_if<std::vector<HrdEncoderSample>>(&file->samples);
        if (samples == nullptr || samples->empty()) {
          return std::unexpected("the drive recorded no encoder raw data");
        }
        return *samples;
      };

      if (auto r = drive->setTargetVelocity(target); !r) {
        return finish(std::unexpected(r.error()));
      }
      std::optional<std::chrono::steady_clock::time_point> reached;
      bool stopping = false;
      const auto deadline =
          std::chrono::steady_clock::now() + motion.recording + std::chrono::seconds(60);
      while (true) {
        if (auto r = checkMotion(); !r) {
          return finish(std::unexpected(r.error()));
        }
        auto demand = drive->velocityDemandValue();
        if (!demand) {
          return finish(std::unexpected(demand.error()));
        }
        const int32_t magnitude = std::abs(*demand);
        if (!stopping && magnitude >= lead) {
          startRecording();
        }
        if (!stopping && !reached && magnitude >= std::abs(target)) {
          reached = std::chrono::steady_clock::now();
        }
        if (!stopping && reached &&
            std::chrono::steady_clock::now() - *reached >= motion.recording) {
          if (auto r = drive->setTargetVelocity(0); !r) {
            return finish(std::unexpected(r.error()));
          }
          stopping = true;
        }
        if (stopping && *demand == 0) {
          break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
          (void)drive->setTargetVelocity(0);
          return finish(std::unexpected("the drive did not reach the calibration velocity"));
        }
        std::this_thread::sleep_for(request.pollInterval);
      }
      return finish({});
    };

    bool bothPassed = false;
    bool coverageWarned = false;
    int direction = 1;
    for (int iteration = 1;; ++iteration) {
      const bool validation = bothPassed || request.measurementOnly;
      const std::string step =
          validation ? std::string(kIcMuValidationStep) : icMuIterationStep(iteration);
      reporter.start(step);
      auto recorded = record(direction);
      if (!recorded) {
        return fail(step, recorded.error());
      }
      const auto samples = somanet::icMuCalibrationSamples(*recorded, motion.recording);
      nlohmann::json value{{"iteration", iteration},
                           {"direction", direction},
                           {"velocityRpm", motion.velocityRpm},
                           {"samples", samples.size()}};
      const double covered = somanet::icMuRecordedRingRevolutions(samples, plan.masterPolePairs);
      value["recordedRevolutions"] = covered;
      if (auto warning = somanet::icMuCoverageWarning(covered); warning && !coverageWarned) {
        coverageWarned = true;
        value["warning"] = *warning;
      }
      auto analysis = engine->analyze(samples);
      if (!analysis) {
        return fail(step, analysis.error());
      }
      value["phaseRangeLimit"] = analysis->phaseRangeLimit;

      if (validation) {
        value["phaseMargin"] = curveJson(analysis->phaseMargin, samples);
        value["lowerMarginPercent"] = analysis->lowerMarginPercent;
        value["upperMarginPercent"] = analysis->upperMarginPercent;
        const double margin = std::min(analysis->lowerMarginPercent, analysis->upperMarginPercent);
        const auto band = somanet::icMuMarginBand(margin);
        value["marginPercent"] = margin;
        value["marginBand"] = somanet::toString(band);
        value["marginMessage"] = somanet::icMuMarginMessage(band);
        const bool passed = analysis->noniusValid &&
                            analysis->lowerMarginPercent >= kIcMuMinMarginPercent &&
                            analysis->upperMarginPercent >= kIcMuMinMarginPercent;
        value["passed"] = passed;
        if (encoder == plan.encoders.commutation && !request.measurementOnly) {
          value["commutationWarning"] =
              "The calibrated encoder is the commutation source. Measure the commutation offset "
              "again, and store the parameters to keep the new 0x2001.";
        }
        if (!passed) {
          const std::string why =
              request.measurementOnly
                  ? "The nonius phase margin is below 10 %. The encoder system changed after the "
                    "last calibration, so the absolute position at startup is not reliable; "
                    "calibrate the encoder again."
                  : "The nonius phase margin is below 10 %, too low to compute the absolute "
                    "position reliably. Check the mechanical assembly, the ring for damage, and "
                    "eccentricity.";
          reporter.fail(step, why);
          return std::unexpected(why);
        }
        reporter.succeed(step, value);
        return {};
      }

      const int tolerance = somanet::icMuTolerance(iteration);
      const bool masterPassed =
          somanet::icMuTrackPasses(somanet::IcMuTrack::kMaster, analysis->master, tolerance);
      const bool noniusPassed =
          somanet::icMuTrackPasses(somanet::IcMuTrack::kNonius, analysis->nonius, tolerance);
      value["tolerance"] = tolerance;
      value["master"] = residualJson(analysis->master, masterPassed);
      value["nonius"] = residualJson(analysis->nonius, noniusPassed);
      value["phaseError"] = curveJson(analysis->phaseError, samples);

      std::expected<void, std::string> written;
      if (masterPassed && noniusPassed) {
        auto table = engine->optimizeNoniusTable();
        written = table ? write(somanet::icMuNoniusTableWrites(*table))
                        : std::expected<void, std::string>(std::unexpected(table.error()));
        bothPassed = true;
      } else if (iteration == somanet::kIcMuMaxIterations) {
        const std::string why = std::format(
            "the tracks did not pass in {} iterations; the recordings are not stable enough to "
            "calibrate the encoder",
            somanet::kIcMuMaxIterations);
        reporter.fail(step, why);
        return std::unexpected(why);
      } else {
        written = engine->adjust(!masterPassed, !noniusPassed).and_then([&] {
          std::vector<somanet::IcMuRegisterWrite> writes;
          for (const auto track : {somanet::IcMuTrack::kMaster, somanet::IcMuTrack::kNonius}) {
            const bool passed = track == somanet::IcMuTrack::kMaster ? masterPassed : noniusPassed;
            if (!passed) {
              const auto w = somanet::icMuAnalogWrites(track, engine->adjustments(track));
              writes.insert(writes.end(), w.begin(), w.end());
            }
          }
          return write(writes);
        });
      }
      if (!written) {
        return fail(step, written.error());
      }
      reporter.succeed(step, value);
      direction = -direction;
    }
  };

  // The frame back to standard, with the output registers the run found.
  auto leaveRawMode = [&]() -> std::expected<void, std::string> {
    auto original11 = std::ranges::find(original, somanet::kIcMuOutputRegister,
                                        &somanet::IcMuRegisterWrite::address);
    auto original12 =
        std::ranges::find(original, kIcMuModeRegister, &somanet::IcMuRegisterWrite::address);
    auto restored = drive->setIcMuCalibrationMode(
        encoder, somanet::IcMuCalibrationMode::kConfiguration,
        {.timeout = kBissServiceTimeout, .pollInterval = kEncoderRegisterPollInterval});
    if (restored && original11 != original.end()) {
      restored = writeIcMuRegister(*drive, encoder, *original11, {});
    }
    if (restored && original12 != original.end()) {
      restored = writeIcMuRegister(*drive, encoder, *original12, {});
    }
    if (restored) {
      restored = drive->setIcMuCalibrationMode(
          encoder, somanet::IcMuCalibrationMode::kStandard,
          {.timeout = kBissServiceTimeout, .pollInterval = kEncoderRegisterPollInterval});
    }
    rawMode = false;
    return restored;
  };

  auto calibrated = calibrate();

  if (calibrated && !request.measurementOnly) {
    // The save: standard frame first, so the EEPROM never stores raw mode.
    reporter.start(kIcMuStandardModeStep);
    if (auto r = waitForStandstill().and_then(leaveRawMode); !r) {
      calibrated = fail(kIcMuStandardModeStep, r.error());
    } else {
      reporter.succeed(kIcMuStandardModeStep);
    }
    if (calibrated && plan.multiturnCheckOff) {
      auto value = std::ranges::find(original, kIcMuMultiturnInterfaceRegister,
                                     &somanet::IcMuRegisterWrite::address);
      if (auto r = writeIcMuRegister(*drive, encoder, *value, stop); !r) {
        calibrated = fail(kIcMuStandardModeStep, r.error());
      }
    }
    if (calibrated) {
      reporter.start(kIcMuSaveStep);
      std::expected<uint8_t, std::string> stored = 0;
      if (request.activateHealthMonitoring) {
        stored = writeOne(0x0C, kIcMuHealthMonitoring).transform([] { return uint8_t{0}; });
      }
      stored = stored.and_then([&](uint8_t) { return saveIcMuEeprom(*drive, encoder, stop); });
      if (!stored) {
        calibrated = fail(kIcMuSaveStep, stored.error());
      } else {
        saved = true;
        reporter.succeed(kIcMuSaveStep, nlohmann::json{{"status1", *stored}});
      }
    }
    if (calibrated) {
      reporter.start(kIcMuAbsResetStep);
      auto before = drive->positionActualValue();
      auto reset = before.and_then([&](int32_t) {
        return writeIcMuRegister(
            *drive, encoder,
            {somanet::kIcMuCommandRegister, static_cast<uint8_t>(somanet::IcMuCommand::kAbsReset)},
            stop);
      });
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      if (!reset) {
        calibrated = fail(kIcMuAbsResetStep, reset.error());
      } else {
        nlohmann::json value = nlohmann::json::object();
        auto after = drive->positionActualValue();
        if (after && plan.encoders.position == encoder) {
          const double jump = std::abs(static_cast<double>(int64_t{*after} - *before)) /
                              units.positionPerRevolution * units.positionEncoderResolution;
          value["jumpCounts"] = jump;
          if (jump > kIcMuAbsResetJump) {
            value["warning"] =
                "The absolute position changed after the calibration. Reference the machine "
                "again and set the software position limits again.";
          }
        }
        reporter.succeed(kIcMuAbsResetStep, value);
      }
    }
    if (calibrated) {
      reporter.start(kIcMuRestoreFileStep);
      if (!somanet::isIntegratedSensorPort(plan.encoder.sensorPort)) {
        reporter.succeed(kIcMuRestoreFileStep,
                         nlohmann::json{{"file", nullptr},
                                        {"reason",
                                         "the drive keeps a restore file only for sensor "
                                         "ports 1 and 2"}});
      } else if (auto file =
                     writeIcMuRestoreFile(device, *drive, encoder, plan.encoder.sensorPort, stop);
                 !file) {
        calibrated = fail(kIcMuRestoreFileStep, file.error());
      } else {
        reporter.succeed(kIcMuRestoreFileStep, nlohmann::json{{"file", file->name}});
      }
    }
  }

  // The restore path. Its commands take no stop token: a cancelled run is what it exists for.
  if (rawMode) {
    reporter.start(kIcMuStandardModeStep);
    if (enabled && !interrupted) {
      (void)drive->setTargetVelocity(0);
    }
    if (auto r = leaveRawMode(); !r) {
      reporter.fail(kIcMuStandardModeStep, r.error());
    } else {
      reporter.succeed(kIcMuStandardModeStep);
    }
  }
  if (wroteEncoder && !saved) {
    reporter.start(kIcMuRestoreEncoderStep);
    std::expected<void, std::string> restored;
    for (const auto& w : original) {
      if (restored) {
        restored = writeIcMuRegister(*drive, encoder, w, {});
      }
    }
    if (!restored) {
      reporter.fail(kIcMuRestoreEncoderStep, restored.error());
    } else {
      reporter.succeed(kIcMuRestoreEncoderStep);
    }
  }
  if (enabled && !interrupted) {
    reporter.start(kIcMuReturnStep);
    auto position = drive->positionActualValue();
    const auto twoDegrees = units.position(2.0 / 360);
    const auto threeDegrees = units.position(3.0 / 360);
    const bool inside = plan.startPosition > plan.limits.min + twoDegrees &&
                        plan.startPosition < plan.limits.max - twoDegrees;
    if (position && inside && std::abs(int64_t{*position} - plan.startPosition) > threeDegrees) {
      if (auto r = moveTo(plan.startPosition); !r) {
        reporter.fail(kIcMuReturnStep, r.error());
      } else {
        reporter.succeed(kIcMuReturnStep);
      }
    } else {
      reporter.succeed(kIcMuReturnStep, nlohmann::json{{"moved", false}});
    }
  }
  if (ignoring) {
    reporter.start(kIcMuRestoreStatusBitsStep);
    if (auto r = drive->setIgnoreBissStatusBits(
            encoder, false,
            {.timeout = kBissServiceTimeout, .pollInterval = kEncoderRegisterPollInterval});
        !r) {
      reporter.fail(kIcMuRestoreStatusBitsStep, r.error());
    } else {
      reporter.succeed(kIcMuRestoreStatusBitsStep);
    }
  }
  return calibrated;
}

#endif  // MM_HAVE_MU_3SL

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
  DriveRestorer restorer(*drive, reporter, kOpenPhaseDetectionProcedure);

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

  DriveRestorer restorer(*drive, reporter, kOffsetDetectionProcedure);

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

std::vector<ProgressStep> commutationOffsetMeasurementSteps() {
  return stepsFrom(
      {kPrepareStep, kReleaseBrakeStep, kCommutationOffsetMeasurementStep, kRestoreStep});
}

std::expected<void, std::string> runCommutationOffsetMeasurementProcedure(
    Device& device, ProgressReporter& reporter, std::stop_token stop) {
  auto drive = createSomanetDrive(device);
  if (!drive) {
    return std::unexpected(drive.error());
  }

  // Read before anything is changed, and reported as a run-level failure rather than against a
  // step: the method decides whether the brake has to be released, so not knowing it means there is
  // no safe way to proceed — and refusing here leaves the drive untouched instead of enabled in
  // diagnostics mode.
  //
  // The rotating methods run only with a disengaged brake, and the firmware refuses the command
  // outright otherwise. The stationary method asks nothing of the brake, so the brake is left
  // exactly as it was found and whatever it holds stays held.
  auto method = drive->commutationOffsetMethod();
  if (!method) {
    return std::unexpected(method.error());
  }

  return runMeasurementProcedure(
      device, reporter, std::move(stop),
      {.procedure = kCommutationOffsetMeasurementProcedure,
       .step = kCommutationOffsetMeasurementStep,
       .what = "commutation offset measurement",
       .releaseBrake = somanet::requiresBrakeReleased(*method),
       .timeout = std::chrono::seconds(60)},
      [method = *method](SomanetDrive& drive, const OsCommandConfig& config) {
        return drive.runCommutationOffsetMeasurement(method, config);
      });
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

// Writes the chirp's settings: a disarm, then the five values. The disarm is what makes an arm
// that follows a rising edge, which is the only thing the firmware acts on.
//
// @param what  How to name the run in a cancellation message.
std::expected<void, std::string> writeChirpSettings(SomanetDrive& drive,
                                                    const SystemIdentificationRequest& request,
                                                    const OsCommandConfig& config,
                                                    std::string_view what) {
  using Parameter = somanet::SystemIdentificationParameter;
  const std::array<std::pair<Parameter, uint32_t>, 6> settings{{
      {Parameter::kStartProcedure,
       static_cast<uint32_t>(somanet::SystemIdentificationStart::kNone)},
      {Parameter::kStartFrequency, request.startFrequencyMilliHz},
      {Parameter::kTargetFrequency, request.targetFrequencyMilliHz},
      {Parameter::kTargetAmplitude, request.targetAmplitudePermil},
      {Parameter::kTransitionTime, request.transitionTimeMs},
      {Parameter::kSignalType, static_cast<uint32_t>(request.signalType)},
  }};
  for (const auto& [parameter, value] : settings) {
    if (config.stop.stop_requested()) {
      return std::unexpected(
          std::format("{} was cancelled after {}", what, somanet::toString(parameter)));
    }
    if (auto r = drive.setSystemIdentificationParameter(parameter, value, config); !r) {
      return std::unexpected(r.error());
    }
  }
  return {};
}

// The chirp settings as a step records them. Nothing reads them back from the drive, so the step's
// record is the only account of what it holds.
nlohmann::json chirpSettingsJson(const SystemIdentificationRequest& request) {
  return nlohmann::json{{"startFrequencyMilliHz", request.startFrequencyMilliHz},
                        {"targetFrequencyMilliHz", request.targetFrequencyMilliHz},
                        {"targetAmplitudePermil", request.targetAmplitudePermil},
                        {"transitionTimeMs", request.transitionTimeMs},
                        {"signalType", somanet::toString(request.signalType)}};
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

std::optional<FirmwareLatencyAction> parseFirmwareLatencyAction(std::string_view token) {
  for (const auto action : {FirmwareLatencyAction::kMeasure, FirmwareLatencyAction::kStart,
                            FirmwareLatencyAction::kReadMaximum, FirmwareLatencyAction::kStop}) {
    if (token == toString(action)) {
      return action;
    }
  }
  return std::nullopt;
}

std::expected<FirmwareLatencyRequest, std::string> parseFirmwareLatencyRequest(
    const nlohmann::json& body) {
  if (!body.is_object()) {
    return std::unexpected("the request body must be a JSON object");
  }
  FirmwareLatencyRequest request;

  if (auto action = body.find("action"); action != body.end() && !action->is_null()) {
    if (!action->is_string()) {
      return std::unexpected("'action' must be a string");
    }
    auto parsed = parseFirmwareLatencyAction(action->get<std::string>());
    if (!parsed) {
      return std::unexpected(std::format(
          "'action' must be {}, {}, {} or {}", toString(FirmwareLatencyAction::kMeasure),
          toString(FirmwareLatencyAction::kStart), toString(FirmwareLatencyAction::kReadMaximum),
          toString(FirmwareLatencyAction::kStop)));
    }
    request.action = *parsed;
  }

  if (auto latency = body.find("latency"); latency != body.end() && !latency->is_null()) {
    if (!latency->is_string()) {
      return std::unexpected("'latency' must be a string");
    }
    auto parsed = somanet::parseFirmwareLatency(latency->get<std::string>());
    if (!parsed) {
      return std::unexpected(std::format("'latency' must be {} or {}",
                                         somanet::toString(somanet::FirmwareLatency::kSetpoint),
                                         somanet::toString(somanet::FirmwareLatency::kFeedback)));
    }
    request.latency = *parsed;
  }

  auto duration = readMillis(body, "durationMs", request.duration, kMinFirmwareLatencyDuration,
                             kMaxFirmwareLatencyDuration);
  if (!duration) {
    return std::unexpected(duration.error());
  }
  request.duration = *duration;
  return request;
}

std::vector<ProcedureParameter> firmwareLatencyParameters() {
  const FirmwareLatencyRequest defaults;
  return {
      enumParameter(
          "action", "Action",
          "What to do. Measure starts a measurement, lets the drive run for the duration below, "
          "then reads the maximum — the only action that answers with a figure. Start and read "
          "maximum are the same two halves as separate runs, for a window longer than one run can "
          "wait: start now, run the machine, read afterwards. Stop ends measuring.",
          std::string(toString(defaults.action)),
          {
              ParameterOption{.value = std::string(toString(FirmwareLatencyAction::kMeasure)),
                              .title = "Measure"},
              ParameterOption{.value = std::string(toString(FirmwareLatencyAction::kStart)),
                              .title = "Start measurement"},
              ParameterOption{.value = std::string(toString(FirmwareLatencyAction::kReadMaximum)),
                              .title = "Read and clear maximum"},
              ParameterOption{.value = std::string(toString(FirmwareLatencyAction::kStop)),
                              .title = "Stop measurements"},
          }),
      enumParameter(
          "latency", "Latency",
          "Which duration inside the drive control cycle to measure. Setpoint latency runs from "
          "the "
          "start of the cycle until the setpoints are handed to the motion control service; "
          "feedback latency from the request for control feedback until the end of the cycle. The "
          "two are measured independently. Ignored when stopping, which stops both.",
          std::string(somanet::toString(defaults.latency)),
          {
              ParameterOption{
                  .value = std::string(somanet::toString(somanet::FirmwareLatency::kSetpoint)),
                  .title = "Setpoint latency"},
              ParameterOption{
                  .value = std::string(somanet::toString(somanet::FirmwareLatency::kFeedback)),
                  .title = "Feedback latency"},
          }),
      integerParameter(
          "durationMs", "Duration (ms)",
          "How long to let the measurement collect before reading it, for the measure action only. "
          "What comes back is the worst case over this window, so it is only as informative as "
          "what "
          "the drive was doing during it — measuring an idle drive reports an idle drive.",
          defaults.duration.count(), kMinFirmwareLatencyDuration.count(),
          kMaxFirmwareLatencyDuration.count()),
  };
}

std::vector<ProgressStep> firmwareLatencySteps() {
  return stepsFrom({kFirmwareLatencyStartStep, kFirmwareLatencyObserveStep,
                    kFirmwareLatencyReadStep, kFirmwareLatencyStopStep});
}

std::expected<void, std::string> runFirmwareLatencyProcedure(
    Device& device, ProgressReporter& reporter, std::stop_token stop,
    const FirmwareLatencyRequest& request) {
  auto drive = createSomanetDrive(device);
  if (!drive) {
    return std::unexpected(drive.error());
  }

  // One config for whichever commands this action issues, and the only holder of the stop token —
  // the observation window checks config.stop rather than keeping a second copy of it.
  const OsCommandConfig config{.timeout = kFirmwareLatencyTimeout,
                               .pollInterval = kEncoderRegisterPollInterval,
                               .stop = std::move(stop)};

  const bool starts = request.action == FirmwareLatencyAction::kMeasure ||
                      request.action == FirmwareLatencyAction::kStart;
  const bool reads = request.action == FirmwareLatencyAction::kMeasure ||
                     request.action == FirmwareLatencyAction::kReadMaximum;

  if (starts) {
    reporter.start(kFirmwareLatencyStartStep);
    auto started = drive->startFirmwareLatencyMeasurement(request.latency, config);
    if (!started) {
      reporter.fail(kFirmwareLatencyStartStep, started.error());
      return std::unexpected(started.error());
    }
    // What was started, because nothing on the drive reports it back — a later read is the only
    // other account of it, and by then this run's record is what says which latency it began.
    reporter.succeed(kFirmwareLatencyStartStep,
                     nlohmann::json{{"latency", somanet::toString(request.latency)}});
  }

  if (request.action == FirmwareLatencyAction::kMeasure) {
    reporter.start(kFirmwareLatencyObserveStep);
    if (!sleepUnlessStopped(request.duration, config.stop)) {
      // The measurement is running and stays running: cancelling the wait abandons the reading, and
      // stopping here would stop the other latency's measurement too.
      const auto reason = "the firmware latency measurement was cancelled while collecting";
      reporter.fail(kFirmwareLatencyObserveStep, reason);
      return std::unexpected(reason);
    }
    reporter.succeed(kFirmwareLatencyObserveStep,
                     nlohmann::json{{"durationMs", request.duration.count()}});
  }

  if (reads) {
    reporter.start(kFirmwareLatencyReadStep);
    auto result = drive->readMaximumFirmwareLatency(request.latency, config);
    if (!result) {
      reporter.fail(kFirmwareLatencyReadStep, result.error());
      return std::unexpected(result.error());
    }
    reporter.succeed(kFirmwareLatencyReadStep, *result);
  }

  if (request.action == FirmwareLatencyAction::kStop) {
    reporter.start(kFirmwareLatencyStopStep);
    auto stopped = drive->stopFirmwareLatencyMeasurements(config);
    if (!stopped) {
      reporter.fail(kFirmwareLatencyStopStep, stopped.error());
      return std::unexpected(stopped.error());
    }
    reporter.succeed(kFirmwareLatencyStopStep);
  }
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

  reporter.start(kSystemIdConfigureStep);
  if (auto r = writeChirpSettings(*drive, request, config, "system identification"); !r) {
    reporter.fail(kSystemIdConfigureStep, r.error());
    return std::unexpected(r.error());
  }
  reporter.succeed(kSystemIdConfigureStep, chirpSettingsJson(request));

  reporter.start(kSystemIdArmStep);
  if (auto r = drive->setSystemIdentificationParameter(
          somanet::SystemIdentificationParameter::kStartProcedure,
          static_cast<uint32_t>(request.start), config);
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

  auto encoder = readEncoderOrdinal(body, request.encoderOrdinal);
  if (!encoder) {
    return std::unexpected(encoder.error());
  }
  request.encoderOrdinal = *encoder;

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
      encoderParameter(defaults.encoderOrdinal),
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
  auto result = drive->setIgnoreBissStatusBits(request.encoderOrdinal, request.ignore,
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
                   nlohmann::json{{"encoderOrdinal", static_cast<uint8_t>(request.encoderOrdinal)},
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

std::vector<ProgressStep> readObjectDictionarySteps() {
  return stepsFrom({kReadObjectDictionaryStep});
}

std::expected<void, std::string> runReadObjectDictionaryProcedure(Device& device,
                                                                  ProgressReporter& reporter,
                                                                  std::stop_token stop) {
  auto drive = createSomanetDrive(device);
  if (!drive) {
    return std::unexpected(drive.error());
  }

  reporter.start(kReadObjectDictionaryStep);
  auto values =
      drive->readObjectDictionaryValues({.timeout = kReadObjectDictionaryTimeout,
                                         .pollInterval = kReadObjectDictionaryPollInterval,
                                         .stop = std::move(stop)});
  if (!values) {
    reporter.fail(kReadObjectDictionaryStep, values.error());
    return std::unexpected(values.error());
  }
  reporter.succeed(kReadObjectDictionaryStep, *values);
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
                       "the SOMANET package naming convention. The package's ESI is stored too, so "
                       "a device on this firmware gets its parameter metadata at the next scan.",
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
                    kAppFirmwareStep, kComFirmwareStep, kActivateFirmwareStep, kFinalStateStep});
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
    // cycle — long past the point where it could still be declined.
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
      // The ESI goes with the package, so a device on this firmware gets its metadata at the next
      // scan. Best effort like the package itself: a package with no ESI still installs.
      if (auto esi = readPackageEsi(request.package); !esi) {
        spdlog::warn("Firmware package {}: the ESI was not stored: {}", request.filename,
                     esi.error());
      } else if (*esi) {
        if (auto stored = deviceManager.esiCache().store(**esi, request.name->softwareVersion);
            stored) {
          spdlog::info("Firmware package {}: stored its ESI as {}", request.filename,
                       stored->string());
        } else {
          spdlog::warn("Firmware package {}: the ESI was not stored: {}", request.filename,
                       stored.error());
        }
      }
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
  } else if (const auto device = deviceManager.deviceAt(devicePosition);
             device && !device->supportsEsc()) {
    // SPoE reaches the drive without an EtherCAT Slave Controller, so there is no EEPROM to write.
    reporter.succeed(kSiiStep,
                     std::format("{} not written: this transport has no SII", package->sii->name));
  } else {
    std::expected<void, std::string> written = deviceNotFound(devicePosition);
    if (device) {
      written = device->writeSii(package->sii->content);
    }
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

  // ── activate-firmware ──────────────────────────────────────────────────────────────────────
  // Over EtherCAT, leaving BOOT applies the firmware, and this does nothing. Over SPoE the drive
  // needs an explicit request, which restarts it into the firmware that both binaries hold.
  reporter.start(kActivateFirmwareStep);
  if (!package->appBinary && !package->comBinary) {
    reporter.succeed(kActivateFirmwareStep, "no firmware was written");
    return finish();
  }
  {
    const auto device = deviceManager.deviceAt(devicePosition);
    if (!device) {
      failure = deviceNotFound(devicePosition).error();
      reporter.fail(kActivateFirmwareStep, *failure);
      return finish();
    }
    const auto activation = device->activateFirmware(kActivationTimeout);
    if (!activation) {
      failure = std::format("activating the firmware failed: {}", activation.error());
      reporter.fail(kActivateFirmwareStep, *failure);
      return finish();
    }
    reporter.succeed(kActivateFirmwareStep,
                     *activation == mm::comm::FieldbusDriver::FirmwareActivation::kRestarted
                         ? "the device restarted into the new firmware"
                         : "applied when the device leaves BOOT");
  }

  return finish();
}

std::expected<PlantIdentificationRequest, std::string> parsePlantIdentificationRequest(
    const nlohmann::json& body) {
  auto chirp = parseSystemIdentificationRequest(body);
  if (!chirp) {
    return std::unexpected(chirp.error());
  }
  auto duration =
      plantIdentificationStreamDuration(std::chrono::milliseconds(chirp->transitionTimeMs));
  if (!duration) {
    return std::unexpected(std::format("'transitionTimeMs': {}", duration.error()));
  }
  PlantIdentificationRequest request;
  request.chirp = *chirp;
  request.chirp.start = somanet::SystemIdentificationStart::kAfterHrdStreamStart;
  request.streamDuration = *duration;
  return request;
}

std::vector<ProcedureParameter> plantIdentificationParameters() {
  // The chirp's own parameters, under the same names, so a client that knows one procedure knows
  // the other. Two differences: the run arms the chirp itself, so there is no start to choose, and
  // the chirp must fit in the recording with its padding, so the time has a lower ceiling.
  std::vector<ProcedureParameter> parameters;
  const auto limit =
      somanet::maxHrdStreamDuration(somanet::HrdData::kSystemIdentificationData) - kChirpPaddingMs;
  for (auto& parameter : systemIdentificationParameters()) {
    if (parameter.name == "start") {
      continue;
    }
    if (parameter.name == "transitionTimeMs") {
      parameter.maxValue = limit.count();
      parameter.description = std::format(
          "How long the sweep takes, in milliseconds. The recording adds {} ms around it, and the "
          "drive records at most {} ms, so the sweep may take up to {} ms.",
          kChirpPaddingMs.count(), (limit + kChirpPaddingMs).count(), limit.count());
    }
    parameters.push_back(std::move(parameter));
  }
  return parameters;
}

std::vector<ProgressStep> plantIdentificationSteps() {
  return stepsFrom({kPrepareStep, kReleaseBrakeStep, kPlantIdChirpStep, kPlantIdRecordStep,
                    kRestoreStep, kPlantIdFitStep, kPlantIdStoreStep});
}

namespace {

constexpr std::string_view kPlantIdWhat = "plant identification";

// Wait after a manual brake is commanded released, on top of its pull time. The same margin
// SomanetDrive::releaseBrake adds for a brake the firmware controls.
constexpr auto kBrakeSettle = std::chrono::milliseconds(50);

// The excitation and the recording, with the drive restored on the way out. Split from the fit and
// the store so the restorer's scope ends here: the drive is back in Switched On before the program
// computes, however long that takes.
//
// @return The converted recording, with a warning about its amplitude when there is one.
std::expected<std::pair<PlantMeasurement, std::optional<std::string>>, std::string>
excitePlantAndRecord(SomanetDrive& drive, ProgressReporter& reporter, const std::stop_token& stop,
                     const PlantIdentificationRequest& request, uint32_t ratedTorqueMilliNm) {
  DriveRestorer restorer(drive, reporter, kPlantIdentificationProcedure);
  const auto cancelled = [](std::string_view when) {
    return std::unexpected(std::format("{} was cancelled {}", kPlantIdWhat, when));
  };

  // ── prepare ──
  reporter.start(kPrepareStep);
  const auto failPrepare = [&reporter](std::string reason) {
    reporter.fail(kPrepareStep, reason);
    return std::unexpected(std::move(reason));
  };
  // The recording is configured before the drive is enabled: configuring deletes the files of the
  // previous recording, which can take seconds, and the drive has no reason to be enabled for that.
  if (auto r = drive.configureHrdStream(
          somanet::HrdData::kSystemIdentificationData, request.streamDuration,
          {.timeout = kHrdConfigureTimeout, .pollInterval = kHrdPollInterval, .stop = stop});
      !r) {
    return failPrepare(std::format("could not configure the recording: {}", r.error()));
  }
  auto savedMode = drive.operationModeValue();
  if (!savedMode) {
    return failPrepare(savedMode.error());
  }
  auto state = drive.state();
  if (!state) {
    return failPrepare(state.error());
  }
  // The mode changes under no running controller: an enabled drive is brought down to Switched On
  // first, and goes back up once CST and the zero target are in place.
  if (*state == cia402::State::kOperationEnabled) {
    if (auto r = drive.transitionToState(cia402::State::kSwitchedOn); !r) {
      return failPrepare(
          std::format("the drive did not leave Operation Enabled for Switched On: {}", r.error()));
    }
  }
  if (auto r = drive.setOperationMode(cia402::OperationMode::kCyclicSyncTorque); !r) {
    return failPrepare(r.error());
  }
  restorer.restoreMode(*savedMode, cia402::State::kSwitchedOn);
  if (auto r = drive.setTargetTorque(0); !r) {
    return failPrepare(std::format("could not write a zero target torque: {}", r.error()));
  }
  if (auto r = drive.enable(); !r) {
    return failPrepare(std::format(
        "the drive did not reach Operation Enabled, which the chirp requires: {}", r.error()));
  }
  reporter.succeed(kPrepareStep,
                   nlohmann::json{{"ratedTorqueMilliNm", ratedTorqueMilliNm},
                                  {"streamDurationMs", request.streamDuration.count()},
                                  {"operationModeFound", *savedMode}});
  if (stop.stop_requested()) {
    return cancelled("before the brake");
  }

  // ── release-brake ──
  reporter.start(kReleaseBrakeStep);
  auto brake = drive.brakeState();
  if (!brake) {
    reporter.fail(kReleaseBrakeStep, brake.error());
    return std::unexpected(brake.error());
  }
  if (brake->status == somanet::BrakeStatus::kNotConfigured) {
    reporter.succeed(kReleaseBrakeStep, nlohmann::json{{"brake", "none"}, {"state", *brake}});
  } else if (brake->softwareControllable()) {
    // Strategy 1 or 2: the drive released it when it enabled in CST, and engages it again when it
    // leaves Operation Enabled. Nothing to write, and nothing to restore.
    reporter.succeed(kReleaseBrakeStep,
                     nlohmann::json{{"brake", "released by the drive"}, {"state", *brake}});
  } else {
    // Strategy 0: the brake is a voltage the firmware does not manage, so the release and the
    // wait for the pull time are both this procedure's. Armed before the write, so a write that
    // lands and then fails to read back is still undone.
    restorer.restoreBrake(brake->status);
    if (auto r = drive.setBrakeStatus(somanet::BrakeStatus::kDisengaged); !r) {
      reporter.fail(kReleaseBrakeStep, r.error());
      return std::unexpected(r.error());
    }
    if (!sleepUnlessStopped(brake->pullTime + kBrakeSettle, stop)) {
      return cancelled("while the brake released");
    }
    auto released = drive.brakeState();
    if (!released) {
      reporter.fail(kReleaseBrakeStep, released.error());
      return std::unexpected(released.error());
    }
    reporter.succeed(kReleaseBrakeStep,
                     nlohmann::json{{"brake", "released manually"}, {"state", *released}});
  }
  if (stop.stop_requested()) {
    return cancelled("before the chirp was configured");
  }

  // ── chirp ──
  reporter.start(kPlantIdChirpStep);
  const OsCommandConfig osConfig{.timeout = kEncoderRegisterTimeout,
                                 .pollInterval = kEncoderRegisterPollInterval,
                                 .stop = stop};
  if (auto r = writeChirpSettings(drive, request.chirp, osConfig, kPlantIdWhat); !r) {
    reporter.fail(kPlantIdChirpStep, r.error());
    return std::unexpected(r.error());
  }
  // Armed before the arm is written, so an arm that lands and then fails to be acknowledged is
  // still disarmed on the way out.
  restorer.disarmChirp();
  if (auto r = drive.setSystemIdentificationParameter(
          somanet::SystemIdentificationParameter::kStartProcedure,
          static_cast<uint32_t>(request.chirp.start), osConfig);
      !r) {
    reporter.fail(kPlantIdChirpStep, r.error());
    return std::unexpected(r.error());
  }
  reporter.succeed(kPlantIdChirpStep, chirpSettingsJson(request.chirp));
  if (stop.stop_requested()) {
    return cancelled("before the recording");
  }

  // ── record ──
  reporter.start(kPlantIdRecordStep);
  const auto failRecord = [&reporter](std::string reason) {
    reporter.fail(kPlantIdRecordStep, reason);
    return std::unexpected(std::move(reason));
  };
  if (auto r = drive.startHrdStream({.timeout = request.streamDuration + kHrdRecordMargin,
                                     .pollInterval = kHrdPollInterval,
                                     .stop = stop});
      !r) {
    return failRecord(std::format("the recording did not complete: {}", r.error()));
  }
  if (stop.stop_requested()) {
    return cancelled("during the recording");
  }
  // A drive that left Operation Enabled during the chirp — a fault, a quick stop — recorded
  // something other than the plant's response to it, and the state names the cause.
  if (auto ready = confirmStillEnabled(drive); !ready) {
    return failRecord(ready.error());
  }
  auto recording = drive.readHrdRecording(somanet::HrdData::kSystemIdentificationData);
  if (!recording) {
    return failRecord(std::format("could not read the recording back: {}", recording.error()));
  }
  const auto& samples = std::get<std::vector<HrdSystemIdentificationSample>>(recording->samples);
  auto measurement = convertPlantMeasurement(samples, ratedTorqueMilliNm);
  auto amplitudes = checkPlantMeasurementAmplitudes(measurement, ratedTorqueMilliNm);
  if (!amplitudes) {
    return failRecord(amplitudes.error());
  }
  reporter.succeed(
      kPlantIdRecordStep,
      nlohmann::json{{"samples", samples.size()},
                     {"files", recording->files.size()},
                     {"warning", *amplitudes ? nlohmann::json(**amplitudes) : nlohmann::json()}});
  return std::pair{std::move(measurement), std::move(*amplitudes)};
}

}  // namespace

std::expected<void, std::string> runPlantIdentificationProcedure(
    Device& device, ProgressReporter& reporter, std::stop_token stop,
    const PlantIdentificationRequest& request, const PlantModelFitFn& fitPlantModel) {
  auto drive = createSomanetDrive(device);
  if (!drive) {
    return std::unexpected(drive.error());
  }
  // Read before anything moves: it scales every torque sample, and a zero would make every
  // amplitude check pass on nothing.
  reporter.start(kPrepareStep);
  auto ratedTorque = drive->motorRatedTorque();
  if (!ratedTorque) {
    reporter.fail(kPrepareStep, ratedTorque.error());
    return std::unexpected(ratedTorque.error());
  }
  if (*ratedTorque == 0) {
    const std::string reason =
        "the motor rated torque (0x6076) is 0, so the recorded torque cannot be converted; "
        "configure the motor first";
    reporter.fail(kPrepareStep, reason);
    return std::unexpected(reason);
  }

  auto recorded = excitePlantAndRecord(*drive, reporter, stop, request, *ratedTorque);
  if (!recorded) {
    return std::unexpected(recorded.error());
  }
  const auto& [measurement, warning] = *recorded;

  // ── fit ──
  reporter.start(kPlantIdFitStep);
  const auto failFit = [&reporter](std::string reason) {
    reporter.fail(kPlantIdFitStep, reason);
    return std::unexpected(std::move(reason));
  };
  if (!fitPlantModel) {
    return failFit("the plant model fit is not configured on this server");
  }
  const double startHz = request.chirp.startFrequencyMilliHz / 1000.0;
  const double endHz = request.chirp.targetFrequencyMilliHz / 1000.0;
  auto fit = fitPlantModel(measurement, startHz, endHz);
  if (!fit) {
    return failFit(fit.error());
  }
  if (!isStablePlantModel(fit->denominator)) {
    return failFit(std::format(
        "the identified model is unstable: its denominator {} has a negative coefficient; raise "
        "the amplitude or widen the sweep and run it again",
        nlohmann::json(fit->denominator).dump()));
  }
  PlantModel model;
  model.numerator = std::move(fit->numerator);
  model.denominator = std::move(fit->denominator);
  model.chirp = PlantModelChirp{.startFrequencyHz = startHz,
                                .endFrequencyHz = endHz,
                                .amplitudePermil = request.chirp.targetAmplitudePermil,
                                .durationMs = request.chirp.transitionTimeMs,
                                .signalType = request.chirp.signalType};
  nlohmann::json fitted = model;
  fitted["bode"] = std::move(fit->bode);
  fitted["warning"] = warning ? nlohmann::json(*warning) : nlohmann::json();
  reporter.succeed(kPlantIdFitStep, std::move(fitted));

  // ── store ──
  reporter.start(kPlantIdStoreStep);
  const std::string csv = plantModelCsv(model);
  const std::span<const uint8_t> bytes{reinterpret_cast<const uint8_t*>(csv.data()), csv.size()};
  if (auto r = device.writeFile(std::string(kPlantModelFile), bytes); !r) {
    reporter.fail(kPlantIdStoreStep, r.error().message);
    return std::unexpected(r.error().message);
  }
  // The text as written, so the step shows exactly what the drive holds, and a client that wants
  // a copy of the file does not have to read it back from the drive.
  reporter.succeed(
      kPlantIdStoreStep,
      nlohmann::json{{"file", kPlantModelFile}, {"bytes", csv.size()}, {"content", csv}});
  return {};
}

}  // namespace mm::node
