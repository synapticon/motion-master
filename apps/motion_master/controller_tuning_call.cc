#include "controller_tuning_call.h"

#include <chrono>
#include <format>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace mm {

namespace {

// A full tuning searches the bandwidth or the settling time, and each step evaluates the loop.
// Measured against version 3.1.1 it returns in well under a second. The limit only fires when the
// program stops answering.
constexpr std::chrono::seconds kTuningTimeout{60};

constexpr const char* kProgramNotRunning =
    "the auto-tuning program is not running; GET /api/auto-tuning says why";

bool isPosition(const node::ControllerTuningCall& call) {
  return call.controller == node::TunedController::kPosition;
}

std::expected<node::LoopGains, std::string> loopGains(const nlohmann::json& gains,
                                                      const char* loop) {
  const auto it = gains.find(loop);
  if (it == gains.end() || !it->is_object()) {
    return std::unexpected(std::format("the reply has no {} gains", loop));
  }
  node::LoopGains result;
  for (const auto& [name, field] :
       {std::pair{"kp", &result.kp}, std::pair{"ki", &result.ki}, std::pair{"kd", &result.kd}}) {
    const auto value = it->find(name);
    if (value == it->end() || !value->is_number()) {
      return std::unexpected(std::format("the reply has no number for the {} {}", loop, name));
    }
    *field = value->get<double>();
  }
  return result;
}

}  // namespace

std::string controllerTuningFunction(const node::ControllerTuningCall& call) {
  return std::format("{}auto_tune_{}_controller", call.driveConfiguration ? "full_" : "",
                     node::toString(call.controller));
}

std::string controllerTuningRequest(const node::ControllerTuningCall& call) {
  nlohmann::json data{
      {"plant_model", {{"numerator", call.numerator}, {"denominator", call.denominator}}}};
  if (isPosition(call)) {
    data["controller_type"] = node::toString(call.controllerType);
  }
  if (call.driveConfiguration) {
    data["drive_config"] = *call.driveConfiguration;
    if (isPosition(call)) {
      data["highFriction"] = 0;
    }
  } else {
    data["zeta"] = call.targets.dampingRatio;
    if (isPosition(call)) {
      data["demanded_st"] = call.targets.settlingTimeS;
    } else {
      data["demanded_bw"] = call.targets.bandwidthHz;
    }
  }
  return nlohmann::json{{"run", controllerTuningFunction(call)}, {"data", std::move(data)}}.dump();
}

std::expected<node::ControllerTuningReply, std::string> parseControllerTuningReply(
    const node::ControllerTuningCall& call, int status, const std::string& body) {
  if (status != 200) {
    return std::unexpected(
        std::format("the auto-tuning program answered {}: {}", status, body.substr(0, 200)));
  }
  const auto reply = nlohmann::json::parse(body, nullptr, false);
  if (reply.is_discarded() || !reply.is_object()) {
    return std::unexpected("the auto-tuning program answered with something other than JSON");
  }
  if (const auto error = reply.find("error"); error != reply.end()) {
    const auto message = error->is_object()   ? error->value("message", error->dump())
                         : error->is_string() ? error->get<std::string>()
                                              : error->dump();
    return std::unexpected(std::format("the auto-tuning program refused: {}", message));
  }
  const auto gains = reply.find("gains");
  if (gains == reply.end() || !gains->is_object()) {
    return std::unexpected("the auto-tuning program answered without gains");
  }
  auto position = loopGains(*gains, "position");
  auto velocity = loopGains(*gains, "velocity");
  if (!position || !velocity) {
    return std::unexpected(std::format("the auto-tuning program answered with incomplete gains: {}",
                                       !position ? position.error() : velocity.error()));
  }
  node::ControllerTuningReply result{.gains = {.position = *position, .velocity = *velocity},
                                     .used = call.targets,
                                     .state = reply.value("state", nlohmann::json())};
  if (call.driveConfiguration) {
    const char* target = isPosition(call) ? "demanded_st" : "demanded_bw";
    const auto zeta = reply.find("demanded_zeta");
    const auto chosen = reply.find(target);
    if (zeta == reply.end() || !zeta->is_number() || chosen == reply.end() ||
        !chosen->is_number()) {
      return std::unexpected(
          std::format("the auto-tuning program answered without demanded_zeta and {}", target));
    }
    result.used.dampingRatio = zeta->get<double>();
    if (isPosition(call)) {
      result.used.settlingTimeS = chosen->get<double>();
    } else {
      result.used.bandwidthHz = chosen->get<double>();
    }
  }
  return result;
}

node::ControllerTuningFn makeControllerTuning(const std::optional<auto_tuning::Client>& client) {
  return [&client](const node::ControllerTuningCall& call)
             -> std::expected<node::ControllerTuningReply, std::string> {
    if (!client) {
      return std::unexpected(kProgramNotRunning);
    }
    auto reply = client->run(controllerTuningRequest(call), kTuningTimeout);
    if (!reply) {
      return std::unexpected(reply.error());
    }
    return parseControllerTuningReply(call, reply->status, reply->body);
  };
}

}  // namespace mm
