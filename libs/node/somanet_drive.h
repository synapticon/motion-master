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
#include "node/firmware_package.h"

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
/// phase resistance (8), phase inductance (9) and torque constant measurement (10) report a value
/// without storing it, and :01, :02, :03 and :04 are the entries it goes in.
enum MotorSetting : uint8_t {
  kMotorPolePairs = 1,  ///< UNSIGNED8 — where a pole pair count (command 7) belongs.

  /// INTEGER32 — where a torque constant (command 10) belongs, **in µNm/A_rms**, which is not the
  /// unit the command reports. The drive's own description of this entry names the unit; the
  /// command answers in mNm/A_rms, a factor of 1000 coarser. See @c TorqueConstantResult.
  kMotorTorqueConstant = 2,
  /// INTEGER32 — where a phase resistance (command 8) belongs, **in µΩ**, which is not the unit the
  /// command reports either: it answers in mΩ, so this is the second of the three that needs
  /// multiplying by 1000 before it is stored. The firmware reads this object into a variable it
  /// calls @c phase_resistance_uOhm, which is what settles it.
  kMotorPhaseResistance = 3,

  /// INTEGER32 — where a phase inductance (command 9) belongs, in µH. **The one that matches its
  /// command**, which also reports µH, so a measurement is stored as it comes. The firmware's
  /// variable is @c phase_inductance_uH.
  kMotorPhaseInductance = 4,
  kMotorPhasesInverted = 5,  ///< BOOLEAN — the phase order motor phase order detection (4) writes.
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
/// The ordinal selects a *configuration object*, not a kind of encoder: encoder 1 is the encoder
/// configured in 0x2110 and encoder 2 the one configured in 0x2112. A command addressed at an
/// unconfigured encoder — or at one whose service does not support the command — is refused by the
/// drive rather than misapplied to the other one.
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

/// @brief One of the drive's two independently-scheduled control loops, as the firmware's
///        @c FirmwareServiceId names them. The enum value @b is the byte the command carries.
///
/// They are separate loops with separate periods and separate counters, so a command addressed at
/// one says nothing about the other. **Each service answers only requests naming itself**: a byte
/// naming neither leaves the command unanswered by anything, and the drive's OS command handler
/// eventually fails it as a timeout rather than as a bad parameter — which is why a caller
/// validates this rather than passing a number through.
enum class FirmwareService : uint8_t {
  kDriveControl = 0,   ///< The current/torque loop — the fast one, period per 0x60C2.
  kMotionControl = 1,  ///< The position/velocity loop above it.
};

/// @brief Name of a firmware service (for logging / JSON). Never returns @c nullptr.
constexpr std::string_view toString(FirmwareService service) {
  switch (service) {
    case FirmwareService::kDriveControl:
      return "drive-control";
    case FirmwareService::kMotionControl:
      return "motion-control";
  }
  return "unknown";
}

/// @brief Parses a service token ("drive-control" / "motion-control") — the inverse of
///        @c toString. Returns @c std::nullopt for any other token.
std::optional<FirmwareService> parseFirmwareService(std::string_view token);

/// @brief What the Kübler encoder reported when a register access failed (OS command 19).
///
/// Command-specific codes, so they mean nothing outside this command. **The specification numbers
/// the last one 5; the firmware sends 4**, and there is no code 5 — @c OsCmd19ErrorCodes is the
/// authority.
enum class KueblerRegisterFault : uint8_t {
  kAsyncInProgress = 0,        ///< The encoder's asynchronous channel is busy with something else.
  kInvalidRegisterLength = 1,  ///< The length byte was below 1 or above 4.
  kEncoderTimeout = 2,         ///< The encoder returned no value within 10 ms. Reads only.
  kWrongByteCount = 3,  ///< It answered with a different width than was asked for. Reads only.
  /// The encoder is writing flash and will not service the access. **Four addresses are exempt** —
  /// 0x24, 0x25, 0x50 and 0x52, the POA and correction status/control pair — so those stay
  /// reachable during a correction-table operation, which is when the encoder is most likely to be
  /// busy.
  kEncoderBusy = 4,
};

/// @brief Name of a Kübler register fault, as a message should render it. Never @c nullptr.
constexpr std::string_view toString(KueblerRegisterFault fault) {
  switch (fault) {
    case KueblerRegisterFault::kAsyncInProgress:
      return "the encoder's asynchronous channel is busy with another transaction";
    case KueblerRegisterFault::kInvalidRegisterLength:
      return "the register length must be 1 to 4 bytes";
    case KueblerRegisterFault::kEncoderTimeout:
      return "the encoder did not return a value within 10 ms";
    case KueblerRegisterFault::kWrongByteCount:
      return "the encoder answered with a different number of bytes than were asked for";
    case KueblerRegisterFault::kEncoderBusy:
      return "the encoder is accessing its flash memory and cannot service this register";
  }
  return "unknown fault";
}

/// @brief Where the velocity control loop takes its feedback from (OS command 18). The enum value
///        @b is the trigger bit the command carries.
///
/// **Only the Integro's internal (Kübler) encoder provides its own velocity**, so this decides
/// anything only for that encoder, wherever it is configured. On a drive whose Kübler encoder
/// is not configured for velocity control the command is accepted and changes nothing.
///
/// The velocity feedback filter is applied either way; this chooses what goes into it.
enum class VelocitySource : uint8_t {
  /// Differentiated from encoder position by the firmware. **The default on every build except
  /// Integro** — and the OS command specification calls it the default outright, which is wrong for
  /// the one product the command applies to.
  kFirmware = 0,
  /// Reported by the encoder itself, which integrates it in hardware. **The default on an Integro
  /// build**, where the firmware sets the flag at start-up with a comment saying so — so on an
  /// Integro the useful direction is towards @c kFirmware, not away from it.
  kEncoder = 1,
};

/// @brief Name of a velocity source (for logging / JSON). Never returns @c nullptr.
constexpr std::string_view toString(VelocitySource source) {
  switch (source) {
    case VelocitySource::kFirmware:
      return "firmware";
    case VelocitySource::kEncoder:
      return "encoder";
  }
  return "unknown";
}

/// @brief Parses a velocity source token ("firmware" / "encoder"). @c std::nullopt otherwise.
std::optional<VelocitySource> parseVelocitySource(std::string_view token);

/// @brief Which internal firmware latency OS command 22 measures. The enum value @b is the latency
///        index byte the command carries.
///
/// Both are durations inside the **drive control service's** cycle, which is the only service that
/// implements the command; the drive keeps one maximum per latency and measures them independently.
enum class FirmwareLatency : uint8_t {
  /// From the start of the drive control service cycle until the setpoints are handed to the motion
  /// control service. The specification's use for it: deciding how far ahead of the motion control
  /// cycle the drive control cycle has to start.
  kSetpoint = 0,
  /// From the request for control feedback until the end of the drive control service cycle. The
  /// specification's use for it: deciding whether there is time to wait for another motion control
  /// cycle before taking the feedback.
  kFeedback = 1,
};

/// @brief Name of a firmware latency (for logging / JSON). Never returns @c nullptr.
constexpr std::string_view toString(FirmwareLatency latency) {
  switch (latency) {
    case FirmwareLatency::kSetpoint:
      return "setpoint";
    case FirmwareLatency::kFeedback:
      return "feedback";
  }
  return "unknown";
}

/// @brief Parses a firmware latency token ("setpoint" / "feedback"). @c std::nullopt otherwise.
std::optional<FirmwareLatency> parseFirmwareLatency(std::string_view token);

/// @brief The errors OS command 16 can provoke in a firmware service. The enum value @b is the
///        error-type byte the command carries.
///
/// **A test and diagnostic tool, and most of it is one-way.** The XMOS exception types are raised
/// by executing genuinely faulty code — an unaligned store, a divide by zero, an out-of-bounds
/// index — and the service that executes it does not come back. Only @c kResettableFirmwareError
/// leaves a drive you can carry on using.
enum class FirmwareErrorType : uint8_t {
  kLinkError = 0,           ///< ET_LINK_ERROR. Not implemented by the firmware.
  kIllegalPc = 1,           ///< ET_ILLEGAL_PC. Not implemented.
  kIllegalInstruction = 2,  ///< ET_ILLEGAL_INSTRUCTION. Not implemented.
  kIllegalResource = 3,     ///< ET_ILLEGAL_RESOURCE. Not implemented.
  kLoadStore = 4,           ///< ET_LOAD_STORE, by an unaligned 64-bit store. Stops the service.
  kIllegalPs = 5,           ///< ET_ILLEGAL_PS. Not implemented.
  kArithmetic = 6,          ///< ET_ARITHMETIC, by dividing by zero. Stops the service.
  kEcall = 7,               ///< ET_ECALL, by an out-of-bounds array index. Stops the service.
  kResourceDependency = 8,  ///< ET_RESOURCE_DEP. Not implemented.
  kKcall = 9,               ///< ET_KCALL. Not implemented.
  kEndlessLoop = 10,        ///< Spins forever in the service loop. Stops the service.
  /// Reports a resettable @c DiagErr with the drive's configured quick-stop reaction. **The only
  /// type that leaves the drive usable** — it faults, and a fault reset clears it.
  kResettableFirmwareError = 11,
};

/// @brief What a firmware error type actually does on this firmware — which is not what every one
///        of them is named after.
enum class FirmwareErrorEffect : uint8_t {
  /// The firmware's case body is empty: the command is accepted, nothing happens, and the drive
  /// answers that it failed. Seven of the twelve are like this, and the specification marks them
  /// "(disabled)".
  kNotImplemented,
  /// The service executes faulty code or hangs, and stops answering for good. **Only a power cycle
  /// brings it back**; the drive does not fault, it stops participating.
  kStopsService,
  /// A resettable error is reported and the drive reacts as 0x605A says. Recoverable.
  kRaisesResettableError,
};

/// @brief What @p type does, from the firmware's own case bodies rather than from its name.
constexpr FirmwareErrorEffect effectOf(FirmwareErrorType type) {
  switch (type) {
    case FirmwareErrorType::kLinkError:
    case FirmwareErrorType::kIllegalPc:
    case FirmwareErrorType::kIllegalInstruction:
    case FirmwareErrorType::kIllegalResource:
    case FirmwareErrorType::kIllegalPs:
    case FirmwareErrorType::kResourceDependency:
    case FirmwareErrorType::kKcall:
      return FirmwareErrorEffect::kNotImplemented;
    case FirmwareErrorType::kLoadStore:
    case FirmwareErrorType::kArithmetic:
    case FirmwareErrorType::kEcall:
    case FirmwareErrorType::kEndlessLoop:
      return FirmwareErrorEffect::kStopsService;
    case FirmwareErrorType::kResettableFirmwareError:
      return FirmwareErrorEffect::kRaisesResettableError;
  }
  return FirmwareErrorEffect::kNotImplemented;
}

/// @brief Name of a firmware error type (for logging / JSON). Never returns @c nullptr.
constexpr std::string_view toString(FirmwareErrorType type) {
  switch (type) {
    case FirmwareErrorType::kLinkError:
      return "link-error";
    case FirmwareErrorType::kIllegalPc:
      return "illegal-pc";
    case FirmwareErrorType::kIllegalInstruction:
      return "illegal-instruction";
    case FirmwareErrorType::kIllegalResource:
      return "illegal-resource";
    case FirmwareErrorType::kLoadStore:
      return "load-store";
    case FirmwareErrorType::kIllegalPs:
      return "illegal-ps";
    case FirmwareErrorType::kArithmetic:
      return "arithmetic";
    case FirmwareErrorType::kEcall:
      return "ecall";
    case FirmwareErrorType::kResourceDependency:
      return "resource-dependency";
    case FirmwareErrorType::kKcall:
      return "kcall";
    case FirmwareErrorType::kEndlessLoop:
      return "endless-loop";
    case FirmwareErrorType::kResettableFirmwareError:
      return "resettable-firmware-error";
  }
  return "unknown";
}

/// @brief Parses a firmware error type token — the inverse of @c toString.
std::optional<FirmwareErrorType> parseFirmwareErrorType(std::string_view token);

/// @brief Every firmware error type, ascending.
inline constexpr FirmwareErrorType kFirmwareErrorTypes[] = {
    FirmwareErrorType::kLinkError,          FirmwareErrorType::kIllegalPc,
    FirmwareErrorType::kIllegalInstruction, FirmwareErrorType::kIllegalResource,
    FirmwareErrorType::kLoadStore,          FirmwareErrorType::kIllegalPs,
    FirmwareErrorType::kArithmetic,         FirmwareErrorType::kEcall,
    FirmwareErrorType::kResourceDependency, FirmwareErrorType::kKcall,
    FirmwareErrorType::kEndlessLoop,        FirmwareErrorType::kResettableFirmwareError,
};

/// @brief The settings of the system-identification chirp (OS command 15). The enum value @b is
///        the parameter index the command carries in byte 1.
///
/// **One command sets one of these**, so configuring a run means issuing the command once per
/// setting. They are held on the drive until overwritten or it is power-cycled; nothing reads them
/// back.
enum class SystemIdentificationParameter : uint8_t {
  kStartFrequency = 0,   ///< Where the sweep begins, in mHz.
  kTargetFrequency = 1,  ///< Where it ends, in mHz. Must not be below the start frequency.
  kTargetAmplitude = 2,  ///< Peak excitation, in per-mille of rated torque. See the note below.
  kTransitionTime = 3,   ///< How long the sweep takes, in ms.
  kSignalType = 4,       ///< Which chirp; see @c ChirpSignalType.
  kStartProcedure = 5,   ///< Arms the run; see @c SystemIdentificationStart.
};

/// @brief Name of a system-identification parameter, as a message should render it. Never
///        @c nullptr.
constexpr std::string_view toString(SystemIdentificationParameter parameter) {
  switch (parameter) {
    case SystemIdentificationParameter::kStartFrequency:
      return "start frequency";
    case SystemIdentificationParameter::kTargetFrequency:
      return "target frequency";
    case SystemIdentificationParameter::kTargetAmplitude:
      return "target amplitude";
    case SystemIdentificationParameter::kTransitionTime:
      return "transition time";
    case SystemIdentificationParameter::kSignalType:
      return "signal type";
    case SystemIdentificationParameter::kStartProcedure:
      return "start the procedure";
  }
  return "unknown parameter";
}

/// @brief Which excitation the system-identification run sweeps with (parameter 4).
enum class ChirpSignalType : uint8_t {
  /// Logarithmic frequency sweep whose amplitude rises with it, from half the target to the target.
  kLogarithmic = 0,
  /// Linear frequency sweep at a constant amplitude.
  kLinear = 1,
};

/// @brief Name of a chirp signal type (for logging / JSON). Never returns @c nullptr.
constexpr std::string_view toString(ChirpSignalType type) {
  switch (type) {
    case ChirpSignalType::kLogarithmic:
      return "logarithmic";
    case ChirpSignalType::kLinear:
      return "linear";
  }
  return "unknown";
}

/// @brief Parses a signal-type token ("logarithmic" / "linear"). @c std::nullopt for any other.
std::optional<ChirpSignalType> parseChirpSignalType(std::string_view token);

/// @brief What writing parameter 5 does — arm the run, and on what trigger.
enum class SystemIdentificationStart : uint8_t {
  /// Do not arm. Writing this is also how an armed run is disarmed, which matters because the
  /// drive starts on the **rising edge** of this parameter: re-arming without clearing first is a
  /// write the firmware never sees as an edge.
  kNone = 0,
  kImmediately = 1,  ///< Start on the next control cycle.
  /// Start when high resolution data streaming starts. This is the pairing that produces a usable
  /// recording — see @c HrdData::kSystemIdentificationData.
  kAfterHrdStreamStart = 2,
};

/// @brief Name of a start trigger (for logging / JSON). Never returns @c nullptr.
constexpr std::string_view toString(SystemIdentificationStart start) {
  switch (start) {
    case SystemIdentificationStart::kNone:
      return "none";
    case SystemIdentificationStart::kImmediately:
      return "immediately";
    case SystemIdentificationStart::kAfterHrdStreamStart:
      return "after-hrd-stream-start";
  }
  return "unknown";
}

/// @brief Parses a start token ("none" / "immediately" / "after-hrd-stream-start").
std::optional<SystemIdentificationStart> parseSystemIdentificationStart(std::string_view token);

/// @brief The bounds the firmware enforces on a chirp configuration.
///
/// **Taken from the firmware's own @c are_sinewave_config_parameters_valid, not from the OS command
/// specification**, which says only that an out-of-range value raises an "Invalid Parameter" error
/// and points at another document for the numbers. They are the range within which its fixed-point
/// arithmetic does not overflow, which is why they are hard limits rather than advice.
///
/// The amplitude is deliberately absent: the firmware does not check it. See
/// @c SomanetDrive::setSystemIdentificationParameter.
inline constexpr uint32_t kMinChirpFrequencyMilliHz = 100;        ///< 0.1 Hz.
inline constexpr uint32_t kMaxChirpFrequencyMilliHz = 1'000'000;  ///< 1000 Hz.
inline constexpr uint32_t kMinChirpTransitionTimeMs = 1'000;      ///< 1 s.
inline constexpr uint32_t kMaxChirpTransitionTimeMs = 20'000;     ///< 20 s.

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
  kTorqueConstantMeasurement = 10,    ///< Measures the motor's torque constant. Rotates the rotor.
  kSkippedCyclesCounter = 13,         ///< Reads a control loop's skipped-cycle counter. Harmless;
                                      ///< no motion.
  kIgnoreBissStatusBits = 14,         ///< Suppresses a BiSS encoder's own error and warning bits.
  kSystemIdentification = 15,         ///< Configures and triggers the system-identification chirp.
  kTriggerError = 16,                 ///< Provokes a firmware error or exception. Test tool only.
  kVelocitySource = 18,               ///< Chooses where the velocity loop's feedback comes from.
  kKueblerRegisterCommunication = 19,  ///< Reads or writes an Integro internal encoder register.
  kMeasureFirmwareLatency = 22,        ///< Measures a duration inside the drive control cycle.
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
  kImpedance = -6,             ///< Impedance mode.
  kJointTorque = -5,           ///< Joint torque mode.
  kSystemIdentification = -4,  ///< System identification mode. **Deprecated by the firmware.**
  kOpenLoopField = -3,         ///< Open loop field control mode.
  kDiagnostics = -2,           ///< Where the master owns the brake and the measurement OS
                               ///< commands run.
  kCoggingCompensationRecording = -1,  ///< Cogging torque table recording.
};

/// @brief Name of a SOMANET operation mode, in the same PascalCase spelling the standard modes use
///        (@c cia402::toString), so the two halves of a mode list read alike. Never @c nullptr.
constexpr std::string_view toString(OperationMode mode) {
  switch (mode) {
    case OperationMode::kImpedance:
      return "Impedance";
    case OperationMode::kJointTorque:
      return "JointTorque";
    case OperationMode::kSystemIdentification:
      return "SystemIdentification";
    case OperationMode::kOpenLoopField:
      return "OpenLoopField";
    case OperationMode::kDiagnostics:
      return "Diagnostics";
    case OperationMode::kCoggingCompensationRecording:
      return "CoggingCompensationRecording";
  }
  return "Unknown";
}

/// @brief The drive's own wording for a mode, as its ESI documents 0x6060. Never @c nullptr.
constexpr std::string_view describe(OperationMode mode) {
  switch (mode) {
    case OperationMode::kImpedance:
      return "Impedance mode";
    case OperationMode::kJointTorque:
      return "Joint torque mode";
    case OperationMode::kSystemIdentification:
      return "System identification mode";
    case OperationMode::kOpenLoopField:
      return "Open loop field mode";
    case OperationMode::kDiagnostics:
      return "Diagnostics mode";
    case OperationMode::kCoggingCompensationRecording:
      return "Cogging compensation recording mode";
  }
  return "Unknown mode";
}

/// @brief Every SOMANET operation mode, ascending — the manufacturer half of a mode list.
///
/// Taken from two sources that agree exactly: the firmware's own @c state_modes.h and the enum the
/// ESI publishes on 0x6060. Neither knows the -101/-103/-108/-109/-110 "output" modes an earlier
/// table here carried, so they are not listed.
inline constexpr OperationMode kOperationModes[] = {
    OperationMode::kImpedance,
    OperationMode::kJointTorque,
    OperationMode::kSystemIdentification,
    OperationMode::kOpenLoopField,
    OperationMode::kDiagnostics,
    OperationMode::kCoggingCompensationRecording,
};

/// @brief Whether the firmware marks @p mode as deprecated — true for system identification alone,
///        which both the firmware header and the ESI label that way.
constexpr bool isDeprecated(OperationMode mode) {
  return mode == OperationMode::kSystemIdentification;
}

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
  kNotAllowed = 251,  ///< Preconditions not met (wrong mode of operation, say).
  kAborted = 252,     ///< The abort the master requested via 0x1024 was carried out.
  /// No downstream service acknowledged the command (or the abort) within the drive's own
  /// reception timeout — 20000 drive-control cycles, so **about 20 seconds** at a 1 ms loop, and it
  /// scales with that loop's period. Measured at 20.04 s on a SOMANET Integro with the master
  /// waiting 60 s. It means "no service recognised this", not "the drive was slow", so it is also
  /// what a command addressed at a service the firmware does not run comes back as.
  kTimeout = 253,
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
/// no downstream service acknowledges on its own, after about 20 s (see @c
/// OsCommandError::kTimeout) — so size it for the command being run (milliseconds for a register
/// read, tens of seconds for a measurement). Hitting it, or a stop request on @c stop, makes the
/// master abort the running command; @c abortTimeout then bounds how long the drive is given to
/// report that abort.
///
/// **A ceiling below the drive's own is a decision, not a safety margin**: it replaces whatever the
/// drive was going to say with "this master gave up". For a command whose failure mode is a service
/// that will never answer, waiting past 20 s is what turns a bare timeout into the drive's own
/// verdict.
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
///
/// **Not the unit 0x2003:03 stores**, which is µΩ — so keeping a measurement means multiplying by
/// 1000, exactly as for the torque constant. Two of the three winding objects differ from their
/// command's unit this way and the third does not, which is why each says so on itself rather than
/// relying on a rule; see @c somanet::kMotorPhaseResistance.
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
///
/// **The one winding measurement whose unit matches its object**: 0x2003:04 is µH too, so unlike
/// the resistance and the torque constant this value is stored exactly as measured.
struct PhaseInductanceResult {
  uint32_t microhenries = 0;  ///< Phase inductance in µH, exactly as the drive reported it.

  /// @brief One line describing the measurement, ready to put in front of a user.
  std::string describe() const;
};
void to_json(nlohmann::json& j, const PhaseInductanceResult& result);

/// @brief What torque constant measurement (command 10) measured.
///
/// **Signed, unlike its two siblings, and that is the firmware's arithmetic rather than a
/// precaution.** The drive has no torque sensor, so it spins the motor and derives the constant
/// from the back-EMF it generates: the applied voltage minus the drop across the winding
/// impedance. That subtraction is done against the resistance and inductance *configured* in
/// 0x2003:03 and :04, so values larger than the motor's real ones drive the result below zero — a
/// number that is meaningless as a torque constant but is exactly the signal that the drive was
/// measured against the wrong configuration. Reported as it comes rather than clamped or read as a
/// four-billion unsigned value.
///
/// The unit is the drive's own, as with @c PhaseResistanceResult: **mNm per A_rms**, named in the
/// member rather than converted. Note it is *not* the unit 0x2003:02 stores (µNm/A_rms), so
/// keeping a measurement means multiplying by 1000 — see @c somanet::kMotorTorqueConstant.
struct TorqueConstantResult {
  int32_t milliNewtonMetresPerAmpere = 0;  ///< Torque constant in mNm/A_rms, as the drive reported.

  /// @brief One line describing the measurement, ready to put in front of a user.
  std::string describe() const;
};
void to_json(nlohmann::json& j, const TorqueConstantResult& result);

/// @brief What the skipped cycles counter (command 13) reported for one control loop.
///
/// **A single reading means very little on its own** — the counter is cumulative since the service
/// started and nothing resets it, so what carries information is the difference between two
/// readings and the time between them. That is why the service read is part of the result: two
/// readings are only comparable when they addressed the same loop.
struct SkippedCyclesResult {
  somanet::FirmwareService service{somanet::FirmwareService::kDriveControl};  ///< Which loop.
  uint32_t skippedCycles = 0;  ///< Cycles that loop has skipped since it started.

  /// @brief One line describing the reading, ready to put in front of a user.
  std::string describe() const;
};
void to_json(nlohmann::json& j, const SkippedCyclesResult& result);

/// @brief What one trigger-error run did (command 16).
struct TriggerErrorResult {
  somanet::FirmwareService service{somanet::FirmwareService::kDriveControl};  ///< Which service.
  somanet::FirmwareErrorType type{somanet::FirmwareErrorType::kLinkError};    ///< What was asked.
  somanet::FirmwareErrorEffect effect{somanet::FirmwareErrorEffect::kNotImplemented};

  /// Whether the service stopped answering, which for @c kStopsService is the *intended* outcome
  /// and for anything else means something went wrong beyond what was asked for.
  bool serviceStopped = false;

  /// @brief One line describing what happened, ready to put in front of a user.
  std::string describe() const;
};
void to_json(nlohmann::json& j, const TriggerErrorResult& result);

/// @brief What one Kübler register access produced (command 19).
struct KueblerRegisterResult {
  uint8_t address = 0;  ///< The register accessed.
  uint8_t length = 0;   ///< Bytes transferred, 1 to 4.
  bool wrote = false;   ///< Whether this was a write; the drive echoes the value either way.

  /// The value's bytes as they came off the wire, least significant first.
  ///
  /// **Little-endian, which in this family only this command and command 22 are** — every
  /// measurement reply puts the most significant byte first. Kept alongside the assembled number
  /// because a bit field's bytes are what a reader wants, and because a caller checking this
  /// against the specification's worked example needs the raw order.
  std::vector<uint8_t> bytes;

  /// The bytes assembled little-endian into one unsigned number. How to *read* it depends on the
  /// register — see @c somanet::KueblerFormat, which this deliberately does not apply: sign
  /// extension and half-splitting are presentation, and the raw number plus the register's format
  /// is the honest pair to hand over.
  uint32_t value = 0;

  /// @brief One line describing the access, ready to put in front of a user.
  std::string describe() const;
};
void to_json(nlohmann::json& j, const KueblerRegisterResult& result);

/// @brief The maximum one firmware latency reached, as command 22 reported it.
///
/// **A pair, and the second number is the point of it.** The drive records the latency's own
/// maximum, and alongside it whatever latency was *configured* at the moment that maximum happened
/// — so the two together say whether the configured figure covered the worst case actually
/// observed. The configured number is taken from a different firmware variable for each latency
/// (the DCS/MCS alignment offset for @c kSetpoint, the estimated upstream processing time for
/// @c kFeedback), which is why it travels with the latency it belongs to rather than on its own.
///
/// **Both are zero on a drive that has not measured anything**, which is indistinguishable from a
/// genuine zero: the firmware records a value only while the measurement is enabled, and nothing
/// reports whether it is. A zero pair means "no measurement has run", in practice.
struct FirmwareLatencyResult {
  somanet::FirmwareLatency latency{somanet::FirmwareLatency::kSetpoint};  ///< Which latency.

  /// The largest value measured since the measurement was started, in nanoseconds.
  ///
  /// The drive sends 10 ns units in 24 bits, multiplied out here because nanoseconds is exact for
  /// every value it can send. That width is also a ceiling: a latency above 167.77 ms is truncated
  /// by the firmware as it packs the reply, not reported as an overflow.
  uint32_t maximumNanoseconds = 0;

  /// The latency configured when that maximum was reached, in nanoseconds — same unit, same width,
  /// same ceiling.
  uint32_t configuredNanoseconds = 0;

  /// @brief One line describing the reading, ready to put in front of a user.
  std::string describe() const;
};
void to_json(nlohmann::json& j, const FirmwareLatencyResult& result);

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
  /// command, so a non-BiSS or unconfigured encoder comes back as an error rather than a bad
  /// reading.
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
  /// @c readEncoderRegister's BiSS-only rule — and to a configured encoder; the drive refuses
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

  /// @brief Reads and parses the drive's @c .hardware_description file (FoE).
  ///
  /// This is how a device says what it *is* — its product id and revision, serial number, the
  /// components it is built from, and the assembly it was packaged into. It is also the only source
  /// of the descriptor that decides which firmware belongs on it, which is what most callers want
  /// it for (see @c checkFirmwareCompatibility).
  ///
  /// **Readable in BOOT as well as PRE-OP and above.** The bootloader deliberately allows this one
  /// file, which is what lets a compatibility check work on a drive left stranded in BOOT by a
  /// failed install — precisely when knowing whether the package was the right one matters most.
  ///
  /// @return The parsed description, or why it could not be read or made sense of.
  std::expected<HardwareDescription, std::string> readHardwareDescription() const;

  /// @brief Reads and parses the drive's @c .variant file (FoE).
  ///
  /// Only Integro drives carry one. A Node or a Circulo has none, so an empty optional is the
  /// normal answer rather than an error.
  ///
  /// Any failure to read the file is taken as "there is none", because FoE cannot distinguish a
  /// missing file from a failed read. Reading the hardware description first is what makes that
  /// safe: its failure is fatal, so FoE is known to work by the time this runs.
  ///
  /// A file that is read but does not decode is the one error — the device has a variant this build
  /// cannot make sense of.
  ///
  /// @return The parsed file, nothing if the device has none, or why an existing one made no sense.
  std::expected<std::optional<IntegroVariant>, std::string> readIntegroVariant() const;

  /// @brief Reads both files and assembles the descriptors this drive accepts firmware under.
  ///
  /// Two FoE reads, one of which is expected to fail on any drive that is not an Integro. The
  /// hardware description is required — without it there is no descriptor at all — so its failure
  /// is this call's failure.
  ///
  /// @return The device and (where there is one) assembly descriptors, or why they could not be
  ///         assembled.
  std::expected<FullFirmwareDescriptors, std::string> readFullFirmwareDescriptors() const;

  /// @brief Reads what this drive is and decides whether @p packageFilename belongs on it.
  ///
  /// The two FoE reads of @c readFullFirmwareDescriptors, then @c checkFirmwareCompatibility. An
  /// incompatible package is a **verdict, not an error** — the returned value carries both
  /// descriptors and a sentence naming them — so an error here means the question could not be
  /// asked: the filename is not a package name, or the hardware description could not be read.
  ///
  /// Nothing acts on the answer: the firmware installation procedure writes whatever it is given,
  /// on purpose. This is for telling a user before they start.
  ///
  /// @param packageFilename  Bare package filename, with no directory part.
  /// @return The verdict, or why no verdict could be reached.
  std::expected<FirmwareCompatibility, std::string> checkFirmwarePackage(
      std::string_view packageFilename) const;

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

  /// @brief Runs torque constant measurement (OS command 10) and decodes the value it reports.
  ///
  /// Measures how much torque the motor produces per ampere of effective (RMS) current. The drive
  /// cannot measure torque, so it measures the back-EMF instead — which is the same constant seen
  /// from the other side: it spins the motor open-loop up to a fixed electrical frequency, waits
  /// for the load to settle, and works the constant out from the voltage the motor generates at
  /// that speed.
  ///
  /// **The measurement is only as good as the drive's existing motor configuration.** Subtracting
  /// the winding impedance from the applied voltage is what leaves the back-EMF, and the drive
  /// takes the resistance, the inductance and the pole pair count from 0x2003:03, :04 and :01 —
  /// not from anything measured during this command. Measure and *store* those first; a stale
  /// value there produces a wrong constant, silently, and a badly wrong one produces a negative
  /// (see @c TorqueConstantResult).
  ///
  /// Preconditions, all enforced by the drive refusing with OS error 251: operation mode
  /// @c somanet::OperationMode::kDiagnostics, CiA402 state Operation Enabled, no limit switch
  /// active, **and the brake disengaged** if one is configured — the firmware groups this command
  /// with motor phase order and pole pair detection, not with the winding measurements.
  ///
  /// **This command turns the rotor, continuously and for the whole run.** It is not a step or a
  /// fraction of a turn: the motor is spun up over about ten seconds and held there while the
  /// measurement is taken, so it is the longest-moving of the diagnostics commands.
  ///
  /// The command has no command-specific error codes — a failure carries a general one, or none.
  ///
  /// @param config  Timing and cancellation. The default timeout is sized for this command.
  /// @return The measured torque constant, or why no measurement was produced.
  std::expected<TorqueConstantResult, std::string> runTorqueConstantMeasurement(
      const OsCommandConfig& config = {.timeout = std::chrono::seconds(60),
                                       .pollInterval = std::chrono::milliseconds(100)});

  /// @brief Provokes a firmware error or exception in a control service (OS command 16).
  ///
  /// **A destructive test tool.** Eight of the twelve error types do something a drive does not
  /// recover from on its own — see @c somanet::FirmwareErrorEffect — and this issues whichever it
  /// is asked for. It exists because the firmware does, and because reproducing a stopped service
  /// on purpose is how the behaviour around one gets tested.
  ///
  /// What comes back depends on the type, and the result says which happened rather than guessing:
  ///   - @c kNotImplemented — the firmware's case body is empty. The drive answers that the command
  ///     failed and nothing has happened. Seven of the twelve.
  ///   - @c kStopsService — the service executes faulty code or hangs and never answers again, so
  ///     **the command timing out is the intended outcome**, reported as such rather than as a
  ///     failure. The drive keeps its other services; the one addressed is gone until a power
  ///     cycle. A drive that *does* answer means the error was not triggered.
  ///   - @c kRaisesResettableError — a @c DiagErr is reported and the drive reacts per 0x605A. It
  ///     faults and a fault reset clears it.
  ///
  /// **The two services do not answer this alike, and it is a firmware defect rather than a
  /// design.** Motion control sets its failure status before the switch, so the resettable type
  /// overwrites it and answers success; drive control sets the same status *after* the switch, so
  /// it overwrites the success the resettable type just set and answers failure. The error is
  /// raised either way — only the status byte differs — so this reports on what the drive did, not
  /// on which byte came back.
  ///
  /// @param service Which control loop to provoke.
  /// @param type    Which error to raise.
  /// @param config  Timing and cancellation. For a @c kStopsService type the timeout is how long to
  ///                wait before concluding the service is gone, so it should be short.
  /// @return What happened, or why the command could not be issued at all.
  std::expected<TriggerErrorResult, std::string> triggerError(
      somanet::FirmwareService service, somanet::FirmwareErrorType type,
      const OsCommandConfig& config = {.timeout = std::chrono::seconds(3),
                                       .pollInterval = std::chrono::milliseconds(20)});

  /// @brief Reads or writes one register of the Integro's internal (Kübler) encoder (command 19).
  ///
  /// The register map is @c somanet::kKueblerRegisters — the vendor's own draft — and this command
  /// addresses any byte, so a register the draft does not document is read as an unnamed one rather
  /// than refused.
  ///
  /// **The value is little-endian here, which of this family only this command and command 22
  /// are.** The reply puts the least significant byte first, and so does the write value. Reusing
  /// the big-endian decoder the measurements share would read every multi-byte register backwards.
  ///
  /// **@p length is bytes, 1 to 4, and it must match the register's real width** — the encoder
  /// answers a mismatch with @c KueblerRegisterFault::kWrongByteCount rather than truncating. Which
  /// means the 64-bit register 0x04 cannot be read at all: the length byte caps at 4. See
  /// @c somanet::kMaxKueblerRegisterBytes.
  ///
  /// A write is answered the same way a read is, by echoing the register's value, so a write
  /// confirms itself.
  ///
  /// Preconditions: an Integro, with its internal encoder configured and not in bootloader mode.
  ///
  /// @param address Register address.
  /// @param length  Width in bytes, 1 to 4.
  /// @param write   Write @p value rather than read.
  /// @param value   The value to write, little-endian on the wire; ignored for a read.
  /// @param config  Timing and cancellation.
  /// @return What the encoder reported, or why the access did not happen.
  std::expected<KueblerRegisterResult, std::string> accessKueblerRegister(
      uint8_t address, uint8_t length, bool write = false, uint32_t value = 0,
      const OsCommandConfig& config = {.timeout = std::chrono::seconds(5),
                                       .pollInterval = std::chrono::milliseconds(20)});

  /// @brief Chooses where the velocity control loop takes its feedback from (OS command 18).
  ///
  /// See @c somanet::VelocitySource for which source is the default, which is not what the OS
  /// command specification says: on an Integro build the firmware selects the encoder's own
  /// velocity at start-up, so a run asking for @c kEncoder there changes nothing and the
  /// informative direction is @c kFirmware.
  ///
  /// **No preconditions and nothing to restore.** The command is accepted in any state, and the
  /// choice holds until another run changes it or the drive is power-cycled — nothing reports it
  /// back. It takes effect only for a configured Kübler encoder; on any other encoder it is
  /// accepted and does nothing.
  ///
  /// It commands no motion, but it is not inert on a moving drive: the velocity loop's feedback
  /// changes under it, and the two sources do not agree exactly, so a closed loop can be disturbed
  /// by the switch.
  ///
  /// @param source Which velocity to feed the loop.
  /// @param config Timing and cancellation.
  /// @return Void once the drive accepted the choice, otherwise why it did not.
  std::expected<void, std::string> setVelocitySource(
      somanet::VelocitySource source,
      const OsCommandConfig& config = {.timeout = std::chrono::seconds(5),
                                       .pollInterval = std::chrono::milliseconds(20)});

  /// @brief Starts measuring one internal firmware latency (OS command 22, action 0).
  ///
  /// Clears whatever that latency had recorded and enables its measurement. The drive then keeps
  /// the maximum of every drive control cycle until @c stopFirmwareLatencyMeasurements ends it or
  /// the drive is power-cycled; the other latency is untouched either way.
  ///
  /// **Nothing reports whether a measurement is running**, so starting one twice is
  /// indistinguishable from starting it once, except that the second start throws away what the
  /// first had collected.
  ///
  /// **No preconditions and nothing to restore.** The command is accepted in any state and moves
  /// nothing — the measurement is two timer reads and a comparison inside a cycle the drive was
  /// running anyway.
  ///
  /// @param latency Which latency to measure.
  /// @param config  Timing and cancellation.
  /// @return Void once the drive started measuring, otherwise why it did not.
  std::expected<void, std::string> startFirmwareLatencyMeasurement(
      somanet::FirmwareLatency latency,
      const OsCommandConfig& config = {.timeout = std::chrono::seconds(5),
                                       .pollInterval = std::chrono::milliseconds(20)});

  /// @brief Reads and clears one firmware latency's recorded maximum (OS command 22, action 1).
  ///
  /// **The read is destructive and the measurement survives it**: the drive answers with the
  /// maximum, then zeroes it, and goes on measuring. So consecutive reads each describe the window
  /// since the previous one rather than the whole run — which is what makes a maximum useful over
  /// time, and what means a read cannot be repeated to double-check a surprising figure.
  ///
  /// Reading a latency that was never started answers zero for both numbers; see
  /// @c FirmwareLatencyResult.
  ///
  /// @param latency Which latency to read.
  /// @param config  Timing and cancellation.
  /// @return The maximum and the latency configured when it happened, or why it could not be read.
  std::expected<FirmwareLatencyResult, std::string> readMaximumFirmwareLatency(
      somanet::FirmwareLatency latency,
      const OsCommandConfig& config = {.timeout = std::chrono::seconds(5),
                                       .pollInterval = std::chrono::milliseconds(20)});

  /// @brief Stops measuring **both** firmware latencies (OS command 22, action 2).
  ///
  /// The command has no per-latency stop — one action disables both — so stopping one measurement
  /// necessarily ends the other. What each latency had recorded is left alone and can still be
  /// read.
  ///
  /// @param config Timing and cancellation.
  /// @return Void once the drive stopped measuring, otherwise why it did not.
  std::expected<void, std::string> stopFirmwareLatencyMeasurements(
      const OsCommandConfig& config = {.timeout = std::chrono::seconds(5),
                                       .pollInterval = std::chrono::milliseconds(20)});

  /// @brief Reads a control loop's skipped-cycle counter (OS command 13).
  ///
  /// How many cycles @p service has failed to start on time since it began running. The firmware
  /// counts a cycle as skipped when it starts late enough to miss its slot, and adds the whole
  /// backlog when several are missed at once — so the figure is missed *cycles*, not missed
  /// deadlines. **It is cumulative and nothing resets it**, so read it twice and subtract: a
  /// counter that is large but unchanging describes a startup transient, and a small one that keeps
  /// climbing describes a drive that is still missing cycles now.
  ///
  /// The two loops are counted separately and a request addressed at one says nothing about the
  /// other, which is why the service is a parameter rather than a detail.
  ///
  /// **The drive reports the same event twice, and the other half is easier to miss**: when a cycle
  /// is skipped while a controller is enabled, the firmware also raises a @c CtrlCyEx warning that
  /// lands in the error report (0x203F). A rising counter with no warning means the cycles were
  /// skipped while the drive was disabled.
  ///
  /// **No preconditions.** The command needs no operation mode, no CiA402 state and no brake, and
  /// it moves nothing — it is a pure read, safe to run on a drive that is enabled and moving.
  ///
  /// @param service Which control loop to ask.
  /// @param config  Timing and cancellation. The default timeout clears the drive's own ~20 s
  ///                reception timeout (measured; see @c OsCommandError::kTimeout), so a firmware
  ///                that does not run the addressed service answers for itself instead of being
  ///                aborted from here first — which is the difference between "no such service" and
  ///                a bare "this master gave up". The read itself takes one control cycle.
  /// @return The counter and the service it came from, or why it could not be read.
  std::expected<SkippedCyclesResult, std::string> readSkippedCycles(
      somanet::FirmwareService service,
      const OsCommandConfig& config = {.timeout = std::chrono::seconds(30),
                                       .pollInterval = std::chrono::milliseconds(20)});

  /// @brief Starts or stops ignoring a BiSS encoder's status bits (OS command 14).
  ///
  /// Every BiSS frame the encoder returns carries two status bits alongside the position, by which
  /// the encoder reports on its own reading. The firmware checks them each cycle and acts:
  /// a warning goes in the error report as @c BisWnBit, and an **error faults the drive into active
  /// short circuit** (@c BisErBit, reaction ASC — the phases are shorted whatever 0x605A says), and
  /// on an iC-MU it additionally reads the chip's status registers to find out why. Which bit
  /// pattern means what depends on the encoder's configured active level (0x2110/0x2112).
  ///
  /// Ignoring them switches that whole check off for the addressed encoder: no warning, no fault,
  /// no register read. **The drive then keeps running on an encoder that is saying its position is
  /// unreliable**, which is why this exists for bringing up and diagnosing an encoder rather than
  /// for running a machine.
  ///
  /// **There is nothing to restore and nothing restores it.** The flag lives in the BiSS service's
  /// memory, so it holds until another run turns it back on or the drive is power-cycled — it is
  /// not a mode that ends with the operation that set it.
  ///
  /// Preconditions: the addressed encoder must be **configured and be a BiSS encoder**, because the
  /// BiSS service instance for that encoder is what answers. When it is not, *nothing* answers, and
  /// the drive reports OS error 253 after its whole ~20 s reception timeout — which this decodes
  /// into what actually happened rather than passing on a bare "timeout".
  ///
  /// @param encoder Which encoder's status bits to act on.
  /// @param ignore  True to stop the firmware acting on them, false to restore the default.
  /// @param config  Timing and cancellation. The default timeout clears the drive's own ~20 s
  ///                reception timeout, so an encoder that is not BiSS is reported as such.
  /// @return Void once the drive applied the change, otherwise why it did not.
  std::expected<void, std::string> setIgnoreBissStatusBits(
      somanet::EncoderOrdinal encoder, bool ignore,
      const OsCommandConfig& config = {.timeout = std::chrono::seconds(30),
                                       .pollInterval = std::chrono::milliseconds(20)});

  /// @brief Writes one system-identification setting (OS command 15).
  ///
  /// The command carries a parameter index and a 32-bit value, so a configured run is this called
  /// once per setting — there is no writing them together. The drive stores each as it arrives and
  /// answers immediately; nothing is checked at this point and nothing reads them back.
  ///
  /// **Validation happens later, and its failure is a drive fault.** The firmware checks the
  /// configuration on the rising edge of @c SystemIdentificationParameter::kStartProcedure, inside
  /// the motion control loop — and a configuration outside
  /// @c somanet::kMinChirpFrequencyMilliHz and friends does not merely fail to start: it raises
  /// @c IvldPara with a quick-stop reaction. So every one of these writes succeeds, and a bad set
  /// of numbers surfaces as a faulted drive when it is armed. Check before writing, which is what
  /// @c parseSystemIdentificationRequest does.
  ///
  /// **The amplitude is not among the checked values** — neither here nor in the firmware — and it
  /// is a torque command in per-mille of rated torque. Nothing will refuse an unreasonable one.
  ///
  /// @param parameter Which setting to write.
  /// @param value     Its value, in the units @c SystemIdentificationParameter names.
  /// @param config    Timing and cancellation.
  /// @return Void once the drive stored it, otherwise why it did not. An unknown parameter index
  ///         comes back as the drive's status 2, reported as such.
  std::expected<void, std::string> setSystemIdentificationParameter(
      somanet::SystemIdentificationParameter parameter, uint32_t value,
      const OsCommandConfig& config = {.timeout = std::chrono::seconds(5),
                                       .pollInterval = std::chrono::milliseconds(20)});

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
