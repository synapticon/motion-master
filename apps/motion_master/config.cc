#include "config.h"

#include <algorithm>
#include <array>
#include <string>

std::expected<Config, std::string> parseConfig(const nlohmann::json& doc) {
  if (!doc.is_object()) {
    return std::unexpected("top-level config must be a JSON object");
  }

  Config config;
  try {
    config = doc.get<Config>();
  } catch (const nlohmann::json::exception& e) {
    // nlohmann reports field type mismatches via exceptions; convert to our error channel.
    return std::unexpected(e.what());
  }

  static constexpr std::array kLevels{"trace", "debug", "info", "warn", "error"};
  if (std::find(kLevels.begin(), kLevels.end(), config.logLevel) == kLevels.end()) {
    return std::unexpected("logLevel must be one of: trace, debug, info, warn, error");
  }

  if (!config.fieldbus.driver.empty()) {
    static constexpr std::array kDrivers{"soem", "spoe"};
    if (std::find(kDrivers.begin(), kDrivers.end(), config.fieldbus.driver) == kDrivers.end()) {
      return std::unexpected("fieldbus.driver must be one of: soem, spoe");
    }
  }

  if (config.gameLoop.periodUs == 0) {
    return std::unexpected("gameLoop.periodUs must be greater than 0");
  }

  if (config.recorder.historySeconds == 0) {
    return std::unexpected("recorder.historySeconds must be greater than 0");
  }

  return config;
}
