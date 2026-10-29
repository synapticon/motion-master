#include "node/controller_tuning.h"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "node/somanet_drive_objects.h"

namespace mm::node {
namespace {

namespace objects = somanet::objects;

using Address = std::pair<uint16_t, uint8_t>;

Address key(ObjectAddress<float> address) { return {address.index, address.subindex}; }

std::function<bool(ObjectAddress<float>)> dictionaryWithout(std::set<Address> missing) {
  return [missing = std::move(missing)](ObjectAddress<float> address) {
    return !missing.contains(key(address));
  };
}

std::vector<HrdSystemIdentificationSample> velocities(const std::vector<double>& rpm) {
  std::vector<HrdSystemIdentificationSample> samples;
  samples.reserve(rpm.size());
  for (double value : rpm) {
    samples.push_back({.velocityRpm = value, .torquePermil = 0});
  }
  return samples;
}

TEST(TuningDriveConfiguration, RendersTheProgramsFieldNames) {
  TuningDriveConfiguration configuration{
      .velocityFilterType = 2,
      .velocityFilterCutoffHz = 300,
      .positionFilterType = 1,
      .positionFilterCutoffHz = 400,
      .notchEnabled = true,
      .notchCenterHz = 1000,
      .notchWidthHz = 50,
      .notchDepthDb = 20,
      .velocityFeedForwardPermil = -100,
      .velocityFeedForwardCutoffHz = 900,
      .torqueSettlingTimeUs = 800,
      .torqueDampingRatioPermil = 1500,
      .firmwareVersion = "v5.6.11",
      .ratedTorqueMilliNm = 2100,
      .velocityNoiseRpm = 0.25,
  };
  const nlohmann::json j = configuration;
  EXPECT_EQ(j.at("vel_lp_type"), 2);
  EXPECT_EQ(j.at("vel_lp_fc"), 300);
  EXPECT_EQ(j.at("pos_lp_type"), 1);
  EXPECT_EQ(j.at("pos_lp_fc"), 400);
  EXPECT_EQ(j.at("notch_en"), true);
  EXPECT_EQ(j.at("notch_fc"), 1000);
  EXPECT_EQ(j.at("notch_fw"), 50);
  EXPECT_EQ(j.at("notch_dp"), 20);
  EXPECT_EQ(j.at("vel_ff_gain"), -100);
  EXPECT_EQ(j.at("vel_ff_fc"), 900);
  EXPECT_EQ(j.at("tc_st"), 800);
  EXPECT_EQ(j.at("tc_damping"), 1500);
  EXPECT_EQ(j.at("software_version"), "v5.6.11");
  EXPECT_EQ(j.at("rated_torque"), 2100);
  EXPECT_DOUBLE_EQ(j.at("max_vel_noise").get<double>(), 0.25);
  // The fields the program reads only for firmware 4 are left to its defaults.
  for (const char* field : {"L", "R", "vdc", "current_ratio", "switch_F", "tc_kp", "tc_ki"}) {
    EXPECT_FALSE(j.contains(field)) << field;
  }
}

TEST(TuningDriveConfiguration, LeavesTheNotchDepthToTheProgramWithoutTheObject) {
  const nlohmann::json j = TuningDriveConfiguration{};
  EXPECT_FALSE(j.contains("notch_dp"));
}

TEST(ControllerFirmwareError, AcceptsFirmwareFrom510) {
  EXPECT_FALSE(controllerFirmwareError("v5.1.0", "x"));
  EXPECT_FALSE(controllerFirmwareError("5.6.11", "x"));
  EXPECT_FALSE(controllerFirmwareError("v10.0.4", "x"));
}

TEST(ControllerFirmwareError, RefusesOlderAndUnreadableFirmware) {
  EXPECT_TRUE(controllerFirmwareError("v5.0.9", "x"));
  EXPECT_TRUE(controllerFirmwareError("v5.1.0-rc.1", "x"));
  EXPECT_TRUE(controllerFirmwareError("v4.4.1", "x"));
  EXPECT_TRUE(controllerFirmwareError("unknown", "x"));
}

IntegralLimitInputs limitInputs() {
  return {.maxTorquePermil = 3000,
          .ratedTorqueMilliNm = 400,
          .maxMotorSpeed = 2000,
          .feedConstantFeed = 1,
          .feedConstantShaftRevolutions = 1,
          .gearMotorRevolutions = 1,
          .gearShaftRevolutions = 1,
          .positionControlStrategy = 2};
}

TEST(IntegralLimits, TheVelocityLoopsStopAtTheMaximumTorque) {
  const auto limits = integralLimits(limitInputs());
  ASSERT_TRUE(limits) << limits.error();
  EXPECT_EQ(limits->velocityController, 1200u);  // 3000 per mille of 400 mNm
  EXPECT_EQ(limits->positionVelocityLoop, 1200u);
}

TEST(IntegralLimits, TheCascadedPositionLoopStopsAtTheMaximumSpeedOnTheDrivingShaft) {
  auto inputs = limitInputs();
  inputs.gearMotorRevolutions = 50;  // 50 motor turns for 3 turns of the shaft
  inputs.gearShaftRevolutions = 3;
  inputs.feedConstantFeed = 360000;  // 360000 units for one shaft turn
  inputs.feedConstantShaftRevolutions = 1;
  const auto limits = integralLimits(inputs);
  ASSERT_TRUE(limits) << limits.error();
  EXPECT_EQ(limits->positionLoop, 43200000u);  // 2000 × 360000 / (50 / 3)
}

TEST(IntegralLimits, AFeedOf0xFFFFFFFFTurnsTheFeedConstantOff) {
  // The bench Circulo: 3750000 mrpm, a 120:1 gear, the feed constant off.
  auto inputs = limitInputs();
  inputs.maxMotorSpeed = 3750000;
  inputs.gearMotorRevolutions = 120;
  inputs.feedConstantFeed = kFeedConstantOff;
  inputs.feedConstantShaftRevolutions = 0;  // not read while the feed constant is off
  const auto limits = integralLimits(inputs);
  ASSERT_TRUE(limits) << limits.error();
  EXPECT_EQ(limits->positionLoop, 31250u);
}

TEST(IntegralLimits, TheSimplePidPositionLoopStopsAtTheMaximumTorque) {
  auto inputs = limitInputs();
  inputs.positionControlStrategy = 1;
  inputs.gearMotorRevolutions = 50;
  const auto limits = integralLimits(inputs);
  ASSERT_TRUE(limits) << limits.error();
  EXPECT_EQ(limits->positionLoop, 1200u);
}

TEST(IntegralLimits, RoundsToAWholeNumber) {
  auto inputs = limitInputs();
  inputs.maxTorquePermil = 1001;    // 400.4 mNm
  inputs.gearMotorRevolutions = 3;  // 666.67
  const auto limits = integralLimits(inputs);
  ASSERT_TRUE(limits) << limits.error();
  EXPECT_EQ(limits->velocityController, 400u);
  EXPECT_EQ(limits->positionLoop, 667u);
}

TEST(IntegralLimits, RefusesAZeroInTheRatiosAndAnUnknownStrategy) {
  auto inputs = limitInputs();
  inputs.gearShaftRevolutions = 0;
  auto limits = integralLimits(inputs);
  ASSERT_FALSE(limits);
  EXPECT_NE(limits.error().find("0x6091:02"), std::string::npos) << limits.error();
  inputs = limitInputs();
  inputs.positionControlStrategy = 3;
  EXPECT_FALSE(integralLimits(inputs));
  inputs = limitInputs();
  inputs.feedConstantFeed = 1e12;
  EXPECT_FALSE(integralLimits(inputs));
}

TEST(VelocityNoise, IsThreeSampleStandardDeviationsOfTheChange) {
  // Changes: +1, -1, +1, -1. Mean 0, sum of squares 4, sample variance 4 / 3.
  const auto noise = velocityNoise(velocities({0.0, 1.0, 0.0, 1.0, 0.0}));
  EXPECT_FALSE(noise.warning);
  EXPECT_DOUBLE_EQ(noise.noiseRpm, 3.0 * std::sqrt(4.0 / 3.0));
}

TEST(VelocityNoise, AConstantVelocityHasNoNoise) {
  const auto noise = velocityNoise(velocities({2.0, 2.0, 2.0, 2.0}));
  EXPECT_FALSE(noise.warning);
  EXPECT_DOUBLE_EQ(noise.noiseRpm, 0.0);
}

TEST(VelocityNoise, AMovingMotorGivesZeroAndAWarning) {
  // The mean is absolute, so a motor turning backwards is caught too.
  const auto noise = velocityNoise(velocities({-5.0, -5.0, -5.0, -5.0}));
  ASSERT_TRUE(noise.warning);
  EXPECT_NE(noise.warning->find("moved"), std::string::npos) << *noise.warning;
  EXPECT_DOUBLE_EQ(noise.noiseRpm, 0.0);
}

TEST(VelocityNoise, TheThresholdItselfIsAtRest) {
  const auto noise = velocityNoise(velocities({4.0, -4.0, 4.0, -4.0}));
  EXPECT_FALSE(noise.warning);
}

TEST(VelocityNoise, TooFewSamplesGiveZeroAndAWarning) {
  const auto noise = velocityNoise(velocities({0.1, 0.2}));
  ASSERT_TRUE(noise.warning);
  EXPECT_DOUBLE_EQ(noise.noiseRpm, 0.0);
}

TEST(ControllerGainPlan, ConvertsAndOrdersThePositionGains) {
  const TunedGains gains{.position = {.kp = 100.0, .ki = 50.0, .kd = 7.0},
                         .velocity = {.kp = 0.6, .ki = 3.0, .kd = 9.0}};
  const auto plan =
      controllerGainPlan(TunedController::kPosition, gains, 1 << 19, dictionaryWithout({}));
  ASSERT_TRUE(plan) << plan.error();
  ASSERT_EQ(plan->writes.size(), 6u);
  EXPECT_TRUE(plan->skipped.empty());
  const double positionScale = 60.0 / (1 << 19);
  const double velocityScale = 2000.0 * std::numbers::pi / 60.0;
  const std::vector<std::pair<ObjectAddress<float>, double>> expected = {
      {objects::kPositionControllerPositionLoopKp, 100.0 * positionScale},
      {objects::kPositionControllerPositionLoopKi, 50.0 * positionScale},
      {objects::kPositionControllerPositionLoopKd, 0.0},
      {objects::kPositionControllerVelocityLoopKp, 0.6 * velocityScale},
      {objects::kPositionControllerVelocityLoopKi, 3.0 * velocityScale},
      {objects::kPositionControllerVelocityLoopKd, 0.0},
  };
  for (size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(key(plan->writes[i].address), key(expected[i].first)) << i;
    EXPECT_FLOAT_EQ(plan->writes[i].value, static_cast<float>(expected[i].second)) << i;
  }
}

TEST(ControllerGainPlan, ConvertsTheVelocityGains) {
  const TunedGains gains{.position = {}, .velocity = {.kp = 0.6, .ki = 3.0, .kd = 9.0}};
  // A velocity tuning does not read the resolution.
  const auto plan = controllerGainPlan(TunedController::kVelocity, gains, 0, dictionaryWithout({}));
  ASSERT_TRUE(plan) << plan.error();
  ASSERT_EQ(plan->writes.size(), 3u);
  const double velocityScale = 2000.0 * std::numbers::pi / 60.0;
  EXPECT_EQ(key(plan->writes[0].address), key(objects::kVelocityControllerControllerKp));
  EXPECT_FLOAT_EQ(plan->writes[0].value, static_cast<float>(0.6 * velocityScale));
  EXPECT_EQ(key(plan->writes[1].address), key(objects::kVelocityControllerControllerKi));
  EXPECT_FLOAT_EQ(plan->writes[1].value, static_cast<float>(3.0 * velocityScale));
  EXPECT_EQ(key(plan->writes[2].address), key(objects::kVelocityControllerControllerKd));
  EXPECT_FLOAT_EQ(plan->writes[2].value, 0.0F);
}

TEST(ControllerGainPlan, SkipsTheKdObjectsADictionaryLacks) {
  const auto missing = dictionaryWithout({key(objects::kPositionControllerPositionLoopKd),
                                          key(objects::kPositionControllerVelocityLoopKd)});
  const auto plan = controllerGainPlan(TunedController::kPosition, TunedGains{}, 1000, missing);
  ASSERT_TRUE(plan) << plan.error();
  ASSERT_EQ(plan->writes.size(), 4u);
  ASSERT_EQ(plan->skipped.size(), 2u);
  EXPECT_EQ(key(plan->skipped[0]), key(objects::kPositionControllerPositionLoopKd));
  EXPECT_EQ(key(plan->skipped[1]), key(objects::kPositionControllerVelocityLoopKd));
  EXPECT_EQ(key(plan->writes[2].address), key(objects::kPositionControllerVelocityLoopKp));
}

TEST(ControllerGainPlan, RefusesADictionaryWithoutAKiObject) {
  const auto plan =
      controllerGainPlan(TunedController::kVelocity, TunedGains{}, 0,
                         dictionaryWithout({key(objects::kVelocityControllerControllerKi)}));
  ASSERT_FALSE(plan);
  EXPECT_NE(plan.error().find("0x2011:02"), std::string::npos) << plan.error();
}

TEST(ControllerGainPlan, RefusesAPositionTuningWithoutAResolution) {
  const auto plan =
      controllerGainPlan(TunedController::kPosition, TunedGains{}, 0, dictionaryWithout({}));
  ASSERT_FALSE(plan);
  EXPECT_NE(plan.error().find("resolution"), std::string::npos) << plan.error();
}

}  // namespace
}  // namespace mm::node
