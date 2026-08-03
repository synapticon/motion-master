#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "node/cia402_drive.h"

namespace mm::node {

/// @brief Pure SOMANET (Synapticon manufacturer-specific) vocabulary — object indices, sub-entry
///        numbers and enumerations, with no dependency on @c Device.
///
/// The vendor counterpart of @c mm::node::cia402, and nested rather than flat for one concrete
/// reason: SOMANET extends 0x6060 with modes CiA402 does not define, so @c somanet::OperationMode
/// has to sit beside @c cia402::OperationMode without colliding with it.
namespace somanet {

/// @brief Manufacturer-specific object indices (the 0x2000-0x5FFF vendor range).
enum Object : uint16_t {
  kBrakeOptions = 0x2004,  ///< RECORD — brake voltages, timing, release strategy and status.
};

/// @brief Sub-entries of 0x2004 (brake options). The enum value @b is the subindex.
enum BrakeOption : uint8_t {
  kBrakePullVoltage = 1,      ///< UNSIGNED32, mV — voltage that disengages the brake.
  kBrakeHoldVoltage = 2,      ///< UNSIGNED32, mV — lower voltage that keeps it disengaged.
  kBrakePullTime = 3,         ///< UNSIGNED16, ms — how long the pull voltage is applied.
  kBrakeReleaseStrategy = 4,  ///< UNSIGNED8 — how the brake is driven; see BrakeReleaseStrategy.
  kBrakeControllerDisableDelay = 5,  ///< UNSIGNED16, ms — engage-to-controller-off delay.
  kBrakeStatus = 7,  ///< UNSIGNED8 — reports *and* commands the brake; see BrakeStatus.
  kBrakeMinimumDisplacement = 8,  ///< UNSIGNED32 — pin-brake release travel threshold.
  kBrakeMaximumTorque = 9,        ///< UNSIGNED16, mNm — torque ceiling during a pin-brake release.
  kBrakeOutputVoltage = 10,       ///< UNSIGNED16, mV — phase-D voltage in manual mode.
  kBrakeSwitchingFrequency = 11,  ///< UNSIGNED8 — phase-D PWM rate (0: 16, 1: 32, 2: 64 kHz).
};

/// @brief How the brake is driven (0x2004:04).
///
/// @c kManualOutputVoltage is the one that matters to a caller: the brake is not under firmware
/// control at all, so commanding @c BrakeStatus does nothing and release/engage are no-ops.
enum class BrakeReleaseStrategy : uint8_t {
  kManualOutputVoltage = 0,  ///< Not firmware-controlled — a raw phase-D voltage (0x2004:10).
  kClutch = 1,               ///< Standard brake: pull voltage for the pull time, then hold voltage.
  kPin = 2,                  ///< Pin brake — releasing it *moves the shaft* (see releaseBrake).
};

/// @brief Brake state (0x2004:07), which both reports the brake and commands it when written.
enum class BrakeStatus : uint8_t {
  kNotConfigured = 0,  ///< No brake configured.
  kEngaged = 1,        ///< Holding. A brake is spring-engaged, so this is its powered-off state.
  kDisengaged = 2,     ///< Released.
};

/// @brief Human-readable name of a brake status (for logging / JSON). Never returns @c nullptr.
constexpr std::string_view toString(BrakeStatus status) {
  switch (status) {
    case BrakeStatus::kNotConfigured:
      return "notConfigured";
    case BrakeStatus::kEngaged:
      return "engaged";
    case BrakeStatus::kDisengaged:
      return "disengaged";
  }
  return "unknown";
}

/// @brief Human-readable name of a release strategy (for logging / JSON). Never @c nullptr.
constexpr std::string_view toString(BrakeReleaseStrategy strategy) {
  switch (strategy) {
    case BrakeReleaseStrategy::kManualOutputVoltage:
      return "manualOutputVoltage";
    case BrakeReleaseStrategy::kClutch:
      return "clutch";
    case BrakeReleaseStrategy::kPin:
      return "pin";
  }
  return "unknown";
}

/// @brief OS command IDs (byte 0 of 0x1023:01).
///
/// Only the commands this codebase issues are listed; the firmware implements more. The numbering
/// is the firmware's, so gaps are real rather than reserved space.
enum class OsCommandId : uint8_t {
  kMotorPhaseOrderDetection = 4,    ///< Detects the motor phase order. Rotates the rotor.
  kOpenPhaseDetection = 6,          ///< Checks every motor phase and FET leg for an open circuit.
  kPolePairDetection = 7,           ///< Detects the motor's pole pair count. Rotates the rotor.
  kPhaseResistanceMeasurement = 8,  ///< Measures the motor's phase resistance, in milliohms.
  kPhaseInductanceMeasurement = 9,  ///< Measures the motor's phase inductance, in microhenries.
  kSkippedCycleCounter = 13,        ///< Reads the drive's skipped-cycle counter. Harmless; no
                                    ///< motion.
};

/// @brief The faults open phase detection (command 6) reports, as its command-specific OS error
/// code.
///
/// Command-specific codes count up from 0 and mean nothing outside their own command — general
/// codes
/// (@c OsCommandError) count down from 254 — so this table is only valid for command 6.
enum class OpenPhaseFault : uint8_t {
  kOpenTerminalA = 0,  ///< Terminal A of the drive is not connected.
  kOpenTerminalB = 1,  ///< Terminal B of the drive is not connected.
  kOpenTerminalC = 2,  ///< Terminal C of the drive is not connected.
  kOpenFetAHigh = 3,   ///< Upper FET in leg A is not conducting (open circuit fault).
  kOpenFetALow = 4,    ///< Lower FET in leg A is not conducting (open circuit fault).
  kOpenFetBHigh = 5,   ///< Upper FET in leg B is not conducting (open circuit fault).
  kOpenFetBLow = 6,    ///< Lower FET in leg B is not conducting (open circuit fault).
  kOpenFetCHigh = 7,   ///< Upper FET in leg C is not conducting (open circuit fault).
  kOpenFetCLow = 8,    ///< Lower FET in leg C is not conducting (open circuit fault).
};

/// @brief Name of an open-phase fault, as the console and logs should render it. Never @c nullptr.
constexpr std::string_view toString(OpenPhaseFault fault) {
  switch (fault) {
    case OpenPhaseFault::kOpenTerminalA:
      return "open terminal A";
    case OpenPhaseFault::kOpenTerminalB:
      return "open terminal B";
    case OpenPhaseFault::kOpenTerminalC:
      return "open terminal C";
    case OpenPhaseFault::kOpenFetAHigh:
      return "open FET A high";
    case OpenPhaseFault::kOpenFetALow:
      return "open FET A low";
    case OpenPhaseFault::kOpenFetBHigh:
      return "open FET B high";
    case OpenPhaseFault::kOpenFetBLow:
      return "open FET B low";
    case OpenPhaseFault::kOpenFetCHigh:
      return "open FET C high";
    case OpenPhaseFault::kOpenFetCLow:
      return "open FET C low";
  }
  return "unknown";
}

/// @brief What an open-phase fault means, for a message a user has to act on. Never @c nullptr.
constexpr std::string_view describe(OpenPhaseFault fault) {
  switch (fault) {
    case OpenPhaseFault::kOpenTerminalA:
      return "terminal A of the drive is not connected";
    case OpenPhaseFault::kOpenTerminalB:
      return "terminal B of the drive is not connected";
    case OpenPhaseFault::kOpenTerminalC:
      return "terminal C of the drive is not connected";
    case OpenPhaseFault::kOpenFetAHigh:
      return "the upper FET in leg A is not conducting (open circuit fault)";
    case OpenPhaseFault::kOpenFetALow:
      return "the lower FET in leg A is not conducting (open circuit fault)";
    case OpenPhaseFault::kOpenFetBHigh:
      return "the upper FET in leg B is not conducting (open circuit fault)";
    case OpenPhaseFault::kOpenFetBLow:
      return "the lower FET in leg B is not conducting (open circuit fault)";
    case OpenPhaseFault::kOpenFetCHigh:
      return "the upper FET in leg C is not conducting (open circuit fault)";
    case OpenPhaseFault::kOpenFetCLow:
      return "the lower FET in leg C is not conducting (open circuit fault)";
  }
  return "unknown fault";
}

/// @brief Which way round the motor's phases are wired, as motor phase order detection (command 4)
///        reports it and as 0x2003:05 "Motor phases inverted" stores it.
///
/// "Inverted" means the sensor angle and the rotor angle move in opposite directions; normal means
/// they increase together.
enum class MotorPhaseOrder : uint8_t {
  kNormal = 0,    ///< Sensor and rotor angles change in the same direction.
  kInverted = 1,  ///< They change in opposite directions.
};

/// @brief Name of a motor phase order (for logging / JSON). Never returns @c nullptr.
constexpr std::string_view toString(MotorPhaseOrder order) {
  switch (order) {
    case MotorPhaseOrder::kNormal:
      return "normal";
    case MotorPhaseOrder::kInverted:
      return "inverted";
  }
  return "unknown";
}

/// @brief The faults the current-injecting motor measurements report as their command-specific OS
///        error code — pole pair (7), phase resistance (8) and phase inductance (9), which the
///        firmware specification gives the same single-entry table.
///
/// A table shared between commands is only safe *because* the specification gives them the same
/// one; command-specific codes count up from 0 and otherwise mean nothing outside the command that
/// issued them (general codes count down from 254 — see @c OsCommandError). Open phase detection's
/// code 0 means "open terminal A", which is exactly why each command decodes its own table.
enum class MotorMeasurementFault : uint8_t {
  kCurrentAmplitudeError = 0,  ///< The drive could not reach the current amplitude it needed.
};

/// @brief Name of a motor-measurement fault, as the console and logs should render it. Never
///        @c nullptr.
constexpr std::string_view toString(MotorMeasurementFault fault) {
  switch (fault) {
    case MotorMeasurementFault::kCurrentAmplitudeError:
      return "current amplitude error";
  }
  return "unknown";
}

/// @brief What a motor-measurement fault means, for a message a user has to act on. Never
///        @c nullptr.
constexpr std::string_view describe(MotorMeasurementFault fault) {
  switch (fault) {
    case MotorMeasurementFault::kCurrentAmplitudeError:
      return "the drive could not raise the motor phase currents to the amplitude it needed, which "
             "a limited DC-link voltage or a high motor phase impedance can both cause";
  }
  return "unknown fault";
}

/// @brief SOMANET's manufacturer-specific operation modes — the negative half of 0x6060, which
///        CiA402 leaves to the vendor.
///
/// Deliberately a separate enum rather than extra enumerators on @c cia402::OperationMode: that one
/// is the standard set, and @c cia402::toOperationMode exists so an API boundary can reject a mode
/// it does not know. Folding these in would make every vendor mode validate as a CiA402 mode
/// everywhere the standard enum is used, including the console's mode list.
enum class OperationMode : int8_t {
  kCyclicSyncOutputTorque = -110,
  kCyclicSyncOutputVelocity = -109,
  kCyclicSyncOutputPosition = -108,
  kProfileOutputVelocity = -103,
  kProfileOutputPosition = -101,
  kSystemIdentification = -4,
  kOpenLoopField = -3,
  kDiagnostics = -2,  ///< Where the master owns the brake and the measurement OS commands run.
  kCoggingCompensationRecording = -1,
};

}  // namespace somanet

/// @brief Byte length of the OS command and response octet strings (0x1023:01 / 0x1023:03).
///        Manufacturer-specific — the firmware specification allows it to grow, so read it from
///        here rather than writing 8 at a call site.
inline constexpr size_t kOsCommandSize = 8;

/// @brief Terminal OS command status — byte 0 of the response (0x1023:03), which the firmware
///        mirrors from 0x1023:02. Only these four values end a command; 100-200 (executing, with
///        percentage) and 255 (executing) are transient and never surface to a caller.
enum class OsCommandStatus : uint8_t {
  kCompleted = 0,          ///< Completed, no error, no response data.
  kCompletedWithData = 1,  ///< Completed, no error, response data available.
  kFailed = 2,             ///< Completed with error, no response data.
  kFailedWithData = 3,     ///< Completed with error, OS error code and response data available.
};

/// @brief The general OS error codes — the ones any command can report, counting down from 254.
///        Command-specific codes count *up* from 0 and are named by the command that issued them,
///        not here (open phase detection's 0 is "open terminal A", phase resistance's 0 is
///        "current amplitude error", ...).
enum class OsCommandError : uint8_t {
  kNotAllowed = 251,   ///< Preconditions not met (wrong mode of operation, say).
  kAborted = 252,      ///< The abort the master requested via 0x1024 was carried out.
  kTimeout = 253,      ///< No downstream service acknowledged the command (or the abort) in 5 s.
  kUnsupported = 254,  ///< The command ID does not exist on this drive.
  kReserved = 255,     ///< Reserved for future expansion of the feature.
};

/// @brief Names a general OS error code, or @c std::nullopt when @p code is command-specific.
///
/// A caller that knows which command it issued should try its own error table first and fall back
/// to this one; a caller that does not can report the raw code alongside whatever this returns.
std::optional<std::string_view> osCommandErrorName(uint8_t code);

/// @brief A completed OS command's response — the decoded form of 0x1023:03.
///
/// @c status is the terminal status byte; @c data is the service response payload (bytes 2-7 when
/// the command succeeded with data, bytes 3-7 when it failed with data, empty otherwise — byte 1
/// is unused by the protocol); @c errorCode is the OS error code, present only for
/// @c kFailedWithData.
struct OsCommandResponse {
  OsCommandStatus status{OsCommandStatus::kCompleted};  ///< Terminal status (response byte 0).
  std::vector<uint8_t> data;                            ///< Service response payload, if any.
  std::optional<uint8_t> errorCode;                     ///< OS error code, if the drive sent one.

  /// @brief Whether the drive reported the command as failed (status 2 or 3). A failed command is
  ///        still a response — the drive rendered a verdict — so it arrives as a value, not an
  ///        error; consult @c errorCode for the reason.
  bool failed() const {
    return status == OsCommandStatus::kFailed || status == OsCommandStatus::kFailedWithData;
  }
};

/// @brief Timing and cancellation for @c SomanetDrive::runOsCommand.
///
/// @c timeout is a ceiling on the whole command, not a liveness check — the drive fails a command
/// no downstream service acknowledges within 5 s on its own — so size it for the command being
/// run (milliseconds for a register read, tens of seconds for a measurement). Hitting it, or a
/// stop request on @c stop, makes the master abort the running command; @c abortTimeout then
/// bounds how long the drive is given to report that abort (its own internal abort path is
/// bounded by 5 s).
struct OsCommandConfig {
  std::chrono::milliseconds timeout{1000};        ///< Ceiling on the whole command.
  std::chrono::milliseconds pollInterval{10};     ///< Delay between response polls.
  std::chrono::milliseconds abortTimeout{10000};  ///< Time allowed to confirm a forced abort.
  std::stop_token stop{};  ///< Requesting a stop aborts the command; default never stops.
};

/// @brief The brake's configuration and its current state — one read of the parts of 0x2004 that
///        decide what a release or engage will actually do.
///
/// Returned by the brake operations as well as by @c brakeState, so a caller always learns the
/// outcome from the same shape. It is also the answer to "why did nothing happen?": a release is a
/// no-op when the brake is not firmware-controlled, and @c softwareControllable says so without the
/// caller having to know that strategy 0 means manual.
struct BrakeState {
  somanet::BrakeStatus status{somanet::BrakeStatus::kNotConfigured};  ///< 0x2004:07.
  somanet::BrakeReleaseStrategy releaseStrategy{
      somanet::BrakeReleaseStrategy::kManualOutputVoltage};  ///< 0x2004:04.
  std::chrono::milliseconds pullTime{};                      ///< 0x2004:03.
  uint32_t pullVoltageMv = 0;                                ///< 0x2004:01.
  uint32_t holdVoltageMv = 0;                                ///< 0x2004:02.

  /// @brief Whether the firmware drives the brake, i.e. whether commanding 0x2004:07 does anything.
  ///        False for @c kManualOutputVoltage, where the brake is a raw phase-D voltage instead.
  bool softwareControllable() const {
    return releaseStrategy != somanet::BrakeReleaseStrategy::kManualOutputVoltage;
  }

  /// @brief Whether releasing this brake turns the motor — true only for a pin brake, whose release
  ///        procedure lifts the load off the pin under progressively raised torque.
  bool releaseMovesShaft() const { return releaseStrategy == somanet::BrakeReleaseStrategy::kPin; }
};
void to_json(nlohmann::json& j, const BrakeState& state);

/// @brief What open phase detection found.
///
/// The command's verdict is inverted relative to every other OS command, and that is the firmware's
/// design rather than a quirk to paper over: the drive reports **success when no phase is open**,
/// and reports a *failure* whose command-specific error code names which terminal or FET leg is
/// open. So a failed OS command here is a completed measurement with a negative finding, not a
/// malfunction — which is why this is a value and not an error.
struct OpenPhaseResult {
  bool phaseOpened = false;  ///< True when the drive found an open phase.

  /// Which fault, when the drive reported a code this build names. Absent for a code it does not,
  /// which @c faultCode still carries — a firmware that grows the table stays reportable.
  std::optional<somanet::OpenPhaseFault> fault;
  std::optional<uint8_t> faultCode;  ///< The raw code, present whenever @c phaseOpened.

  /// @brief One line describing the finding, ready to put in front of a user.
  std::string describe() const;
};
void to_json(nlohmann::json& j, const OpenPhaseResult& result);

/// @brief What motor phase order detection (command 4) found.
///
/// Unlike the other measurements, **the drive stores this one**: on success the firmware writes the
/// detected order into 0x2003:05 "Motor phases inverted" itself, so a successful run has
/// reconfigured the drive rather than merely reported a number.
struct MotorPhaseOrderResult {
  somanet::MotorPhaseOrder order{somanet::MotorPhaseOrder::kNormal};  ///< What the drive detected.

  /// @brief Whether the phases are inverted — the one bit a caller normally branches on.
  bool inverted() const { return order == somanet::MotorPhaseOrder::kInverted; }

  /// @brief One line describing the finding, ready to put in front of a user.
  std::string describe() const;
};
void to_json(nlohmann::json& j, const MotorPhaseOrderResult& result);

/// @brief What pole pair detection (command 7) found.
struct PolePairResult {
  uint8_t polePairs = 0;  ///< Pole pairs the drive counted.

  /// @brief One line describing the finding, ready to put in front of a user.
  std::string describe() const;
};
void to_json(nlohmann::json& j, const PolePairResult& result);

/// @brief What phase resistance measurement (command 8) measured.
///
/// The drive reports a whole number of milliohms and that is what is stored: the unit is named in
/// the member rather than applied to it, because scaling to ohms here would invent precision the
/// drive never reported and force every consumer to scale back to compare two readings.
struct PhaseResistanceResult {
  uint32_t milliohms = 0;  ///< Phase resistance in mΩ, exactly as the drive reported it.

  /// @brief One line describing the measurement, ready to put in front of a user.
  std::string describe() const;
};
void to_json(nlohmann::json& j, const PhaseResistanceResult& result);

/// @brief What phase inductance measurement (command 9) measured.
///
/// Microhenries as the drive reported them, for the same reason @c PhaseResistanceResult keeps
/// milliohms: the unit belongs in the member name, not in a conversion that would invent precision.
struct PhaseInductanceResult {
  uint32_t microhenries = 0;  ///< Phase inductance in µH, exactly as the drive reported it.

  /// @brief One line describing the measurement, ready to put in front of a user.
  std::string describe() const;
};
void to_json(nlohmann::json& j, const PhaseInductanceResult& result);

/// @brief Borrowed view of a SOMANET drive — a CiA402 drive plus Synapticon-specific
///        object-dictionary access (encoder/motor configuration, custom OS commands, etc.).
///
/// SOMANET drives implement CiA402 in full, so this *is-a* @c Cia402Drive and inherits the whole
/// state machine and setpoint surface; it adds only the vendor-specific objects in the
/// manufacturer range. Like every profile view it is a thin, stateless borrow over a @c Device
/// (see @c ProfileDevice) — construct it for one operation and drop it.
///
/// Construct via @c createSomanetDrive, which checks the vendor ID before binding.
///
/// **Multi-cycle procedures do not live here.** Commutation-offset detection, auto-tuning, and
/// the like are command-and-wait procedures that run off-RT on a background thread, taking a
/// @c DeviceManager& and re-resolving their @c Device each step (a long-lived view would dangle
/// across a bus rescan). Whoever owns that procedure runs it; this view's job is the synchronous,
/// single-shot SOMANET object access that frames those procedures (reading/writing config,
/// checking preconditions).
class SomanetDrive : public Cia402Drive {
 public:
  /// @brief Binds an (unchecked) SOMANET view to @p device. Prefer @c createSomanetDrive.
  explicit SomanetDrive(Device& device) : Cia402Drive(device) {}

  /// @brief Issues an OS command (0x1023) and blocks until the drive reports it finished.
  ///
  /// The mechanism behind every typed SOMANET command: write the 8-byte request to 0x1023:01,
  /// poll 0x1023:03 until its status byte goes terminal, decode the response. The objects are
  /// CiA301, but everything about how they are driven is Synapticon firmware behaviour — the
  /// status mirrored into response byte 0, the 100-200 percentage band, the OS error code in
  /// byte 2, the eight-byte payloads — which is why this lives on the vendor view rather than on
  /// @c ProfileDevice beside the raw accessors it is built from.
  ///
  /// Two firmware rules (both stated in the OS command specification) shape the implementation and
  /// are easy to break by "tidying" it:
  /// - **Only reading 0x1023:03 re-arms 0x1023:01.** The status byte is mirrored in 0x1023:02, but
  ///   reading *that* does not make the command object writable again — the drive returns to its
  ///   idle state only once the response has been read — so a command is never polled through
  ///   0x1023:02, and 0x1023:03 is read even when its payload is not wanted.
  /// - **0x1024 must be 0 for a command to be accepted**, and a write to 0x1023:01 while it is not
  ///   is *silently ignored* rather than refused. That would make the next poll read the previous
  ///   command's terminal status and report it as this command's response, so the mode is written
  ///   to 0 before every command, and an abort (mode 3) is followed by a restore to 0 on every
  ///   path out.
  ///
  /// No pre-check is made for a command already in progress: the drive enforces that itself by
  /// aborting the write to 0x1023:01 (SDO abort 0x08000021, "local control"), and that error is
  /// forwarded verbatim. Progress reported by the drive (status 100-200) is logged at debug level
  /// as it changes and is not otherwise surfaced — this call reports only the final outcome.
  ///
  /// Blocks the calling thread for up to @c config.timeout (plus @c config.abortTimeout if the
  /// command has to be aborted). Control-plane only: it sleeps between polls, but each poll takes
  /// the driver's lock for one transaction, so it never blocks the RT loop. Requires the mailbox
  /// to be active (PRE-OP/SAFE-OP/OP).
  ///
  /// @param command  The 8-byte request: byte 0 is the OS command ID, bytes 1-7 its parameters.
  /// @param config   Timing and cancellation (see @c OsCommandConfig).
  /// @return The decoded response — including one the drive marked failed, which is a verdict and
  ///         not a transport error (check @c OsCommandResponse::failed). An error string if the
  ///         command was not run or produced no verdict: a malformed request, an SDO failure
  ///         (forwarded as-is), an unknown status byte, a timeout, or a cancellation.
  std::expected<OsCommandResponse, std::string> runOsCommand(const std::vector<uint8_t>& command,
                                                             const OsCommandConfig& config = {});

  // --- Brake (0x2004) --------------------------------------------------------------------------

  /// @brief Reads the whole brake configuration and its current state in one call.
  std::expected<BrakeState, std::string> brakeState() const;

  /// @brief Reads the brake state (0x2004:07).
  std::expected<somanet::BrakeStatus, std::string> brakeStatus() const;

  /// @brief Commands the brake by writing 0x2004:07 — the raw write, with no wait and no checks.
  ///
  /// Prefer @c releaseBrake / @c engageBrake, which apply the timing the firmware requires. This is
  /// for restoring a previously captured status, where the point is to write exactly what was read.
  std::expected<void, std::string> setBrakeStatus(somanet::BrakeStatus status);

  /// @brief Releases (disengages) the brake and waits for it to have done so.
  ///
  /// Writes @c kDisengaged to 0x2004:07, then waits the drive's pull time (0x2004:03) plus @p
  /// settle before returning, because the firmware blocks motion — and motion-related OS commands —
  /// until the pull time expires. **Release is open-loop**: nothing confirms the brake actually let
  /// go, so that wait is the only margin there is, which is why @p settle is a parameter and not a
  /// constant.
  ///
  /// Does nothing when the release strategy is @c kManualOutputVoltage (the brake is not
  /// firmware-controlled) or when it is already disengaged. That is not an error, and the returned
  /// state is how a caller tells: @c BrakeState::softwareControllable is false in the first case.
  ///
  /// **Two preconditions that make this a no-op rather than a failure if unmet**, both from the
  /// SOMANET brake documentation: the release procedure runs only while the drive is in OP ENABLED,
  /// and in any other state the write merely energises phase D. In *diagnostics* mode entering OP
  /// ENABLED does **not** release the brake automatically the way normal operation does — which is
  /// exactly why a diagnostics procedure has to call this at all.
  ///
  /// **On a pin brake (@c kPin) this moves the shaft.** The controller raises torque progressively
  /// until the load has lifted off the pin by the minimum displacement (0x2004:08), reversing
  /// direction if it hits the torque ceiling (0x2004:09) first. Releasing a brake is not
  /// electrically passive on that strategy.
  ///
  /// Control-plane only: it sleeps. Requires an active mailbox.
  ///
  /// @param settle  Extra wait on top of the drive's pull time.
  /// @return The brake state read back afterwards, or why the attempt failed.
  std::expected<BrakeState, std::string> releaseBrake(
      std::chrono::milliseconds settle = std::chrono::milliseconds(50));

  /// @brief Engages the brake and waits @p settle for it to bite.
  ///
  /// Writes @c kEngaged to 0x2004:07 and waits. There is no pull time on the way in — the brake is
  /// spring-engaged, so engaging is removing voltage — so the wait is only @p settle. Like
  /// @c releaseBrake it does nothing when the strategy is @c kManualOutputVoltage.
  ///
  /// @param settle  How long to wait after commanding the brake before returning.
  /// @return The brake state read back afterwards, or why the attempt failed.
  std::expected<BrakeState, std::string> engageBrake(
      std::chrono::milliseconds settle = std::chrono::milliseconds(50));

  // --- Vendor operation modes (0x6060) ---------------------------------------------------------

  // Keep the inherited standard-mode setter visible: declaring an overload below would otherwise
  // hide it, since name lookup stops at the first scope that has a match.
  using Cia402Drive::setOperationMode;

  /// @brief Requests one of SOMANET's manufacturer-specific operation modes (0x6060).
  std::expected<void, std::string> setOperationMode(somanet::OperationMode mode);

  // --- Typed OS commands -----------------------------------------------------------------------

  /// @brief Runs open phase detection (OS command 6) and decodes its verdict.
  ///
  /// Checks every motor terminal and FET leg for an open circuit. **The drive answers success when
  /// nothing is open**, and answers with a *failed* command whose command-specific error code names
  /// the offending terminal or FET when something is — so a failure here is a finding, and this
  /// returns it as an @c OpenPhaseResult value rather than an error. An error comes back only when
  /// the command could not be run or produced no verdict at all (a general OS error, a timeout, a
  /// cancellation, an SDO failure).
  ///
  /// Preconditions, all of which the drive enforces by refusing with OS error 251 ("command not
  /// allowed") rather than by misbehaving: operation mode @c somanet::OperationMode::kDiagnostics,
  /// CiA402 state Operation Enabled, and no limit switch active. **A released brake is deliberately
  /// not among them.** The firmware specification does not list one for this command, and says only
  /// that it "might rotate the motor if there is no brake, or if it's disengaged" — so an engaged
  /// brake does not prevent the check, it merely keeps the shaft still while it runs, which is the
  /// safer way to run it. Contrast pole pair (7) and motor phase order (4), whose restrictions do
  /// require a disengaged brake.
  ///
  /// It may turn the motor if the brake is disengaged and nothing else holds the shaft.
  ///
  /// @param config  Timing and cancellation. The default timeout is sized for this command.
  /// @return What the detection found, or why no verdict was reached.
  std::expected<OpenPhaseResult, std::string> runOpenPhaseDetection(
      const OsCommandConfig& config = {.timeout = std::chrono::seconds(10),
                                       .pollInterval = std::chrono::milliseconds(100)});

  /// @brief Runs motor phase order detection (OS command 4) and decodes what it found.
  ///
  /// Determines whether the motor's phases are wired normally or inverted, by turning the rotor and
  /// comparing which way the sensor angle moves. **A successful run reconfigures the drive**: the
  /// firmware writes the detected order into 0x2003:05 itself, which is the point of running it —
  /// commutation offset measurement (command 5) requires it to have been done.
  ///
  /// Preconditions, all enforced by the drive refusing with OS error 251: operation mode
  /// @c somanet::OperationMode::kDiagnostics, CiA402 state Operation Enabled, no limit switch
  /// active,
  /// **and the brake disengaged** if one is configured (see @c releaseBrake — in diagnostics mode
  /// enabling the drive does not release it).
  ///
  /// **This command rotates the rotor.** As with pole pair detection the specification states it
  /// outright rather than as a possibility.
  ///
  /// This command has **no command-specific error codes at all** — a failure can only carry a
  /// general one (251 "command not allowed", 253 timeout, ...), so an error from here always names
  /// a reason the command did not run rather than something about the motor.
  ///
  /// @param config  Timing and cancellation. The default timeout is sized for this command.
  /// @return The detected phase order, or why none was produced.
  std::expected<MotorPhaseOrderResult, std::string> runMotorPhaseOrderDetection(
      const OsCommandConfig& config = {.timeout = std::chrono::seconds(60),
                                       .pollInterval = std::chrono::milliseconds(100)});

  /// @brief Runs pole pair detection (OS command 7) and decodes the count it reports.
  ///
  /// Counts the connected motor's pole pairs by turning the rotor. A failed command is an error
  /// rather than a finding, as with the winding measurements: the command either answers with a
  /// count or does not answer at all.
  ///
  /// Preconditions, all enforced by the drive refusing with OS error 251: operation mode
  /// @c somanet::OperationMode::kDiagnostics, CiA402 state Operation Enabled, no limit switch
  /// active, **and the brake disengaged** if one is configured. That last one is a real requirement
  /// here — unlike open phase detection and the winding measurements, whose restrictions omit it —
  /// because in diagnostics mode enabling the drive does not release the brake the way normal
  /// operation does, so a caller has to release it explicitly (see @c releaseBrake).
  ///
  /// **This command turns the rotor.** Not "may": the specification says it needs to, so the shaft
  /// must be free to move and whatever it drives must be safe to move with it.
  ///
  /// @param config  Timing and cancellation. The default timeout is sized for this command.
  /// @return The pole pair count, or why none was produced.
  std::expected<PolePairResult, std::string> runPolePairDetection(
      const OsCommandConfig& config = {.timeout = std::chrono::seconds(60),
                                       .pollInterval = std::chrono::milliseconds(100)});

  /// @brief Runs phase resistance measurement (OS command 8) and decodes the value it reports.
  ///
  /// Measures the resistance of one motor phase by driving current into the windings and observing
  /// what it takes. Unlike open phase detection, a failed command here is a plain failure and not a
  /// finding: this command either answers with a value or does not answer at all, so a refusal
  /// comes back as an error naming the reason — the drive's one command-specific code is
  /// @c somanet::PhaseMeasurementFault::kCurrentAmplitudeError, and a general code (251 "command
  /// not allowed", 253 timeout, ...) is named too.
  ///
  /// Preconditions, all enforced by the drive refusing with OS error 251: operation mode
  /// @c somanet::OperationMode::kDiagnostics, CiA402 state Operation Enabled, and no limit switch
  /// active. **The brake is deliberately not among them** — the firmware specification does not ask
  /// for it, and an engaged brake holding the shaft during the measurement is the safer state — so
  /// a caller should leave the brake alone rather than release it out of symmetry with the commands
  /// that do require it (motor phase order, pole pair).
  ///
  /// It may turn the motor if the brake is disengaged and nothing else holds the shaft.
  ///
  /// @param config  Timing and cancellation. The default timeout is sized for this command.
  /// @return The measured resistance, or why no measurement was produced.
  std::expected<PhaseResistanceResult, std::string> runPhaseResistanceMeasurement(
      const OsCommandConfig& config = {.timeout = std::chrono::seconds(30),
                                       .pollInterval = std::chrono::milliseconds(100)});

  /// @brief Runs phase inductance measurement (OS command 9) and decodes the value it reports.
  ///
  /// The companion of @c runPhaseResistanceMeasurement in every respect that matters — same
  /// preconditions (diagnostics mode, Operation Enabled, no limit switch, and **no brake
  /// requirement**), same command-specific fault
  /// (@c somanet::PhaseMeasurementFault::kCurrentAmplitudeError), and a failure is likewise an
  /// error rather than a finding. Only the quantity differs: microhenries instead of milliohms.
  ///
  /// It may turn the motor if the brake is disengaged and nothing else holds the shaft.
  ///
  /// @param config  Timing and cancellation. The default timeout is sized for this command.
  /// @return The measured inductance, or why no measurement was produced.
  std::expected<PhaseInductanceResult, std::string> runPhaseInductanceMeasurement(
      const OsCommandConfig& config = {.timeout = std::chrono::seconds(30),
                                       .pollInterval = std::chrono::milliseconds(100)});

  /// @brief Reads the requested operation mode (0x6060) as its raw value.
  ///
  /// For saving a mode in order to put it back later, where the point is *not* to interpret it: the
  /// drive may be in a standard mode or a vendor one, and a round trip through either enum would
  /// have to decide which. Use @c operationMode() (inherited) or @c somanet::OperationMode when the
  /// mode is being reasoned about rather than preserved.
  std::expected<int8_t, std::string> operationModeValue() const;

  /// @brief Writes a raw operation-mode value to 0x6060 — the counterpart of @c operationModeValue.
  std::expected<void, std::string> setOperationModeValue(int8_t mode);

  // SOMANET-specific synchronous object-dictionary accessors are added here as their object
  // indices are confirmed (encoder type/resolution, motor pole pairs, commutation offset, ...).
  // Each is a thin typed read/write over device(); none holds state.
};

/// @brief Validates that @p device is a SOMANET drive, then binds a view to it.
///
/// Requires the vendor ID to be Synapticon's (@c kSynapticonVendorId) and the device to satisfy
/// the CiA402 check (@c createCia402Drive). Both are offline-safe — vendor ID is immutable
/// identity read at scan, the CiA402 check reads the parameter map — so no bus I/O is performed.
///
/// @param device  Device to view. The reference must outlive the returned view.
/// @return A @c SomanetDrive bound to @p device, or an error string if it is not a SOMANET drive.
std::expected<SomanetDrive, std::string> createSomanetDrive(Device& device);

}  // namespace mm::node
