#include "controller_tuning_call.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "auto_tuning/client.h"
#include "node/controller_tuning.h"

namespace {

using mm::controllerTuningFunction;
using mm::controllerTuningRequest;
using mm::parseControllerTuningReply;
using mm::node::CascadedControllerType;
using mm::node::ControllerTuningCall;
using mm::node::TunedController;
using mm::node::TuningDriveConfiguration;

ControllerTuningCall call(TunedController controller, bool full) {
  ControllerTuningCall c{.controller = controller,
                         .controllerType = CascadedControllerType::kPPi,
                         .numerator = {1.0},
                         .denominator = {0.002, 0.03},
                         .driveConfiguration = std::nullopt,
                         .targets = {.dampingRatio = 0.7, .settlingTimeS = 0.15, .bandwidthHz = 8}};
  if (full) {
    TuningDriveConfiguration configuration;
    configuration.ratedTorqueMilliNm = 1500;
    c.driveConfiguration = configuration;
  }
  return c;
}

const char kGains[] =
    "\"gains\": {\"position\": {\"kp\": 30.0, \"ki\": 4.0, \"kd\": 0.0}, "
    "\"velocity\": {\"kp\": 0.05, \"ki\": 2.5, \"kd\": 0.0}}";

TEST(ControllerTuningFunction, PicksOneOfTheFourFunctions) {
  EXPECT_EQ(controllerTuningFunction(call(TunedController::kPosition, true)),
            "full_auto_tune_position_controller");
  EXPECT_EQ(controllerTuningFunction(call(TunedController::kPosition, false)),
            "auto_tune_position_controller");
  EXPECT_EQ(controllerTuningFunction(call(TunedController::kVelocity, true)),
            "full_auto_tune_velocity_controller");
  EXPECT_EQ(controllerTuningFunction(call(TunedController::kVelocity, false)),
            "auto_tune_velocity_controller");
}

TEST(ControllerTuningRequest, AFullPositionTuningSendsTheConfigurationAndTheStandardRule) {
  const auto request =
      nlohmann::json::parse(controllerTuningRequest(call(TunedController::kPosition, true)));
  const auto& data = request.at("data");
  EXPECT_EQ(data.at("plant_model").at("denominator"), nlohmann::json({0.002, 0.03}));
  EXPECT_EQ(data.at("controller_type"), "P-PI");
  EXPECT_EQ(data.at("highFriction"), 0);
  EXPECT_EQ(data.at("drive_config").at("rated_torque"), 1500);
  EXPECT_FALSE(data.contains("zeta"));
}

TEST(ControllerTuningRequest, ATuningToTargetsSendsTheTargetsInTheProgramsUnits) {
  const auto position =
      nlohmann::json::parse(controllerTuningRequest(call(TunedController::kPosition, false)));
  EXPECT_EQ(position.at("data").at("zeta"), 0.7);
  EXPECT_EQ(position.at("data").at("demanded_st"), 0.15);  // seconds
  EXPECT_FALSE(position.at("data").contains("drive_config"));

  const auto velocity =
      nlohmann::json::parse(controllerTuningRequest(call(TunedController::kVelocity, false)));
  EXPECT_EQ(velocity.at("data").at("demanded_bw"), 8);  // Hz
  EXPECT_FALSE(velocity.at("data").contains("controller_type"));
}

TEST(ParseControllerTuningReply, AFullTuningReportsWhatTheProgramChose) {
  const auto reply = parseControllerTuningReply(
      call(TunedController::kPosition, true), 200,
      std::string("{") + kGains +
          R"(, "demanded_st": 0.12, "demanded_zeta": 0.9, "state": {"gm_p": 3}})");
  ASSERT_TRUE(reply.has_value()) << reply.error();
  EXPECT_DOUBLE_EQ(reply->gains.position.kp, 30.0);
  EXPECT_DOUBLE_EQ(reply->gains.velocity.ki, 2.5);
  EXPECT_DOUBLE_EQ(reply->used.dampingRatio, 0.9);
  EXPECT_DOUBLE_EQ(reply->used.settlingTimeS, 0.12);
  EXPECT_EQ(reply->state.at("gm_p"), 3);
}

TEST(ParseControllerTuningReply, ATuningToTargetsReportsTheTargets) {
  const auto reply = parseControllerTuningReply(call(TunedController::kVelocity, false), 200,
                                                std::string("{") + kGains + "}");
  ASSERT_TRUE(reply.has_value()) << reply.error();
  EXPECT_DOUBLE_EQ(reply->used.dampingRatio, 0.7);
  EXPECT_DOUBLE_EQ(reply->used.bandwidthHz, 8.0);
  EXPECT_TRUE(reply->state.is_null());
}

TEST(ParseControllerTuningReply, ARefusalArrivesAsA200WithAnError) {
  const auto reply =
      parseControllerTuningReply(call(TunedController::kVelocity, true), 200,
                                 R"({"error": {"message": "Config loading error."}})");
  ASSERT_FALSE(reply.has_value());
  EXPECT_NE(reply.error().find("Config loading error."), std::string::npos) << reply.error();
}

TEST(ParseControllerTuningReply, RefusesAReplyWithoutGainsOrChoices) {
  EXPECT_FALSE(parseControllerTuningReply(call(TunedController::kVelocity, false), 200, "{}"));
  EXPECT_FALSE(parseControllerTuningReply(call(TunedController::kVelocity, true), 200,
                                          std::string("{") + kGains + "}"));
  EXPECT_FALSE(parseControllerTuningReply(call(TunedController::kVelocity, false), 500, "boom"));
}

TEST(MakeControllerTuning, WithoutAProgramEveryCallIsRefused) {
  const std::optional<mm::auto_tuning::Client> none;
  const auto tune = mm::makeControllerTuning(none);
  const auto reply = tune(call(TunedController::kVelocity, false));
  ASSERT_FALSE(reply.has_value());
  EXPECT_NE(reply.error().find("not running"), std::string::npos) << reply.error();
}

}  // namespace
