#include "node/controller_tuning.h"

#include <cmath>
#include <format>
#include <numbers>
#include <numeric>
#include <semver.hpp>
#include <string>
#include <vector>

#include "node/somanet_drive_objects.h"

namespace mm::node {

namespace objects = somanet::objects;

void to_json(nlohmann::json& j, const TuningDriveConfiguration& configuration) {
  j = nlohmann::json{
      {"vel_lp_type", configuration.velocityFilterType},
      {"vel_lp_fc", configuration.velocityFilterCutoffHz},
      {"pos_lp_type", configuration.positionFilterType},
      {"pos_lp_fc", configuration.positionFilterCutoffHz},
      {"notch_en", configuration.notchEnabled},
      {"notch_fc", configuration.notchCenterHz},
      {"notch_fw", configuration.notchWidthHz},
      {"vel_ff_gain", configuration.velocityFeedForwardPermil},
      {"vel_ff_fc", configuration.velocityFeedForwardCutoffHz},
      {"tc_st", configuration.torqueSettlingTimeUs},
      {"tc_damping", configuration.torqueDampingRatioPermil},
      {"software_version", configuration.firmwareVersion},
      {"rated_torque", configuration.ratedTorqueMilliNm},
      {"max_vel_noise", configuration.velocityNoiseRpm},
  };
  // Without the object the program keeps its own default depth.
  if (configuration.notchDepthDb) {
    j["notch_dp"] = *configuration.notchDepthDb;
  }
}

std::optional<std::string> controllerTuningFirmwareError(std::string_view version) {
  std::string_view number = version;
  if (number.starts_with('v')) {
    number.remove_prefix(1);
  }
  semver::version<> parsed;
  if (!semver::parse(number, parsed)) {
    return std::format("the firmware version '{}' could not be read", version);
  }
  semver::version<> minimum;
  semver::parse("5.1.0", minimum);
  if (parsed < minimum) {
    return std::format("controller auto-tuning needs firmware 5.1.0 or newer, and the drive has {}",
                       version);
  }
  return std::nullopt;
}

VelocityNoise velocityNoise(std::span<const HrdSystemIdentificationSample> samples) {
  if (samples.size() < 3) {
    return {.noiseRpm = 0.0,
            .warning = std::format(
                "the noise recording has {} samples, too few to measure the noise; the gains are "
                "computed with a noise of 0",
                samples.size())};
  }
  const double absoluteSum = std::accumulate(
      samples.begin(), samples.end(), 0.0,
      [](double sum, const auto& sample) { return sum + std::abs(sample.velocityRpm); });
  const double meanAbsolute = absoluteSum / static_cast<double>(samples.size());
  if (meanAbsolute > kNoiseMaxMeanAbsVelocityRpm) {
    return {.noiseRpm = 0.0,
            .warning = std::format(
                "the motor moved during the noise recording (mean absolute velocity {:.2f} rpm, "
                "more than {} rpm); the gains are computed with a noise of 0",
                meanAbsolute, kNoiseMaxMeanAbsVelocityRpm)};
  }
  std::vector<double> changes;
  changes.reserve(samples.size() - 1);
  for (size_t i = 1; i < samples.size(); ++i) {
    changes.push_back(samples[i].velocityRpm - samples[i - 1].velocityRpm);
  }
  const double mean =
      std::accumulate(changes.begin(), changes.end(), 0.0) / static_cast<double>(changes.size());
  const double squares = std::accumulate(
      changes.begin(), changes.end(), 0.0,
      [mean](double sum, double change) { return sum + (change - mean) * (change - mean); });
  return {.noiseRpm = 3.0 * std::sqrt(squares / static_cast<double>(changes.size() - 1)),
          .warning = std::nullopt};
}

std::expected<GainPlan, std::string> controllerGainPlan(
    TunedController controller, const TunedGains& gains, uint32_t positionEncoderResolution,
    const std::function<bool(ObjectAddress<float>)>& hasObject) {
  struct Gain {
    ObjectAddress<float> address;
    double value = 0.0;
    bool required = false;
  };
  std::vector<Gain> planned;
  const double velocityScale = 1000.0 * 2.0 * std::numbers::pi / 60.0;
  if (controller == TunedController::kPosition) {
    if (positionEncoderResolution == 0) {
      return std::unexpected(
          "the resolution of the position encoder is 0, so the position loop gains cannot be "
          "converted");
    }
    const double positionScale = 60.0 / static_cast<double>(positionEncoderResolution);
    planned = {
        {objects::kPositionControllerPositionLoopKp, gains.position.kp * positionScale, true},
        {objects::kPositionControllerPositionLoopKi, gains.position.ki * positionScale, true},
        {objects::kPositionControllerPositionLoopKd, 0.0, false},
        {objects::kPositionControllerVelocityLoopKp, gains.velocity.kp * velocityScale, true},
        {objects::kPositionControllerVelocityLoopKi, gains.velocity.ki * velocityScale, true},
        {objects::kPositionControllerVelocityLoopKd, 0.0, false},
    };
  } else {
    planned = {
        {objects::kVelocityControllerControllerKp, gains.velocity.kp * velocityScale, true},
        {objects::kVelocityControllerControllerKi, gains.velocity.ki * velocityScale, true},
        {objects::kVelocityControllerControllerKd, 0.0, false},
    };
  }
  GainPlan plan;
  for (const auto& gain : planned) {
    if (!hasObject(gain.address)) {
      if (gain.required) {
        return std::unexpected(std::format(
            "the drive has no object 0x{:04X}:{:02X}, so the {} controller gains cannot be written",
            gain.address.index, gain.address.subindex, toString(controller)));
      }
      plan.skipped.push_back(gain.address);
      continue;
    }
    plan.writes.push_back({.address = gain.address, .value = static_cast<float>(gain.value)});
  }
  return plan;
}

}  // namespace mm::node
