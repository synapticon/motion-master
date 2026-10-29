#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "node/ic_mu_encoder.h"
#include "node/somanet_drive.h"

namespace mm::node::somanet {

/// @file
/// @brief What the calibration of an iC-MU computes and writes, as pure functions and data.
///
/// The MU_3SL library computes the corrections. Everything around it is here: the start values,
/// the byte layout of the registers, the pass test, the motion that records the tracks, and the
/// check of what a recording covered. None of it calls MU_3SL, so it builds and is tested on every
/// platform.

/// @brief Where the ring of an encoder sits.
///
/// The integrated encoder of a board has its inner ring on sensor port 1 and its outer ring on
/// port 2. An external encoder on port 3 to @c kMaxSensorPort does not say, so the request names
/// it.
enum class IcMuRingPosition : uint8_t {
  kInner,
  kOuter,
};

/// @brief Name of a ring position, as the API renders it. Never returns @c nullptr.
constexpr std::string_view toString(IcMuRingPosition position) {
  switch (position) {
    case IcMuRingPosition::kInner:
      return "inner";
    case IcMuRingPosition::kOuter:
      return "outer";
  }
  return "unknown";
}

/// @brief Parses "inner" or "outer". Nothing otherwise.
std::optional<IcMuRingPosition> parseIcMuRingPosition(std::string_view token);

/// @brief The ring position an integrated sensor port gives, or nothing for an external port.
std::optional<IcMuRingPosition> icMuRingPositionOfPort(uint8_t sensorPort);

/// @brief The parts that share one scale of @c PH_M.
///
/// iC-MU datasheet Rev B1, Elec. Char. 505: a @c PH_M step is a smaller angle on the iC-MU128
/// than on the iC-MU150 and the iC-MU200. @c PH_N has one scale on all three (Elec. Char. 506).
enum class IcMuPartClass : uint8_t {
  kIcMu128,
  kIcMu150Or200,
};

/// @brief The part class that @p hardRev names, or nothing for a code that names no iC-MU and for
///        the plain iC-MU, whose PH scale is not known here.
std::optional<IcMuPartClass> icMuPartClass(uint8_t hardRev);

/// @brief The values a calibration starts from for one kind of encoder.
struct IcMuStartValues {
  uint8_t masterPhase = 0;  ///< @c PH_M with @c PHR_M, register 0x04.
  uint8_t noniusPhase = 0;  ///< @c PH_N with @c PHR_N, register 0x0A.

  /// The velocity of the ring of an encoder on the motor shaft, in rpm.
  double motorShaftRingVelocityRpm = 0;

  /// The velocity of the ring below which a recording in a narrow range may not converge, in rpm,
  /// for an encoder on the motor shaft and on the driving shaft.
  double motorShaftLowVelocityRpm = 0;
  double drivingShaftLowVelocityRpm = 0;

  /// False for a row nobody measured: the values of another ring, which a run uses with a warning.
  bool measured = false;
};

/// @brief The start values for a ring of @p masterPolePairs at @p position on a part of
///        @p partClass, or nothing when no row exists.
std::optional<IcMuStartValues> icMuStartValues(uint8_t masterPolePairs, IcMuRingPosition position,
                                               IcMuPartClass partClass);

/// @brief The velocity of the ring of an encoder on the driving shaft, in rpm.
inline constexpr double kIcMuDrivingShaftRingVelocityRpm = 15.0;

/// @brief The velocity of the ring a calibration runs at when the request names none, in rpm.
double icMuDefaultRingVelocity(const IcMuStartValues& values, EncoderShaft shaft);

/// @brief The phase of one track, as register 0x04 or 0x0A holds it.
struct IcMuPhase {
  uint8_t phase = 0;       ///< @c PH, bits 6:0.
  uint8_t phaseRange = 0;  ///< @c PHR, bit 7.

  bool operator==(const IcMuPhase&) const = default;
};

/// @brief Register 0x04 or 0x0A for @p phase.
uint8_t icMuPhaseRegister(IcMuPhase phase);

/// @brief The phase register @p value holds.
IcMuPhase icMuPhaseFromRegister(uint8_t value);

/// @brief The nonius track offset table: @c SPO_BASE and @c SPO_0 to @c SPO_14, four bits each.
struct IcMuNoniusOffsetTable {
  uint8_t base = 0;
  std::array<uint8_t, 15> offsets{};

  bool operator==(const IcMuNoniusOffsetTable&) const = default;
};

/// @brief The first register of the nonius track offset table. It runs to 0x59.
inline constexpr uint8_t kIcMuNoniusTableRegister = 0x52;

/// @brief The writes of registers 0x52 to 0x59 for @p table.
///
/// Register 0x52 holds @c SPO_0 in bits 7:4 and @c SPO_BASE in bits 3:0. Each following register
/// holds the next two offsets, the higher one in bits 7:4 (iC-MU datasheet Rev B1, Table 90).
std::vector<IcMuRegisterWrite> icMuNoniusTableWrites(const IcMuNoniusOffsetTable& table);

/// @brief The table registers 0x52 to 0x59 hold, from their eight values in that order.
IcMuNoniusOffsetTable icMuNoniusTableFromRegisters(std::span<const uint8_t, 8> registers);

/// @brief The two tracks of an iC-MU.
enum class IcMuTrack : uint8_t {
  kMaster,
  kNonius,
};

/// @brief The analog adjustment registers of one track, as the encoder holds them.
struct IcMuAnalogAdjustments {
  uint8_t gain = 0;          ///< @c GX, register 0x01 or 0x07.
  uint8_t sineOffset = 0;    ///< @c VOSS, register 0x02 or 0x08.
  uint8_t cosineOffset = 0;  ///< @c VOSC, register 0x03 or 0x09.
  IcMuPhase phase;           ///< @c PH and @c PHR, register 0x04 or 0x0A.

  bool operator==(const IcMuAnalogAdjustments&) const = default;
};

/// @brief The registers of @p track that hold its adjustments, in the order @c GX, @c VOSS,
///        @c VOSC, @c PH.
std::array<uint8_t, 4> icMuAnalogRegisters(IcMuTrack track);

/// @brief The adjustments of a track from its four registers, in the order of
///        @c icMuAnalogRegisters.
IcMuAnalogAdjustments icMuAnalogFromRegisters(std::span<const uint8_t, 4> values);

/// @brief The writes of @p adjustments into the registers of @p track: @c VOSS, @c VOSC, @c GX,
///        then @c PH.
std::vector<IcMuRegisterWrite> icMuAnalogWrites(IcMuTrack track,
                                                const IcMuAnalogAdjustments& adjustments);

/// @brief The writes a full calibration starts with: @c PH_M and @c PH_N from @p values, the
///        offsets and gains of both tracks at 0, and the nonius table at 0.
std::vector<IcMuRegisterWrite> icMuStartWrites(const IcMuStartValues& values);

/// @brief What MU_3SL reports for one track after an iteration: the error that remains, in LSB of
///        each register, and the phase range of the track.
struct IcMuTrackResidual {
  double sineOffset = 0;
  double cosineOffset = 0;
  double gain = 0;
  double phase = 0;
  uint8_t phaseRange = 0;
};

/// @brief The tolerance of the pass test in @p iteration, counted from 1, in LSB.
///
/// The test widens as the run goes on: 1 up to iteration 5, 3 up to iteration 9, 5 after that.
int icMuTolerance(int iteration);

/// @brief The iterations after which a run without a pass fails.
inline constexpr int kIcMuMaxIterations = 13;

/// @brief Whether @p residual is within @p tolerance on @p track.
///
/// Every value is rounded to the nearest LSB. The phase is first divided by the step of its phase
/// range: 1 or 2 on the master track, 3 or 5 on the nonius track. A phase range other than 0 or 1
/// fails. The phase passes at a tolerance of at least 1.
bool icMuTrackPasses(IcMuTrack track, const IcMuTrackResidual& residual, int tolerance);

/// @brief How much the nonius phase margin of a calibrated encoder tolerates.
enum class IcMuMarginBand : uint8_t {
  kRobust,              ///< 50 % and more.
  kSmallDisplacements,  ///< From 30 % to below 50 %.
  kAvoidDisplacements,  ///< From 10 % to below 30 %.
  kTooHigh,             ///< Below 10 %: the calibration fails.
};

/// @brief Name of a margin band, as the API renders it. Never returns @c nullptr.
constexpr std::string_view toString(IcMuMarginBand band) {
  switch (band) {
    case IcMuMarginBand::kRobust:
      return "robust";
    case IcMuMarginBand::kSmallDisplacements:
      return "small-displacements";
    case IcMuMarginBand::kAvoidDisplacements:
      return "avoid-displacements";
    case IcMuMarginBand::kTooHigh:
      return "too-high";
  }
  return "unknown";
}

/// @brief The band of @p marginPercent, the smaller of the two nonius phase margins.
IcMuMarginBand icMuMarginBand(double marginPercent);

/// @brief What @p band means for the user, in one or two sentences.
std::string_view icMuMarginMessage(IcMuMarginBand band);

/// @brief The time a recording runs at constant velocity when nothing else is needed.
inline constexpr std::chrono::milliseconds kIcMuStandardRecording{6000};

/// @brief The longest encoder raw data recording the drive takes.
inline constexpr std::chrono::milliseconds kIcMuLongestRecording{10000};

/// @brief The part of a turn of the ring that a recording covers twice.
inline constexpr double kIcMuRingOverlapDegrees = 5.0;

/// @brief The revolutions of a move: a ramp up at @p accelerationRpmPerSecond, @p constant at
///        @p velocityRpm, and a ramp down at the same rate.
double icMuMoveRevolutions(double velocityRpm, double accelerationRpmPerSecond,
                           std::chrono::milliseconds constant);

/// @brief The velocity at which a move of @p travelRevolutions fits exactly, the inverse of
///        @c icMuMoveRevolutions.
double icMuVelocityForMove(double travelRevolutions, double accelerationRpmPerSecond,
                           std::chrono::milliseconds constant);

/// @brief What the motion of a calibration needs to know about the drive.
///
/// Positions are in revolutions of the driving shaft, which is where the velocity and the
/// software position limits apply.
struct IcMuMotionInput {
  double gearRatio = 1;         ///< Motor revolutions per driving shaft revolution, at least 1.
  double maxMotorSpeedRpm = 0;  ///< 0x6080, in rpm of the motor.
  EncoderShaft shaft{EncoderShaft::kMotor};

  /// The velocity of the ring, in rpm: the request, or @c icMuDefaultRingVelocity.
  double ringVelocityRpm = 0;
  double lowVelocityRpm = 0;  ///< Of the ring, from the start values for @c shaft.

  double positionRevolutions = 0;  ///< 0x6064.

  /// 0x607D, or nothing when the limits do not bound the motion.
  std::optional<std::pair<double, double>> limitRevolutions;
};

/// @brief How a calibration moves the axis.
struct IcMuMotionPlan {
  double velocityRpm = 0;               ///< On the driving shaft. The direction alternates.
  double accelerationRpmPerSecond = 0;  ///< On the driving shaft, for 0x6083 and 0x6084.
  std::chrono::milliseconds recording{kIcMuStandardRecording};  ///< At constant velocity.

  /// Where to move the axis before the first iteration, or nothing to start where it is.
  std::optional<double> startRevolutions;

  /// Whether the limits bound the motion. The body then plans the recording again with
  /// @c planIcMuNarrowRange from where the axis really stopped.
  bool limitedRange = false;

  std::vector<std::string> warnings;  ///< Each one for the user, in the order they arose.
};

/// @brief Plans the motion, or says why there is no room for it.
///
/// The velocity of the driving shaft is the velocity of the ring over
/// @c icMuRingTurnsPerRevolution. It is capped at 0.95 × the maximum motor speed over the gear
/// ratio. The acceleration is 1000 rpm/s over the gear ratio.
///
/// A move that fits from the current position leaves the axis where it is. One that fits between
/// the limits from another position starts there. One that does not fit between the limits runs at
/// four times the acceleration from 3° above the lower limit, at the velocity at which a recording
/// of 6 s, or else 10 s, covers a turn of the ring plus @c kIcMuRingOverlapDegrees. When neither
/// fits, the plan is refused before the motor moves, and the message says how many degrees to
/// widen the limits by.
///
/// A ring velocity that does not turn the ring once plus @c kIcMuRingOverlapDegrees in 6 s takes
/// the 10 s recording, and one that does not even then is refused.
std::expected<IcMuMotionPlan, std::string> planIcMuMotion(const IcMuMotionInput& input);

/// @brief The ring turns per driving shaft revolution: the gear ratio for a motor-shaft encoder,
///        1 for a driving-shaft encoder.
double icMuRingTurnsPerRevolution(EncoderShaft shaft, double gearRatio);

/// @brief Plans the recording of a narrow range once the axis is at its start, or refuses.
///
/// @param travelRevolutions        The travel from the start to 3° below the upper limit.
/// @param accelerationRpmPerSecond The acceleration of the plan.
/// @param ringTurnsPerRevolution   See @c icMuRingTurnsPerRevolution.
/// @param lowVelocityRpm           The ring velocity below which the plan warns.
std::expected<IcMuMotionPlan, std::string> planIcMuNarrowRange(double travelRevolutions,
                                                               double accelerationRpmPerSecond,
                                                               double ringTurnsPerRevolution,
                                                               double lowVelocityRpm);

/// @brief The revolutions of the ring a recording covered, from its master track counts.
///
/// A step of half a period or more is taken the shorter way round, which holds because the track
/// moves far less than that between two samples.
double icMuRecordedRingRevolutions(std::span<const HrdEncoderSample> samples,
                                   uint8_t masterPolePairs);

/// @brief The warning for a recording that missed at least one degree of a turn of the ring, or
///        nothing.
std::optional<std::string> icMuCoverageWarning(double recordedRevolutions);

/// @brief The samples a calibration analyses, from a recording of @p recording at constant
/// velocity: at most one per millisecond of it.
///
/// The drive writes one sample per millisecond for the duration it was given and flushes its last
/// file at its real length (somanet_software, drive_control_service.xc and storage_service.xc), so
/// a recording holds that count. Anything past it is cut; a shorter recording is kept as it is.
std::span<const HrdEncoderSample> icMuCalibrationSamples(std::span<const HrdEncoderSample> samples,
                                                         std::chrono::milliseconds recording);

/// @brief Why the calibration refuses firmware @p version, or nothing.
///
/// The pre-releases v5.4.0-rc.2 and v5.4.0-rc.3 record a number of samples per millisecond that
/// depends on the control loop period, so a recording of them does not hold one sample per
/// millisecond (somanet_software f74b4e262, fixed in 4406850b1).
std::optional<std::string> icMuRecordingFirmwareError(std::string_view version);

}  // namespace mm::node::somanet
