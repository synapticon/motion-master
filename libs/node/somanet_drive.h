#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
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
  kCommutationAngleOffset = 0x2001,  ///< INTEGER16 — the offset command 5 measures and writes.
  kMotorSpecificSettings = 0x2003,   ///< RECORD — motor configuration; :05 is the phase order.
  kBrakeOptions = 0x2004,            ///< RECORD — brake voltages, timing, release strategy, status.
  kCommutationOffsetDetection = 0x2009,  ///< RECORD — how commutation offset (5) is measured.
  kErrorReport = 0x203F,                 ///< RECORD — :01 describes the drive's most recent fault.
};

/// @brief Sub-entries of 0x203F (error report). The enum value @b is the subindex.
enum ErrorReportEntry : uint8_t {
  kErrorReportDescription = 1,  ///< STRING — what the drive says about its most recent fault.
};

/// @brief Sub-entries of 0x2003 (motor specific settings). The enum value @b is the subindex.
///
/// Types are the drive's own, read off a SOMANET Integro's object dictionary rather than assumed.
/// Worth knowing that **this is where the motor measurements belong**: pole pair detection (7),
/// phase resistance (8) and phase inductance (9) report a value without storing it, and :01, :03
/// and :04 are the entries it goes in.
enum MotorSetting : uint8_t {
  kMotorPolePairs = 1,        ///< UNSIGNED8 — where a pole pair count (command 7) belongs.
  kMotorTorqueConstant = 2,   ///< INTEGER32 — where a torque constant (command 10) belongs.
  kMotorPhaseResistance = 3,  ///< INTEGER32 — where a phase resistance (command 8) belongs.
  kMotorPhaseInductance = 4,  ///< INTEGER32 — where a phase inductance (command 9) belongs.
  kMotorPhasesInverted = 5,   ///< BOOLEAN — the phase order motor phase order detection (4) writes.
};

/// @brief Sub-entries of 0x2009 (commutation offset detection). The enum value @b is the subindex.
///
/// Types and names are the drive's own, read off a SOMANET Integro rather than assumed — and none
/// of them is what a reader would guess. The method is **signed** (INTEGER8), the state and torque
/// percentage are INTEGER16 rather than byte-sized, and the three gains are REAL32 floats.
enum CommutationOffsetSetting : uint8_t {
  kCommutationOffsetState = 1,  ///< INTEGER16 — set to OFFSET_VALID by a successful command 5.

  /// INTEGER16, 0-100, default 100 — how hard the measurement drives the motor. The drive names
  /// this entry "Applied percent of rated torque", which is the name used here, but its own
  /// description and the published documentation both call it a percentage of rated **current**.
  /// The two disagree in the source data, so do not read the name as settling the quantity.
  kCommutationOffsetAppliedTorquePercent = 2,

  kCommutationOffsetMethod = 3,  ///< INTEGER8 — which method runs; see CommutationOffsetMethod.
  kCommutationOffsetKp = 4,      ///< REAL32 — phasing controller Kp; method 1 only.
  kCommutationOffsetKi = 5,      ///< REAL32 — phasing controller Ki; method 1 only.
  kCommutationOffsetKd = 6,      ///< REAL32 — phasing controller Kd; method 1 only.
};

/// @brief How commutation offset measurement (command 5) is performed (0x2009:03).
///
/// **The choice changes what the command physically does, not just its accuracy**, which is why a
/// caller has to read it rather than assume: the two rotating methods need the brake *released*,
/// and the stationary one needs it *engaged* — the drive cannot hold the load itself under that
/// method.
enum class CommutationOffsetMethod : uint8_t {
  /// Rotates the rotor by up to one pole pair. Holds the load, needs no tuning, good precision —
  /// the default, and the one to use unless there is a reason not to.
  kRotating = 0,

  /// Rotates the rotor only a few electrical degrees and holds the load, but **requires the
  /// Kp/Ki/Kd gains in 0x2009:04-06 to have been tuned**; intended for the prototype phase. How
  /// few depends entirely on the quality of that tuning, and the firmware documentation quotes two
  /// different figures for a well-tuned controller — "less than 5 degrees" in prose, "less than 20
  /// electrical degrees" in its comparison table — so treat either as an order of magnitude rather
  /// than a bound, and method 0 as the predictable one.
  kRotatingTuned = 1,

  /// Does not rotate the rotor at all and completes in around 250 ms, at the cost of precision.
  /// **Does not hold the load, so the brake stays engaged** for the duration.
  kStationary = 2,
};

/// @brief Name of a commutation offset method (for logging / JSON). Never returns @c nullptr.
constexpr std::string_view toString(CommutationOffsetMethod method) {
  switch (method) {
    case CommutationOffsetMethod::kRotating:
      return "rotating";
    case CommutationOffsetMethod::kRotatingTuned:
      return "rotatingTuned";
    case CommutationOffsetMethod::kStationary:
      return "stationary";
  }
  return "unknown";
}

/// @brief Whether the method needs the brake released, as opposed to engaged.
///
/// True for the rotating methods, whose restrictions require a disengaged brake. False for
/// @c kStationary, which is the one command in this family that wants the brake **engaged** — it
/// cannot hold the load, so releasing the brake for it would be actively wrong rather than merely
/// unnecessary.
constexpr bool requiresBrakeReleased(CommutationOffsetMethod method) {
  return method != CommutationOffsetMethod::kStationary;
}

/// @brief Which of a drive's two encoders a command addresses. The enum value @b is the ordinal the
///        firmware expects.
///
/// The ordinal selects a *configured slot*, not a kind of encoder: encoder 1 is whatever 0x2110
/// configures and encoder 2 whatever 0x2112 does. A command addressed at an unconfigured slot — or
/// at one holding an encoder whose service does not support the command — is refused by the drive
/// rather than misapplied to the other one.
enum class EncoderOrdinal : uint8_t {
  kEncoder1 = 1,  ///< The encoder configured in 0x2110.
  kEncoder2 = 2,  ///< The encoder configured in 0x2112.
};

/// @brief Name of an encoder ordinal, as a message should render it. Never returns @c nullptr.
constexpr std::string_view toString(EncoderOrdinal encoder) {
  switch (encoder) {
    case EncoderOrdinal::kEncoder1:
      return "encoder 1";
    case EncoderOrdinal::kEncoder2:
      return "encoder 2";
  }
  return "unknown encoder";
}

/// @brief How the BiSS service clocks and interprets an iC-MU encoder (OS command 1). The enum
///        value @b is the "mode enable" field the firmware expects.
///
/// The iC-MU is the chip behind a Circulo's internal encoder, and calibrating one means moving
/// between these three modes rather than setting a single "calibration" switch.
enum class IcMuCalibrationMode : uint8_t {
  /// The service still clocks the encoder but uses only the register-communication bits and
  /// discards the rest, so **position stops updating** and the BiSS CRC error is not raised. It is
  /// what makes it possible to change the encoder's configuration registers without the drive
  /// faulting on the malformed frames that produces.
  kConfiguration = 0,

  /// The service clocks an encoder already configured for raw output, and averages the raw data
  /// into 0x2704 (user MISO) alongside the normal position calculation.
  kRaw = 1,

  /// Normal clocking — the mode an encoder runs in when nothing is being calibrated.
  kStandard = 2,
};

/// @brief Name of an iC-MU calibration mode (for logging / JSON). Never returns @c nullptr.
constexpr std::string_view toString(IcMuCalibrationMode mode) {
  switch (mode) {
    case IcMuCalibrationMode::kConfiguration:
      return "configuration";
    case IcMuCalibrationMode::kRaw:
      return "raw";
    case IcMuCalibrationMode::kStandard:
      return "standard";
  }
  return "unknown";
}

/// @brief Parses a mode token ("configuration" / "raw" / "standard") — the inverse of @c toString.
///        Returns @c std::nullopt for any other token.
std::optional<IcMuCalibrationMode> parseIcMuCalibrationMode(std::string_view token);

/// @brief The faults encoder register communication (command 0) reports as its command-specific OS
///        error code.
///
/// Command-specific codes count up from 0 and mean nothing outside their own command — general
/// codes (@c OsCommandError) count down from 254 — so this table is only valid for command 0.
enum class EncoderRegisterFault : uint8_t {
  kInvalidResponse = 0,     ///< The register transaction failed inside the encoder service.
  kRegisterNotAllowed = 1,  ///< The register cannot be accessed while the firmware is using it.
};

/// @brief Name of an encoder register fault, as the console and logs should render it. Never
///        @c nullptr.
constexpr std::string_view toString(EncoderRegisterFault fault) {
  switch (fault) {
    case EncoderRegisterFault::kInvalidResponse:
      return "invalid response";
    case EncoderRegisterFault::kRegisterNotAllowed:
      return "register not allowed";
  }
  return "unknown";
}

/// @brief What an encoder register fault means, for a message a user has to act on. Never
///        @c nullptr.
constexpr std::string_view describe(EncoderRegisterFault fault) {
  switch (fault) {
    case EncoderRegisterFault::kInvalidResponse:
      return "the register communication transaction failed internally, which a BiSS clock "
             "frequency set too high can cause";
    case EncoderRegisterFault::kRegisterNotAllowed:
      return "the register cannot be accessed at the moment, which happens when the firmware is "
             "itself using it — an iC-MU I2C register while iC-PVL registers are being monitored, "
             "say";
  }
  return "unknown fault";
}

/// @brief Which signal a high resolution data (HRD) stream records — OS command 3's data index. The
///        enum value @b is the index the firmware expects.
///
/// The two formats differ in what a sample *is*, so the choice made when a stream is configured
/// also decides how its files decode. Nothing on the drive records which one was used, which is
/// why reading a recording back takes the same enum as configuring it did.
///
/// **A recording is 1 kHz even on a drive that samples faster.** The drive control loop runs at
/// 250, 500 or 1000 µs, but the OS command 3 handler writes one sample per *millisecond* whatever
/// the loop period is — it divides by its cycles-per-ms, which works because every supported loop
/// period divides 1000 µs. Do not read a recording's length as the drive's resolution.
///
/// Not to be confused with **object 0x20E1, also called high resolution data**, which is a separate
/// feature this code does not touch: a rolling OD array the drive fills every motion cycle with
/// position, velocity and phase currents. Same name, different data, different transport.
enum class HrdData : uint8_t {
  /// The raw position word an iC-MU encoder reports. **Records zeros unless the encoder was first
  /// put into raw mode** (@c IcMuCalibrationMode::kRaw, OS command 1) — the firmware streams
  /// whatever the encoder is currently clocked for.
  kEncoderRawData = 0,

  /// The velocity and torque actual values. **Records the response to a system identification run**
  /// (OS command 15), which has to be configured and started first; on its own this streams a drive
  /// that is not being excited.
  kSystemIdentificationData = 1,
};

/// @brief Name of an HRD data selection, as it appears on the wire. Never returns @c nullptr.
constexpr std::string_view toString(HrdData data) {
  switch (data) {
    case HrdData::kEncoderRawData:
      return "encoder-raw";
    case HrdData::kSystemIdentificationData:
      return "system-identification";
  }
  return "unknown";
}

/// @brief Parses an HRD data token ("encoder-raw" / "system-identification") — the inverse of
///        @c toString. Returns @c std::nullopt for any other token.
std::optional<HrdData> parseHrdData(std::string_view token);

/// @brief How many bytes one recorded sample of @p data occupies in the drive's files.
///
/// Four for a raw encoder word, six for a velocity/torque pair. The firmware writes one sample per
/// millisecond of the configured duration, so this is also what turns a duration into a size.
constexpr size_t hrdSampleSize(HrdData data) { return data == HrdData::kEncoderRawData ? 4 : 6; }

/// @brief The longest stream the drive will record for @p data.
///
/// Two different limits, because the recording is held in at most five 8032-byte files and the
/// ceiling is whatever fills them: 10000 ms of 4-byte samples, or 6000 ms of 6-byte ones. **The
/// drive enforces both** — it picks the limit from the data index and answers
/// @c HrdStreamFault::kDuration — so applying them here only turns a rejected round trip into a
/// caller error. (The firmware specification's error table describes that code as "larger than
/// 10000 ms", which reads as though the narrower limit were unchecked; the drive application
/// checks per data index.)
constexpr std::chrono::milliseconds maxHrdStreamDuration(HrdData data) {
  return data == HrdData::kEncoderRawData ? std::chrono::milliseconds(10000)
                                          : std::chrono::milliseconds(6000);
}

/// @brief The faults HRD streaming (command 3) reports as its command-specific OS error code.
///
/// Command-specific codes count up from 0 and mean nothing outside their own command — general
/// codes (@c OsCommandError) count down from 254 — so this table is only valid for command 3.
enum class HrdStreamFault : uint8_t {
  kInitialization = 0,  ///< The command could not initialise.
  kStreaming = 1,       ///< Something failed while the stream was running.
  kDuration = 2,        ///< The requested duration is above the limit for the chosen data index.
  kDataIndex = 3,       ///< The requested data index is not one the drive knows.
  kAction = 4,          ///< The requested action is neither configure nor start.
};

/// @brief Name of an HRD streaming fault, as the console and logs should render it. Never
///        @c nullptr.
constexpr std::string_view toString(HrdStreamFault fault) {
  switch (fault) {
    case HrdStreamFault::kInitialization:
      return "initialization error";
    case HrdStreamFault::kStreaming:
      return "streaming error";
    case HrdStreamFault::kDuration:
      return "duration value";
    case HrdStreamFault::kDataIndex:
      return "data index value";
    case HrdStreamFault::kAction:
      return "action value";
  }
  return "unknown";
}

/// @brief What an HRD streaming fault means, for a message a user has to act on. Never
///        @c nullptr.
constexpr std::string_view describe(HrdStreamFault fault) {
  switch (fault) {
    case HrdStreamFault::kInitialization:
      return "the drive could not start the recording";
    case HrdStreamFault::kStreaming:
      return "the recording failed part way through, so whatever reached the drive's files is "
             "incomplete";
    case HrdStreamFault::kDuration:
      return "the requested duration is longer than the drive allows for the chosen data, which is "
             "10000 ms for encoder raw data and 6000 ms for system identification data";
    case HrdStreamFault::kDataIndex:
      return "the drive does not know the requested data index, which means its firmware records a "
             "different set of signals than this build expects";
    case HrdStreamFault::kAction:
      return "the drive does not know the requested action, which means its firmware drives HRD "
             "streaming differently than this build expects";
  }
  return "unknown fault";
}

/// @brief Sub-entries of 0x2004 (brake options). The enum value @b is the subindex.
enum BrakeOption : uint8_t {
  kBrakePullVoltage = 1,      ///< UNSIGNED32, mV — voltage that disengages the brake.
  kBrakeHoldVoltage = 2,      ///< UNSIGNED32, mV — lower voltage that keeps it disengaged.
  kBrakePullTime = 3,         ///< UNSIGNED16, ms — how long the pull voltage is applied.
  kBrakeReleaseStrategy = 4,  ///< UNSIGNED8 — how the brake is driven; see BrakeReleaseStrategy.
  kBrakeControllerDisableDelay = 5,  ///< UNSIGNED16, ms — engage-to-controller-off delay.
  kBrakeDcBusVoltage = 6,            ///< UNSIGNED16 — deprecated; the drive still lists it.
  kBrakeStatus = 7,  ///< UNSIGNED8 — reports *and* commands the brake; see BrakeStatus.
  kBrakeMinimumDisplacement = 8,  ///< UNSIGNED32 — pin-brake release travel threshold.

  /// UNSIGNED16 — "Percentage of Rated Current": the current ceiling a pin-brake release works up
  /// to. A percentage of rated current, *not* a torque in mNm.
  kBrakePinCurrentPercent = 9,

  kBrakeOutputVoltage = 10,        ///< UNSIGNED16, mV — phase-D voltage in manual mode.
  kBrakeSwitchingFrequency = 11,   ///< UNSIGNED8 — phase-D PWM rate (0: 16, 1: 32, 2: 64 kHz).
  kBrakePinInitialDirection = 12,  ///< INTEGER8 — which way a pin-brake release moves first.
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
  kEncoderRegisterCommunication = 0,  ///< Reads or writes one register of a BiSS encoder.
  kIcMuCalibrationMode = 1,           ///< Sets how the BiSS service clocks an iC-MU encoder.
  kHrdStreaming = 3,              ///< Records a signal into the drive's high resolution data files.
  kMotorPhaseOrderDetection = 4,  ///< Detects the motor phase order. Rotates the rotor.
  kCommutationOffsetMeasurement = 5,  ///< Measures the commutation angle offset; see
                                      ///< somanet::CommutationOffsetMethod.
  kOpenPhaseDetection = 6,            ///< Checks every motor phase and FET leg for an open circuit.
  kPolePairDetection = 7,             ///< Detects the motor's pole pair count. Rotates the rotor.
  kPhaseResistanceMeasurement = 8,    ///< Measures the motor's phase resistance, in milliohms.
  kPhaseInductanceMeasurement = 9,    ///< Measures the motor's phase inductance, in microhenries.
  kSkippedCycleCounter = 13,          ///< Reads the drive's skipped-cycle counter. Harmless; no
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
  std::chrono::milliseconds timeout{1000};  ///< Ceiling on the whole command.

  std::chrono::milliseconds pollInterval{10};  ///< Delay between response polls.

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

/// @brief What one encoder register access did — the result of both a read and a write, since the
///        drive answers a write by echoing back the register's value.
///
/// @c value is optional because one access does not produce one: the iC-MU soft reset (writing
/// 0x07 to register 0x75) restarts the chip, so it is acknowledged without a response. Reporting
/// that as a value of 0 would be a reading nobody took.
struct EncoderRegisterResult {
  somanet::EncoderOrdinal encoder{somanet::EncoderOrdinal::kEncoder1};  ///< Which encoder answered.
  uint8_t registerAddress = 0;                                          ///< The register accessed.
  bool wrote = false;            ///< Whether this was a write; false for a read.
  std::optional<uint8_t> value;  ///< What the register holds, when the drive reported it.

  /// @brief One line describing the access, ready to put in front of a user.
  std::string describe() const;
};
void to_json(nlohmann::json& j, const EncoderRegisterResult& result);

/// @brief One file stored on a device, as its file list reports it.
///
/// @c byteCount is optional because the size is what the *list* claims, not what a read returned:
/// a firmware that reports a name without one is still reporting a file that can be read.
struct DeviceFile {
  std::string name;                 ///< Filename as FoE addresses it, e.g. "hr_data0.bin".
  std::optional<size_t> byteCount;  ///< Size the device reported, when it reported one.
};
void to_json(nlohmann::json& j, const DeviceFile& file);

/// @brief Parses the body of a @c fs-getlist read into one entry per line.
///
/// Pure, so the one place that knows the firmware's listing format is testable without a device.
/// Each line is `<filename>, size: <bytes>`; a line that does not end that way is taken **whole**
/// as a filename with no size, rather than dropped — a name is what a caller needs, and a firmware
/// that formats a line unexpectedly should not make the files around it unreachable. Blank lines
/// are skipped and CRLF is accepted.
///
/// @param text  The pseudo-file's contents.
/// @return One entry per non-blank line, in the order they appeared.
std::vector<DeviceFile> parseDeviceFileList(std::string_view text);

/// @brief One sample of @c somanet::HrdData::kEncoderRawData.
///
/// @c raw is the word the encoder reported and the two counts are the fields packed inside it —
/// both are carried because the counts are what a calibration works on, while the raw word is the
/// only lossless record (the firmware specification describes bits 0-27 and says nothing about the
/// top four).
struct HrdEncoderSample {
  uint32_t raw = 0;          ///< The 32-bit word as the file holds it.
  uint16_t masterCount = 0;  ///< Bits 0-13: the master track's count.
  uint16_t noniusCount = 0;  ///< Bits 14-27: the nonius track's count.
};

/// @brief One sample of @c somanet::HrdData::kSystemIdentificationData.
struct HrdSystemIdentificationSample {
  /// The velocity actual value, converted from the file's Q15 fixed point to real RPM.
  double velocityRpm = 0.0;

  /// The torque actual value, in per mille of rated torque — the file's own unit.
  int16_t torquePermil = 0;
};

/// @brief A decoded recording's samples: exactly one vector, whichever the data selection implies.
///
/// A variant rather than two vectors with one left empty, so a recording cannot claim to be encoder
/// data while carrying velocity samples.
using HrdSamples =
    std::variant<std::vector<HrdEncoderSample>, std::vector<HrdSystemIdentificationSample>>;

/// @brief One high resolution data recording, read back from the drive's files and decoded.
///
/// @c files is what was read, in order, so a recording that came back short can be told from one
/// that was never made. @c trailingBytes is the bytes past the last whole sample: the firmware
/// allocates its files in fixed-size blocks, so a recording that does not fill the last block
/// leaves padding behind, and a non-zero value here is that padding rather than lost data.
struct HrdRecording {
  somanet::HrdData data{somanet::HrdData::kEncoderRawData};  ///< Which signal this recorded.
  std::vector<DeviceFile> files;  ///< The files it was read from, in order.
  size_t byteCount = 0;           ///< Total bytes read across them.
  size_t trailingBytes = 0;       ///< Bytes past the last whole sample; padding, not data.
  HrdSamples samples;             ///< The decoded samples.

  /// @brief How many samples were decoded, whichever format they are in.
  size_t sampleCount() const;
};
void to_json(nlohmann::json& j, const HrdRecording& recording);

/// @brief Renders a recording as CSV: one header row of column names, then one row per sample.
///
/// The second representation of the same recording, for the spreadsheet-and-script half of the
/// audience — a full recording is ten thousand rows, which is a file to open rather than JSON to
/// read. Beside @c to_json rather than in the HTTP layer, so the two renderings of one type stay
/// together.
///
/// Every row ends in a newline, the header included, so appending or concatenating cannot join two
/// records onto one line.
std::string toCsv(const HrdRecording& recording);

/// @brief The column names of one decoded sample of @p data, in the order a row carries them.
///
/// The rows go on the wire positionally — a 10 s recording is ten thousand of them — so the names
/// travel once, beside the rows, exactly as the monitoring protocol ships its parameter order once.
std::vector<std::string_view> hrdColumns(somanet::HrdData data);

/// @brief Decodes the concatenated contents of a recording's files into samples.
///
/// Pure: the transform from bytes to samples, with no device involved, which is what makes the
/// firmware's two on-disk layouts testable without one. **Little-endian**, unlike the OS command
/// payloads that configure the stream — these are words the firmware wrote to a file rather than
/// bytes it packed into a CoE object.
///
/// A trailing partial sample is ignored rather than rejected: the drive's files are fixed-size
/// blocks, so a recording that does not fill the last one ends in padding. The caller learns how
/// much was left over from @c bytes.size() % somanet::hrdSampleSize.
///
/// @param bytes  Every file's contents, concatenated in file order.
/// @param data   Which layout to read them as — the same selection the stream was configured with.
/// @return The decoded samples, in the alternative @p data implies.
HrdSamples decodeHrdSamples(std::span<const uint8_t> bytes, somanet::HrdData data);

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

/// @brief What commutation offset measurement (command 5) measured.
///
/// Like motor phase order detection, **the drive stores this itself**: on success the firmware
/// writes the offset into 0x2001 and sets 0x2009:01 to OFFSET_VALID, so a successful run has
/// commissioned the axis rather than merely reported a number.
struct CommutationOffsetResult {
  int16_t angleOffset = 0;  ///< The measured offset, signed as the drive's own 0x2001 is.

  /// @brief The method the measurement ran under, which decides whether the rotor turned.
  somanet::CommutationOffsetMethod method{somanet::CommutationOffsetMethod::kRotating};

  /// @brief One line describing the measurement, ready to put in front of a user.
  std::string describe() const;
};
void to_json(nlohmann::json& j, const CommutationOffsetResult& result);

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

  /// @brief Reads the drive's description of its most recent fault (0x203F:01).
  ///
  /// The one thing that turns a bare CiA402 @c Fault into an actionable message, which is why a
  /// procedure that finds a drive faulted mid-sequence reads it. Best-effort by nature: the object
  /// is manufacturer-specific and the string is short, so a caller should attach whatever it gets
  /// and carry on rather than treat a failed read as the real problem.
  std::expected<std::string, std::string> errorReport() const;

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
  /// **On a pin brake (@c kPin) this moves the shaft.** The controller raises current progressively
  /// until the load has lifted off the pin by the minimum displacement (0x2004:08), reversing
  /// direction if it reaches the current ceiling (0x2004:09, a percentage of rated current) first.
  /// Releasing a brake is not electrically passive on that strategy.
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

  /// @brief Reads one register of an encoder (OS command 0, read direction).
  ///
  /// The register communication the encoder's own service performs on the master's behalf —
  /// **only BiSS implements it today**, which is the command's one restriction along with the
  /// addressed encoder actually being configured. Both are enforced by the drive refusing the
  /// command, so a non-BiSS or unconfigured slot comes back as an error rather than a bad reading.
  ///
  /// Unlike the motor measurements this needs **no preparation at all**: no diagnostics mode, no
  /// Operation Enabled, no brake, and the shaft does not move. It needs only an active mailbox, so
  /// it works from PRE-OP up and can be run on a drive that is exchanging process data without
  /// disturbing it.
  ///
  /// A refusal is an error and never a finding: this command either performs the transaction or
  /// does not. See @c somanet::EncoderRegisterFault for what its own error codes mean.
  ///
  /// @param encoder          Which configured encoder to address.
  /// @param registerAddress  The register to read.
  /// @param config           Timing and cancellation. The default is sized for this command.
  /// @return What the register holds, or why it could not be read.
  std::expected<EncoderRegisterResult, std::string> readEncoderRegister(
      somanet::EncoderOrdinal encoder, uint8_t registerAddress,
      const OsCommandConfig& config = {.timeout = std::chrono::seconds(5),
                                       .pollInterval = std::chrono::milliseconds(20)});

  /// @brief Writes one register of an encoder (OS command 0, write direction).
  ///
  /// The counterpart of @c readEncoderRegister in every respect — same BiSS-only restriction, same
  /// absence of any preparation — and the drive answers it the same way, by reporting the
  /// register's value, so a write confirms itself.
  ///
  /// **A write reconfigures the encoder, and nothing here validates what is written.** The register
  /// map belongs to the encoder chip, not to this firmware: a value that means one thing on an
  /// iC-MU means another elsewhere, and a wrong one can leave an encoder unable to report position.
  /// The one exception the firmware documents is the iC-MU soft reset — 0x07 into register 0x75 —
  /// which restarts the chip and is therefore acknowledged *without* a value (see
  /// @c EncoderRegisterResult::value).
  ///
  /// @param encoder          Which configured encoder to address.
  /// @param registerAddress  The register to write.
  /// @param value            The byte to write into it.
  /// @param config           Timing and cancellation. The default is sized for this command.
  /// @return What the register holds afterwards, or why it could not be written.
  std::expected<EncoderRegisterResult, std::string> writeEncoderRegister(
      somanet::EncoderOrdinal encoder, uint8_t registerAddress, uint8_t value,
      const OsCommandConfig& config = {.timeout = std::chrono::seconds(5),
                                       .pollInterval = std::chrono::milliseconds(20)});

  /// @brief Sets an iC-MU encoder's calibration mode (OS command 1).
  ///
  /// Restricted to a **Circulo internal encoder** — narrower than
  /// @c readEncoderRegister's BiSS-only rule — and to a configured slot; the drive refuses
  /// anything else rather than misapplying it.
  ///
  /// Like the register accesses this prepares nothing and moves nothing, needing only an active
  /// mailbox. Unlike them it **leaves the encoder in the mode it set**: there is no restoring
  /// counterpart, so an encoder put into @c kConfiguration or @c kRaw stays there until something
  /// puts it back to @c kStandard.
  ///
  /// Two behaviours of the firmware worth knowing before sequencing calls, both of which make the
  /// order matter rather than being incidental: entering @c kConfiguration **saves the current
  /// position**, and entering @c kRaw uses that saved position as the starting point (raw data is
  /// relative) — so the motor must not move while in configuration mode if raw mode is to follow.
  ///
  /// The command reports nothing on success, so there is no value to return; a mode value the
  /// sensor service does not recognise comes back as an error.
  ///
  /// @param encoder  Which configured encoder to address.
  /// @param mode     The mode to put it in.
  /// @param config   Timing and cancellation. The default is sized for this command.
  /// @return Void once the drive applied the mode, otherwise why it did not.
  std::expected<void, std::string> setIcMuCalibrationMode(
      somanet::EncoderOrdinal encoder, somanet::IcMuCalibrationMode mode,
      const OsCommandConfig& config = {.timeout = std::chrono::seconds(5),
                                       .pollInterval = std::chrono::milliseconds(20)});

  /// @brief Configures a high resolution data stream (OS command 3, configure action).
  ///
  /// Chooses what the next recording captures and for how long, and **deletes every HRD file
  /// already on the drive** — which is the slow part: the firmware specification allows up to
  /// around 5 seconds for it in the worst case, so the default timeout is sized for that and not
  /// for a mailbox exchange.
  ///
  /// Configuring does not record anything; @c startHrdStream does. They are separate commands
  /// precisely so a recording can be armed once and triggered when the machine is ready.
  ///
  /// @p duration is validated against @c somanet::maxHrdStreamDuration before anything is sent. The
  /// drive checks it too and would answer @c HrdStreamFault::kDuration, so this is a courtesy
  /// rather than a safety net: it makes an out-of-range duration a caller error instead of a
  /// round trip, and keeps both limits stated in one place.
  ///
  /// Needs no preparation — no diagnostics mode, no Operation Enabled, no brake, and nothing moves
  /// — only an active mailbox. What the *data* is worth does depend on preparation elsewhere: see
  /// @c somanet::HrdData, whose two selections each require another command to have run first.
  ///
  /// @param data      Which signal the recording should capture.
  /// @param duration  How long to record for; at most @c somanet::maxHrdStreamDuration(data).
  /// @param config    Timing and cancellation. The default allows for the file deletion.
  /// @return Void once the drive accepted the configuration, otherwise why it did not.
  std::expected<void, std::string> configureHrdStream(
      somanet::HrdData data, std::chrono::milliseconds duration,
      const OsCommandConfig& config = {.timeout = std::chrono::seconds(10),
                                       .pollInterval = std::chrono::milliseconds(100)});

  /// @brief Starts the configured high resolution data stream and waits for it to finish (OS
  /// command 3,
  ///        start action).
  ///
  /// **Blocks for the whole configured duration** — up to ten seconds — because the drive holds the
  /// command in progress until the recording is complete, reporting its percentage as it goes. So
  /// @c config.timeout has to exceed the duration that was configured, and there is nothing in the
  /// start request that says what that duration is: the caller is the only one that knows, which is
  /// why this takes no arguments but needs its config sized deliberately.
  ///
  /// Cancelling through @c config.stop aborts the recording on the drive like any other OS command,
  /// and the drive's two ways of stopping are **not** equivalent: finishing normally flushes the
  /// buffers it still holds before closing the stream, while an abort stops immediately and
  /// **discards whatever is still buffered** (up to 1004 bytes, so roughly 250 encoder samples or
  /// 167 velocity/torque ones). Everything already written to flash stays, so a cancelled recording
  /// is a short one rather than none — just short by more than the moment of cancelling.
  ///
  /// @param config  Timing and cancellation. **Has no useful default** — size the timeout from the
  ///                duration that was configured.
  /// @return Void once the recording finished, otherwise why it did not.
  std::expected<void, std::string> startHrdStream(const OsCommandConfig& config);

  /// @brief Reads the drive's high resolution data files back and decodes them (FoE, no OS
  /// command).
  ///
  /// Discovers the files from the device's own file list rather than guessing at their names, so a
  /// firmware that splits a recording across five files and one that keeps it in a single file are
  /// both read whole, and a device with no recording on it says so immediately instead of being
  /// probed for files that are not there.
  ///
  /// The files are concatenated in numeric order and decoded as one stream — a sample may straddle
  /// a file boundary, since the firmware chunks a byte stream rather than padding each file to a
  /// whole number of samples.
  ///
  /// @p data must be the selection the recording was made with. **Nothing on the drive records
  /// it**, so passing the other one silently reinterprets the bytes; the procedure that made the
  /// recording reports what it configured for exactly this reason.
  ///
  /// Blocks for the transfer — five 8 KB FoE reads plus the list — on the calling thread. Requires
  /// an active mailbox.
  ///
  /// @param data  Which layout the files hold.
  /// @return The decoded recording, or an error if the list or a read failed.
  std::expected<HrdRecording, std::string> readHrdRecording(somanet::HrdData data) const;

  /// @brief Reads the list of files stored on the device (FoE read of "fs-getlist").
  ///
  /// Synapticon firmware serves its directory as a pseudo-file rather than through any standard
  /// service: reading the name @c fs-getlist over FoE returns one line per entry. That makes this a
  /// vendor operation despite looking like a filesystem primitive, which is why it lives here and
  /// not on @c Device beside @c readFile.
  ///
  /// The listing is what makes @c readHrdRecording possible without guessing at filenames, and it
  /// is the answer to "what else is on this drive" for firmware, logs and the ESI.
  ///
  /// @return Every file the device reported, in the order it reported them.
  std::expected<std::vector<DeviceFile>, std::string> readFileList() const;

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

  /// @brief Reads which commutation offset method is configured (0x2009:03).
  ///
  /// Worth reading before running command 5 rather than after, because the method decides whether
  /// the rotor will turn and whether the brake must be released or engaged — see
  /// @c somanet::requiresBrakeReleased. A value outside the defined 0-2 range is reported as an
  /// error rather than cast, since acting on a misread method would mean handling the brake the
  /// wrong way.
  std::expected<somanet::CommutationOffsetMethod, std::string> commutationOffsetMethod() const;

  /// @brief Runs commutation offset measurement (OS command 5) and decodes the offset it reports.
  ///
  /// **A successful run reconfigures the drive**: the firmware writes the measured offset into
  /// 0x2001 and sets 0x2009:01 to OFFSET_VALID, which is the point of running it — this is the
  /// measurement that commissions the axis.
  ///
  /// Preconditions, all enforced by the drive refusing with OS error 251: operation mode
  /// @c somanet::OperationMode::kDiagnostics and CiA402 state Operation Enabled always, plus —
  /// **for the rotating methods only** — no limit switch active and the brake disengaged. Motor
  /// phase order detection (command 4) must also have been run; that one the drive does not check.
  ///
  /// **Whether it turns the rotor depends on the configured method** (0x2009:03), so a caller that
  /// has not read the method does not know what this will do physically. @c kStationary does not
  /// turn it and needs the brake engaged; the two rotating methods turn it and need the brake
  /// released.
  ///
  /// This command has no command-specific error codes, so a failure always carries a general one.
  ///
  /// @param method  The method read from the drive, recorded in the result so a reader of the value
  ///                knows which measurement produced it.
  /// @param config  Timing and cancellation. The default timeout is sized for this command.
  /// @return The measured offset, or why none was produced.
  std::expected<CommutationOffsetResult, std::string> runCommutationOffsetMeasurement(
      somanet::CommutationOffsetMethod method,
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
