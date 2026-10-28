#include "node/ic_mu_calibration.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace mm::node::somanet {

namespace {

// One master track period in raw counts: the track angle has 14 bits.
constexpr int64_t kCountsPerPeriod = int64_t{1} << 14;

// The acceleration of a calibration on the motor, in rpm/s.
constexpr double kMotorAccelerationRpmPerSecond = 1000.0;

// The share of the maximum motor speed a calibration may use.
constexpr double kMaxSpeedShare = 0.95;

// A driving-shaft calibration needs more than this, in rpm of the driving shaft.
constexpr double kMinDrivingShaftVelocityRpm = 10.0;

// The narrowest range between the software position limits a calibration takes, in degrees.
constexpr double kMinRangeDegrees = 20.0;

// The distance a calibration keeps from a software position limit, in degrees.
constexpr double kLimitMarginDegrees = 3.0;

// The distance from a start below which the axis is not moved there first, in degrees.
constexpr double kStartToleranceDegrees = 1.0;

constexpr double revolutions(double degrees) { return degrees / 360.0; }

struct StartValueRow {
  uint8_t masterPolePairs = 0;
  IcMuRingPosition position{IcMuRingPosition::kInner};
  IcMuPartClass partClass{IcMuPartClass::kIcMu150Or200};
  IcMuStartValues values;
};

// The start values measured for the rings in the field. The two rows marked unmeasured repeat the
// values of the 32-pole-pair inner ring, because nobody measured their own; a run warns about it.
constexpr std::array kStartValueRows = {
    StartValueRow{32,
                  IcMuRingPosition::kInner,
                  IcMuPartClass::kIcMu150Or200,
                  {0x00, 0x00, 90, 70, 1.1, true}},
    StartValueRow{32,
                  IcMuRingPosition::kOuter,
                  IcMuPartClass::kIcMu150Or200,
                  {0x3F, 0x3F, 90, 70, 1.1, true}},
    StartValueRow{64,
                  IcMuRingPosition::kInner,
                  IcMuPartClass::kIcMu150Or200,
                  {0x4D, 0x48, 45, 50, 1.1, true}},
    StartValueRow{64,
                  IcMuRingPosition::kOuter,
                  IcMuPartClass::kIcMu150Or200,
                  {0x21, 0x1A, 45, 50, 1.1, true}},
    StartValueRow{16,
                  IcMuRingPosition::kInner,
                  IcMuPartClass::kIcMu150Or200,
                  {0x00, 0x00, 90, 70, 1.1, false}},
    StartValueRow{
        32, IcMuRingPosition::kInner, IcMuPartClass::kIcMu128, {0x00, 0x00, 90, 70, 1.1, false}},
};

double roundedAbs(double value) { return std::abs(std::round(value)); }

}  // namespace

std::optional<IcMuRingPosition> parseIcMuRingPosition(std::string_view token) {
  for (const auto position : {IcMuRingPosition::kInner, IcMuRingPosition::kOuter}) {
    if (token == toString(position)) {
      return position;
    }
  }
  return std::nullopt;
}

std::optional<IcMuRingPosition> icMuRingPositionOfPort(uint8_t sensorPort) {
  switch (sensorPort) {
    case 1:
      return IcMuRingPosition::kInner;
    case 2:
      return IcMuRingPosition::kOuter;
    default:
      return std::nullopt;
  }
}

std::optional<IcMuPartClass> icMuPartClass(uint8_t hardRev) {
  switch (hardRev >> 4) {
    case 0x0:
      return IcMuPartClass::kIcMu128;
    case 0x1:
    case 0x2:
      return IcMuPartClass::kIcMu150Or200;
    default:
      return std::nullopt;
  }
}

std::optional<IcMuStartValues> icMuStartValues(uint8_t masterPolePairs, IcMuRingPosition position,
                                               IcMuPartClass partClass) {
  const auto row = std::ranges::find_if(kStartValueRows, [&](const StartValueRow& r) {
    return r.masterPolePairs == masterPolePairs && r.position == position &&
           r.partClass == partClass;
  });
  if (row == kStartValueRows.end()) {
    return std::nullopt;
  }
  return row->values;
}

uint8_t icMuPhaseRegister(IcMuPhase phase) {
  return static_cast<uint8_t>(((phase.phaseRange & 0x01) << 7) | (phase.phase & 0x7F));
}

IcMuPhase icMuPhaseFromRegister(uint8_t value) {
  return {.phase = static_cast<uint8_t>(value & 0x7F),
          .phaseRange = static_cast<uint8_t>((value >> 7) & 0x01)};
}

std::vector<IcMuRegisterWrite> icMuNoniusTableWrites(const IcMuNoniusOffsetTable& table) {
  auto pack = [](uint8_t high, uint8_t low) {
    return static_cast<uint8_t>(((high & 0x0F) << 4) | (low & 0x0F));
  };
  std::vector<IcMuRegisterWrite> writes;
  writes.push_back({kIcMuNoniusTableRegister, pack(table.offsets[0], table.base)});
  for (size_t i = 0; i < 7; ++i) {
    writes.push_back({static_cast<uint8_t>(kIcMuNoniusTableRegister + 1 + i),
                      pack(table.offsets[(2 * i) + 2], table.offsets[(2 * i) + 1])});
  }
  return writes;
}

IcMuNoniusOffsetTable icMuNoniusTableFromRegisters(std::span<const uint8_t, 8> registers) {
  IcMuNoniusOffsetTable table;
  table.base = registers[0] & 0x0F;
  table.offsets[0] = (registers[0] >> 4) & 0x0F;
  for (size_t i = 0; i < 7; ++i) {
    table.offsets[(2 * i) + 1] = registers[i + 1] & 0x0F;
    table.offsets[(2 * i) + 2] = (registers[i + 1] >> 4) & 0x0F;
  }
  return table;
}

int icMuTolerance(int iteration) {
  if (iteration <= 5) {
    return 1;
  }
  return iteration <= 9 ? 3 : 5;
}

bool icMuTrackPasses(IcMuTrack track, const IcMuTrackResidual& residual, int tolerance) {
  if (residual.phaseRange > 1) {
    return false;
  }
  // One phase step means a different angle in each phase range and on each track.
  const double step = track == IcMuTrack::kMaster ? (residual.phaseRange == 0 ? 1.0 : 2.0)
                                                  : (residual.phaseRange == 0 ? 3.0 : 5.0);
  const auto limit = static_cast<double>(tolerance);
  return roundedAbs(residual.sineOffset) <= limit && roundedAbs(residual.cosineOffset) <= limit &&
         roundedAbs(residual.gain) <= limit &&
         roundedAbs(residual.phase / step) <= std::max(limit, 1.0);
}

double icMuMoveRevolutions(double velocityRpm, double accelerationRpmPerSecond,
                           std::chrono::milliseconds constant) {
  const double seconds = std::chrono::duration<double>(constant).count();
  return (velocityRpm * velocityRpm / (60.0 * accelerationRpmPerSecond)) +
         (velocityRpm * seconds / 60.0);
}

double icMuVelocityForMove(double travelRevolutions, double accelerationRpmPerSecond,
                           std::chrono::milliseconds constant) {
  const double at = accelerationRpmPerSecond * std::chrono::duration<double>(constant).count();
  return (-at + std::sqrt((at * at) + (240.0 * accelerationRpmPerSecond * travelRevolutions))) /
         2.0;
}

double icMuRingTurnsPerRevolution(EncoderShaft shaft, double gearRatio) {
  return shaft == EncoderShaft::kMotor ? gearRatio : 1.0;
}

std::expected<IcMuMotionPlan, std::string> planIcMuNarrowRange(double travelRevolutions,
                                                               double accelerationRpmPerSecond,
                                                               double ringTurnsPerRevolution,
                                                               double lowVelocityRpm) {
  const double required = (1.0 + revolutions(kIcMuRingOverlapDegrees)) / ringTurnsPerRevolution;
  // The standard recording first, then the longest the drive takes: a longer recording needs a
  // lower velocity, so the ramps take less of the travel.
  for (const auto recording : {kIcMuStandardRecording, kIcMuLongestRecording}) {
    const double velocity =
        icMuVelocityForMove(travelRevolutions, accelerationRpmPerSecond, recording);
    if (velocity * std::chrono::duration<double>(recording).count() / 60.0 >= required) {
      IcMuMotionPlan plan{.velocityRpm = velocity,
                          .accelerationRpmPerSecond = accelerationRpmPerSecond,
                          .recording = recording,
                          .startRevolutions = std::nullopt,
                          .limitedRange = true,
                          .warnings = {}};
      if (velocity * ringTurnsPerRevolution < lowVelocityRpm) {
        plan.warnings.emplace_back(
            "The range between the software position limits is too narrow for the recommended "
            "calibration velocity. The calibration might not converge.");
      }
      return plan;
    }
  }
  // The travel the longest recording needs at the lowest velocity that still covers the turn.
  const double seconds = std::chrono::duration<double>(kIcMuLongestRecording).count();
  const double minimumVelocity = 60.0 * required / seconds;
  const double needed =
      required + (minimumVelocity * minimumVelocity / (60.0 * accelerationRpmPerSecond));
  return std::unexpected(std::format(
      "the software position limits (0x607D) are too narrow to record a full turn of the encoder "
      "ring at constant velocity; widen the range between them by at least {:.0f} degrees",
      std::ceil((needed - travelRevolutions) * 360.0)));
}

std::expected<IcMuMotionPlan, std::string> planIcMuMotion(const IcMuMotionInput& input) {
  const double gear = std::max(input.gearRatio, 1.0);
  const double maxVelocity = kMaxSpeedShare * input.maxMotorSpeedRpm / gear;

  IcMuMotionPlan plan;
  plan.accelerationRpmPerSecond = kMotorAccelerationRpmPerSecond / gear;
  if (input.shaft == EncoderShaft::kDriving) {
    if (maxVelocity <= kMinDrivingShaftVelocityRpm) {
      return std::unexpected(std::format(
          "the maximum motor speed (0x6080) allows {:.1f} rpm on the driving shaft, and a "
          "calibration of a driving-shaft encoder needs more than {:.0f} rpm",
          maxVelocity, kMinDrivingShaftVelocityRpm));
    }
    plan.velocityRpm = kIcMuDrivingShaftVelocityRpm;
  } else {
    plan.velocityRpm = input.baseVelocityRpm / gear;
  }
  auto cap = [&] {
    if (plan.velocityRpm > maxVelocity) {
      plan.velocityRpm = maxVelocity;
      plan.warnings.emplace_back(
          "The maximum motor speed (0x6080) is too low for the recommended calibration velocity. "
          "A faster move can leave a larger nonius phase margin.");
    }
  };
  cap();

  const double travel =
      icMuMoveRevolutions(plan.velocityRpm, plan.accelerationRpmPerSecond, plan.recording);
  if (!input.limitRevolutions ||
      input.positionRevolutions + travel <= input.limitRevolutions->second) {
    return plan;
  }
  const auto [lower, upper] = *input.limitRevolutions;
  if (upper - lower < revolutions(kMinRangeDegrees)) {
    return std::unexpected(std::format(
        "the software position limits (0x607D) leave {:.1f} degrees, and a calibration needs at "
        "least {:.0f}",
        (upper - lower) * 360.0, kMinRangeDegrees));
  }
  const double margin = revolutions(kLimitMarginDegrees);
  const double tolerance = revolutions(kStartToleranceDegrees);

  if (travel <= upper - lower) {
    // The move fits between the limits, only not from here.
    const double start = upper - margin - travel;
    if (input.positionRevolutions > start + tolerance) {
      plan.startRevolutions = start;
    }
    return plan;
  }

  // The move does not fit: start near the lower limit, accelerate faster, and pick the velocity at
  // which the recording still covers a turn of the ring.
  const double start = lower + margin;
  auto narrow =
      planIcMuNarrowRange(upper - margin - start, plan.accelerationRpmPerSecond * 4.0,
                          icMuRingTurnsPerRevolution(input.shaft, gear), input.lowVelocityRpm);
  if (!narrow) {
    return std::unexpected(narrow.error());
  }
  std::vector<std::string> warnings = std::move(plan.warnings);
  plan = std::move(*narrow);
  warnings.insert(warnings.end(), plan.warnings.begin(), plan.warnings.end());
  plan.warnings = std::move(warnings);
  if (std::abs(input.positionRevolutions - start) > tolerance) {
    plan.startRevolutions = start;
  }
  cap();
  return plan;
}

double icMuRecordedRingRevolutions(std::span<const HrdEncoderSample> samples,
                                   uint8_t masterPolePairs) {
  if (samples.empty() || masterPolePairs == 0) {
    return 0;
  }
  int64_t position = 0;
  for (size_t i = 1; i < samples.size(); ++i) {
    int64_t step = int64_t{samples[i].masterCount} - int64_t{samples[i - 1].masterCount};
    if (step >= kCountsPerPeriod / 2) {
      step -= kCountsPerPeriod;
    } else if (step < -kCountsPerPeriod / 2) {
      step += kCountsPerPeriod;
    }
    position += step;
  }
  return static_cast<double>(std::abs(position)) /
         static_cast<double>(kCountsPerPeriod * masterPolePairs);
}

std::optional<std::string> icMuCoverageWarning(double recordedRevolutions) {
  if (recordedRevolutions >= 1.0) {
    return std::nullopt;
  }
  const double recorded = recordedRevolutions * 360.0;
  return std::format(
      "The recording covered {:.0f} degrees of the ring. The other {:.0f} degrees were not "
      "recorded and are not calibrated. Widen the software position limits (0x607D) and calibrate "
      "again.",
      recorded, 360.0 - recorded);
}

std::span<const HrdEncoderSample> icMuCalibrationSamples(std::span<const HrdEncoderSample> samples,
                                                         std::chrono::milliseconds recording) {
  const auto expected = static_cast<size_t>(recording.count());
  if (samples.size() > expected && samples.size() < 2 * expected) {
    return samples.first(expected);
  }
  return samples;
}

}  // namespace mm::node::somanet
