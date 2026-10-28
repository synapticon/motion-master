#include "node/ic_mu_calibration.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

using mm::node::HrdEncoderSample;
using mm::node::somanet::EncoderShaft;
using mm::node::somanet::IcMuMotionInput;
using mm::node::somanet::IcMuNoniusOffsetTable;
using mm::node::somanet::IcMuPartClass;
using mm::node::somanet::IcMuPhase;
using mm::node::somanet::IcMuRegisterWrite;
using mm::node::somanet::IcMuRingPosition;
using mm::node::somanet::IcMuTrack;
using mm::node::somanet::IcMuTrackResidual;
namespace somanet = mm::node::somanet;
using std::chrono::milliseconds;

TEST(IcMuCalibrationTest, StartValuesOfTheMeasuredRings) {
  const auto inner32 =
      somanet::icMuStartValues(32, IcMuRingPosition::kInner, IcMuPartClass::kIcMu150Or200);
  ASSERT_TRUE(inner32);
  EXPECT_EQ(inner32->masterPhase, 0x00);
  EXPECT_EQ(inner32->noniusPhase, 0x00);
  EXPECT_EQ(inner32->motorShaftRingVelocityRpm, 90);
  EXPECT_TRUE(inner32->measured);

  const auto outer64 =
      somanet::icMuStartValues(64, IcMuRingPosition::kOuter, IcMuPartClass::kIcMu150Or200);
  ASSERT_TRUE(outer64);
  EXPECT_EQ(outer64->masterPhase, 0x21);
  EXPECT_EQ(outer64->noniusPhase, 0x1A);
  EXPECT_EQ(outer64->motorShaftRingVelocityRpm, 45);
  EXPECT_EQ(outer64->motorShaftLowVelocityRpm, 50);

  const auto inner64 =
      somanet::icMuStartValues(64, IcMuRingPosition::kInner, IcMuPartClass::kIcMu150Or200);
  ASSERT_TRUE(inner64);
  EXPECT_EQ(inner64->masterPhase, 0x4D);
  EXPECT_EQ(inner64->noniusPhase, 0x48);
}

TEST(IcMuCalibrationTest, UnmeasuredRowsAreMarkedAndOthersAreMissing) {
  const auto inner16 =
      somanet::icMuStartValues(16, IcMuRingPosition::kInner, IcMuPartClass::kIcMu150Or200);
  ASSERT_TRUE(inner16);
  EXPECT_FALSE(inner16->measured);
  const auto mu128 =
      somanet::icMuStartValues(32, IcMuRingPosition::kInner, IcMuPartClass::kIcMu128);
  ASSERT_TRUE(mu128);
  EXPECT_FALSE(mu128->measured);

  EXPECT_FALSE(
      somanet::icMuStartValues(16, IcMuRingPosition::kOuter, IcMuPartClass::kIcMu150Or200));
  EXPECT_FALSE(somanet::icMuStartValues(64, IcMuRingPosition::kInner, IcMuPartClass::kIcMu128));
  EXPECT_FALSE(somanet::icMuStartValues(32, IcMuRingPosition::kOuter, IcMuPartClass::kIcMu128));
}

TEST(IcMuCalibrationTest, RingPositionAndPartClass) {
  EXPECT_EQ(somanet::icMuRingPositionOfPort(1), IcMuRingPosition::kInner);
  EXPECT_EQ(somanet::icMuRingPositionOfPort(2), IcMuRingPosition::kOuter);
  EXPECT_EQ(somanet::icMuRingPositionOfPort(3), std::nullopt);
  EXPECT_EQ(somanet::parseIcMuRingPosition("outer"), IcMuRingPosition::kOuter);
  EXPECT_EQ(somanet::parseIcMuRingPosition("Outer"), std::nullopt);

  EXPECT_EQ(somanet::icMuPartClass(0x0A), IcMuPartClass::kIcMu128);
  EXPECT_EQ(somanet::icMuPartClass(0x11), IcMuPartClass::kIcMu150Or200);
  EXPECT_EQ(somanet::icMuPartClass(0x24), IcMuPartClass::kIcMu150Or200);
  EXPECT_EQ(somanet::icMuPartClass(0x30), std::nullopt);
}

TEST(IcMuCalibrationTest, PhaseRegisterHoldsTheRangeInBitSeven) {
  EXPECT_EQ(somanet::icMuPhaseRegister({.phase = 0x4D, .phaseRange = 0}), 0x4D);
  EXPECT_EQ(somanet::icMuPhaseRegister({.phase = 0x05, .phaseRange = 1}), 0x85);
  EXPECT_EQ(somanet::icMuPhaseFromRegister(0x85), (IcMuPhase{.phase = 0x05, .phaseRange = 1}));
  EXPECT_EQ(somanet::icMuPhaseFromRegister(0x7F), (IcMuPhase{.phase = 0x7F, .phaseRange = 0}));
}

TEST(IcMuCalibrationTest, NoniusTablePacksTwoOffsetsPerRegister) {
  IcMuNoniusOffsetTable table;
  table.base = 0x3;
  for (size_t i = 0; i < table.offsets.size(); ++i) {
    table.offsets[i] = static_cast<uint8_t>(i);
  }
  const std::vector<IcMuRegisterWrite> expected = {
      {0x52, 0x03}, {0x53, 0x21}, {0x54, 0x43}, {0x55, 0x65},
      {0x56, 0x87}, {0x57, 0xA9}, {0x58, 0xCB}, {0x59, 0xED},
  };
  const auto writes = somanet::icMuNoniusTableWrites(table);
  EXPECT_EQ(writes, expected);

  std::array<uint8_t, 8> registers{};
  for (size_t i = 0; i < registers.size(); ++i) {
    registers[i] = writes[i].value;
  }
  EXPECT_EQ(somanet::icMuNoniusTableFromRegisters(registers), table);
}

TEST(IcMuCalibrationTest, ToleranceWidensAsTheRunGoesOn) {
  EXPECT_EQ(somanet::icMuTolerance(1), 1);
  EXPECT_EQ(somanet::icMuTolerance(5), 1);
  EXPECT_EQ(somanet::icMuTolerance(6), 3);
  EXPECT_EQ(somanet::icMuTolerance(9), 3);
  EXPECT_EQ(somanet::icMuTolerance(10), 5);
  EXPECT_EQ(somanet::icMuTolerance(13), 5);
}

TEST(IcMuCalibrationTest, PassTestRoundsToTheNearestLsb) {
  IcMuTrackResidual residual{.sineOffset = 1.4, .cosineOffset = -1.4, .gain = 0.6, .phase = 0.9};
  EXPECT_TRUE(somanet::icMuTrackPasses(IcMuTrack::kMaster, residual, 1));
  residual.sineOffset = 1.6;
  EXPECT_FALSE(somanet::icMuTrackPasses(IcMuTrack::kMaster, residual, 1));
  EXPECT_TRUE(somanet::icMuTrackPasses(IcMuTrack::kMaster, residual, 3));
}

TEST(IcMuCalibrationTest, PassTestScalesThePhaseByTrackAndRange) {
  // Master: a step of 1 in range 0 and 2 in range 1.
  IcMuTrackResidual master{.phase = 2.9, .phaseRange = 1};
  EXPECT_TRUE(somanet::icMuTrackPasses(IcMuTrack::kMaster, master, 1));
  master.phaseRange = 0;
  EXPECT_FALSE(somanet::icMuTrackPasses(IcMuTrack::kMaster, master, 1));

  // Nonius: a step of 3 in range 0 and 5 in range 1.
  IcMuTrackResidual nonius{.phase = 4.4, .phaseRange = 0};
  EXPECT_TRUE(somanet::icMuTrackPasses(IcMuTrack::kNonius, nonius, 1));
  nonius.phase = 7.4;
  EXPECT_FALSE(somanet::icMuTrackPasses(IcMuTrack::kNonius, nonius, 1));
  nonius.phaseRange = 1;
  EXPECT_TRUE(somanet::icMuTrackPasses(IcMuTrack::kNonius, nonius, 1));

  nonius.phaseRange = 2;
  EXPECT_FALSE(somanet::icMuTrackPasses(IcMuTrack::kNonius, nonius, 5));
}

TEST(IcMuCalibrationTest, MoveAndVelocityAreInverse) {
  // 90 rpm at 1000 rpm/s for 6 s: 0.135 revolutions of ramps and 9 at constant velocity.
  EXPECT_NEAR(somanet::icMuMoveRevolutions(90, 1000, milliseconds(6000)), 9.135, 1e-9);
  EXPECT_NEAR(somanet::icMuVelocityForMove(9.135, 1000, milliseconds(6000)), 90, 1e-9);
}

IcMuMotionInput motorShaftInput() {
  return {.gearRatio = 1,
          .maxMotorSpeedRpm = 3000,
          .shaft = EncoderShaft::kMotor,
          .ringVelocityRpm = 90,
          .lowVelocityRpm = 70,
          .positionRevolutions = 0,
          .limitRevolutions = std::nullopt};
}

TEST(IcMuCalibrationTest, MotionWithoutLimitsRunsAtTheBaseVelocity) {
  auto plan = somanet::planIcMuMotion(motorShaftInput());
  ASSERT_TRUE(plan) << plan.error();
  EXPECT_EQ(plan->velocityRpm, 90);
  EXPECT_EQ(plan->accelerationRpmPerSecond, 1000);
  EXPECT_EQ(plan->recording, milliseconds(6000));
  EXPECT_EQ(plan->startRevolutions, std::nullopt);
  EXPECT_FALSE(plan->limitedRange);
  EXPECT_TRUE(plan->warnings.empty());

  // Behind a gearbox, the velocity and the acceleration are on the driving shaft.
  auto input = motorShaftInput();
  input.gearRatio = 120;
  plan = somanet::planIcMuMotion(input);
  ASSERT_TRUE(plan) << plan.error();
  EXPECT_NEAR(plan->velocityRpm, 0.75, 1e-12);
  EXPECT_NEAR(plan->accelerationRpmPerSecond, 1000.0 / 120, 1e-12);
}

TEST(IcMuCalibrationTest, MotionIsCappedByTheMaximumMotorSpeed) {
  auto input = motorShaftInput();
  input.maxMotorSpeedRpm = 50;
  auto plan = somanet::planIcMuMotion(input);
  ASSERT_TRUE(plan) << plan.error();
  EXPECT_NEAR(plan->velocityRpm, 47.5, 1e-12);
  ASSERT_EQ(plan->warnings.size(), 1u);
}

TEST(IcMuCalibrationTest, DefaultRingVelocityDependsOnTheShaft) {
  const auto values =
      somanet::icMuStartValues(64, IcMuRingPosition::kInner, IcMuPartClass::kIcMu150Or200);
  ASSERT_TRUE(values);
  EXPECT_EQ(somanet::icMuDefaultRingVelocity(*values, EncoderShaft::kMotor), 45);
  EXPECT_EQ(somanet::icMuDefaultRingVelocity(*values, EncoderShaft::kDriving), 15);
}

TEST(IcMuCalibrationTest, RequestedRingVelocityAppliesOnTheDrivingShaft) {
  IcMuMotionInput input{.gearRatio = 120,
                        .maxMotorSpeedRpm = 3000,
                        .shaft = EncoderShaft::kDriving,
                        .ringVelocityRpm = 20,
                        .lowVelocityRpm = 1.1,
                        .positionRevolutions = 0,
                        .limitRevolutions = std::nullopt};
  auto plan = somanet::planIcMuMotion(input);
  ASSERT_TRUE(plan) << plan.error();
  EXPECT_EQ(plan->velocityRpm, 20);
}

TEST(IcMuCalibrationTest, DrivingShaftRunsAt15RpmAndNeedsSpeed) {
  IcMuMotionInput input{.gearRatio = 120,
                        .maxMotorSpeedRpm = 3000,
                        .shaft = EncoderShaft::kDriving,
                        .ringVelocityRpm = 15,
                        .lowVelocityRpm = 1.1,
                        .positionRevolutions = 0,
                        .limitRevolutions = std::nullopt};
  auto plan = somanet::planIcMuMotion(input);
  ASSERT_TRUE(plan) << plan.error();
  EXPECT_EQ(plan->velocityRpm, 15);

  input.maxMotorSpeedRpm = 1200;  // 9.5 rpm on the driving shaft
  EXPECT_FALSE(somanet::planIcMuMotion(input));
}

TEST(IcMuCalibrationTest, MotionThatFitsBetweenTheLimitsStartsLowEnough) {
  auto input = motorShaftInput();
  input.limitRevolutions = std::pair{-20.0, 20.0};
  input.positionRevolutions = 0;
  auto plan = somanet::planIcMuMotion(input);
  ASSERT_TRUE(plan) << plan.error();
  EXPECT_EQ(plan->startRevolutions, std::nullopt);

  input.positionRevolutions = 15;
  plan = somanet::planIcMuMotion(input);
  ASSERT_TRUE(plan) << plan.error();
  ASSERT_TRUE(plan->startRevolutions);
  EXPECT_NEAR(*plan->startRevolutions, 20 - (3.0 / 360) - 9.135, 1e-9);
  EXPECT_FALSE(plan->limitedRange);
}

TEST(IcMuCalibrationTest, NarrowRangePlansAFullTurnFromTheLowerLimit) {
  auto input = motorShaftInput();
  input.limitRevolutions = std::pair{0.0, 2.0};
  input.positionRevolutions = 1.0;
  auto plan = somanet::planIcMuMotion(input);
  ASSERT_TRUE(plan) << plan.error();
  EXPECT_TRUE(plan->limitedRange);
  EXPECT_EQ(plan->accelerationRpmPerSecond, 4000);
  ASSERT_TRUE(plan->startRevolutions);
  EXPECT_NEAR(*plan->startRevolutions, 3.0 / 360, 1e-12);
  const double seconds = std::chrono::duration<double>(plan->recording).count();
  EXPECT_GE(plan->velocityRpm * seconds / 60, 1.0 + (5.0 / 360));
  // 2 revolutions leave 0.33 rpm of ring velocity at most, far below 70.
  EXPECT_FALSE(plan->warnings.empty());
}

TEST(IcMuCalibrationTest, NarrowRangeRefusesWithTheDegreesToWiden) {
  // Gear 120, an encoder on the driving shaft, and limits of ±185°: the bench case of the
  // previous calibration, which refused with 8 degrees.
  IcMuMotionInput input{.gearRatio = 120,
                        .maxMotorSpeedRpm = 3000,
                        .shaft = EncoderShaft::kDriving,
                        .ringVelocityRpm = 15,
                        .lowVelocityRpm = 1.1,
                        .positionRevolutions = 0,
                        .limitRevolutions = std::pair{-185.0 / 360, 185.0 / 360}};
  auto plan = somanet::planIcMuMotion(input);
  ASSERT_FALSE(plan);
  EXPECT_NE(plan.error().find("at least 8 degrees"), std::string::npos) << plan.error();

  // With 10 more degrees the turn fits into a 10 s recording.
  input.limitRevolutions = std::pair{-190.0 / 360, 190.0 / 360};
  plan = somanet::planIcMuMotion(input);
  ASSERT_TRUE(plan) << plan.error();
  EXPECT_EQ(plan->recording, milliseconds(10000));
}

TEST(IcMuCalibrationTest, NarrowRangeBelowTwentyDegreesIsRefused) {
  auto input = motorShaftInput();
  input.limitRevolutions = std::pair{0.0, 19.0 / 360};
  EXPECT_FALSE(somanet::planIcMuMotion(input));
}

std::vector<HrdEncoderSample> ramp(uint8_t masterPolePairs, double revolutions, size_t count) {
  std::vector<HrdEncoderSample> samples(count);
  const double counts = revolutions * masterPolePairs * 16384.0;
  for (size_t i = 0; i < count; ++i) {
    const auto position =
        static_cast<int64_t>(counts * static_cast<double>(i) / static_cast<double>(count - 1));
    samples[i].masterCount = static_cast<uint16_t>(position % 16384);
  }
  return samples;
}

TEST(IcMuCalibrationTest, RecordedRingRevolutionsUnwrapThePeriods) {
  EXPECT_NEAR(somanet::icMuRecordedRingRevolutions(ramp(32, 1.2, 6000), 32), 1.2, 1e-4);
  EXPECT_NEAR(somanet::icMuRecordedRingRevolutions(ramp(16, 0.95, 6000), 16), 0.95, 1e-4);
  EXPECT_EQ(somanet::icMuRecordedRingRevolutions({}, 32), 0);
}

TEST(IcMuCalibrationTest, CoverageWarningNamesTheMissedAngle) {
  EXPECT_EQ(somanet::icMuCoverageWarning(1.0), std::nullopt);
  const auto warning = somanet::icMuCoverageWarning(0.95);
  ASSERT_TRUE(warning);
  EXPECT_NE(warning->find("covered 342 degrees"), std::string::npos) << *warning;
  EXPECT_NE(warning->find("other 18 degrees"), std::string::npos) << *warning;
}

TEST(IcMuCalibrationTest, CalibrationSamplesCutThePaddingOfAOneKilohertzRecording) {
  const std::vector<HrdEncoderSample> padded(8032);
  EXPECT_EQ(somanet::icMuCalibrationSamples(padded, milliseconds(6000)).size(), 6000u);
  const std::vector<HrdEncoderSample> fast(96000);
  EXPECT_EQ(somanet::icMuCalibrationSamples(fast, milliseconds(6000)).size(), 96000u);
  const std::vector<HrdEncoderSample> short_(5000);
  EXPECT_EQ(somanet::icMuCalibrationSamples(short_, milliseconds(6000)).size(), 5000u);
}

TEST(IcMuCalibrationTest, AnalogRegistersOfBothTracks) {
  using somanet::IcMuAnalogAdjustments;
  const IcMuAnalogAdjustments nonius{.gain = 0x11,
                                     .sineOffset = 0x22,
                                     .cosineOffset = 0x33,
                                     .phase = {.phase = 5, .phaseRange = 1}};
  const std::vector<IcMuRegisterWrite> expected = {
      {0x08, 0x22}, {0x09, 0x33}, {0x07, 0x11}, {0x0A, 0x85}};
  EXPECT_EQ(somanet::icMuAnalogWrites(IcMuTrack::kNonius, nonius), expected);
  EXPECT_EQ(somanet::icMuAnalogRegisters(IcMuTrack::kMaster), (std::array<uint8_t, 4>{1, 2, 3, 4}));
  const std::array<uint8_t, 4> values = {0x11, 0x22, 0x33, 0x85};
  EXPECT_EQ(somanet::icMuAnalogFromRegisters(values), nonius);
}

TEST(IcMuCalibrationTest, StartWritesOfTheOuterRingOfACirculo9) {
  const auto values =
      somanet::icMuStartValues(64, IcMuRingPosition::kOuter, IcMuPartClass::kIcMu150Or200);
  ASSERT_TRUE(values);
  // The order of the configuration in use today: the phases, the offsets, the gains, the table.
  const std::vector<IcMuRegisterWrite> expected = {
      {0x04, 0x21}, {0x0A, 0x1A}, {0x02, 0x00}, {0x08, 0x00}, {0x03, 0x00}, {0x09, 0x00},
      {0x01, 0x00}, {0x07, 0x00}, {0x52, 0x00}, {0x53, 0x00}, {0x54, 0x00}, {0x55, 0x00},
      {0x56, 0x00}, {0x57, 0x00}, {0x58, 0x00}, {0x59, 0x00},
  };
  EXPECT_EQ(somanet::icMuStartWrites(*values), expected);
}

}  // namespace
