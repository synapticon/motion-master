#include "monitoring_api.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "core/util.h"

namespace mm {

std::optional<WsCommand> parseWsCommand(std::string_view message) {
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(message);
  } catch (const nlohmann::json::exception&) {
    return std::nullopt;
  }
  if (!j.is_object()) {
    return std::nullopt;
  }

  const auto stringField = [&](const char* key) -> std::optional<std::string> {
    auto it = j.find(key);
    if (it != j.end() && it->is_string()) {
      return it->get<std::string>();
    }
    return std::nullopt;
  };

  if (auto topic = stringField("subscribe")) {
    if (mm::core::isUrlSafeId(*topic)) {
      return WsCommand{WsCommand::Action::Subscribe, std::move(*topic)};
    }
  } else if (auto unsub = stringField("unsubscribe")) {
    if (mm::core::isUrlSafeId(*unsub)) {
      return WsCommand{WsCommand::Action::Unsubscribe, std::move(*unsub)};
    }
  }
  return std::nullopt;
}

std::expected<mm::node::Monitoring, std::string> parseMonitoringRequest(
    const nlohmann::json& body) {
  if (!body.is_object()) {
    return std::unexpected("request body must be a JSON object");
  }

  mm::node::Monitoring m;

  auto topic = body.find("topic");
  if (topic == body.end() || !topic->is_string()) {
    return std::unexpected("topic is required and must be a string");
  }
  m.topic = topic->get<std::string>();

  if (auto name = body.find("name"); name != body.end() && !name->is_null()) {
    if (!name->is_string()) {
      return std::unexpected("name must be a string");
    }
    m.name = name->get<std::string>();
  }

  auto interval = body.find("interval");
  if (interval == body.end() || !interval->is_number_integer()) {
    return std::unexpected("interval is required and must be an integer (milliseconds)");
  }
  m.interval = std::chrono::milliseconds{interval->get<int64_t>()};
  // bufferSize was removed: the stream is lossless and interval is the flush cadence, so there is
  // no batch-size knob. Any bufferSize an older client sends is ignored (validated server-side).

  auto parameters = body.find("parameters");
  if (parameters == body.end() || !parameters->is_array()) {
    return std::unexpected("parameters is required and must be an array");
  }
  for (const auto& p : *parameters) {
    if (!p.is_array() || p.size() != 3 || !p[0].is_number_unsigned() ||
        !p[1].is_number_unsigned() || !p[2].is_number_unsigned()) {
      return std::unexpected("each parameter must be [devicePosition, index, subindex] integers");
    }
    const auto position = p[0].get<int64_t>();
    const auto index = p[1].get<int64_t>();
    const auto subindex = p[2].get<int64_t>();
    if (position > 0xFFFF || index > 0xFFFF || subindex > 0xFF) {
      return std::unexpected(
          "parameter out of range (devicePosition/index 16-bit, subindex 8-bit)");
    }
    m.parameters.push_back({static_cast<uint16_t>(position), static_cast<uint16_t>(index),
                            static_cast<uint8_t>(subindex)});
  }

  return m;
}

}  // namespace mm
