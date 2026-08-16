#include "config.h"

#include <algorithm>
#include <array>
#include <string>
#include <thread>

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

  // uWebSockets reads an empty host as "every interface", so an omitted-but-present bindAddress
  // would silently expose an unauthenticated server to the network. Refuse it: "0.0.0.0" is how you
  // ask for that, explicitly.
  if (config.server.bindAddress.empty()) {
    return std::unexpected(
        "server.bindAddress must not be empty (use \"0.0.0.0\" to bind all "
        "interfaces)");
  }

  static constexpr std::array kLevels{"trace", "debug",    "info", "warning",
                                      "error", "critical", "off"};
  if (std::find(kLevels.begin(), kLevels.end(), config.logging.level) == kLevels.end()) {
    return std::unexpected(
        "logging.level must be one of: trace, debug, info, warning, error, critical, off");
  }
  if (std::find(kLevels.begin(), kLevels.end(), config.logging.file.level) == kLevels.end()) {
    return std::unexpected(
        "logging.file.level must be one of: trace, debug, info, warning, error, critical, off");
  }
  // Both must be positive or the rotation is meaningless: a zero size would rotate on every line,
  // and zero files leaves nowhere to rotate to.
  if (config.logging.file.maxSizeMb == 0) {
    return std::unexpected("logging.file.maxSizeMb must be greater than 0");
  }
  if (config.logging.file.maxFiles == 0) {
    return std::unexpected("logging.file.maxFiles must be greater than 0");
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

  // Naming a core that does not exist would otherwise fail silently at startup (sched_setaffinity
  // returns EINVAL and the thread simply stays unpinned), which is exactly the kind of typo that
  // costs an afternoon on a board you cannot see. hardware_concurrency() counts online CPUs, so
  // isolated cores are included; 0 means "unknown", in which case there is nothing to check
  // against.
  if (const unsigned int cpus = std::thread::hardware_concurrency();
      cpus > 0 && config.gameLoop.cpuAffinity >= static_cast<int>(cpus)) {
    return std::unexpected("gameLoop.cpuAffinity must be less than the CPU count (" +
                           std::to_string(cpus) + "); use -1 to leave the thread unpinned");
  }

  if (config.recorder.capacity == 0) {
    return std::unexpected("recorder.capacity must be greater than 0");
  }

  return config;
}
