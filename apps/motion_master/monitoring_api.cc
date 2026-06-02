#include "monitoring_api.h"

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

}  // namespace mm
