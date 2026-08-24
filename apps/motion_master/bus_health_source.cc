#include "bus_health_source.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>

#include "node/device_manager.h"

namespace mm {

node::NotificationBus::Source busHealthSource(node::DeviceManager& deviceManager,
                                              std::chrono::milliseconds interval) {
  return {
      .revision = [&deviceManager] { return deviceManager.shortWkcCycles(); },
      .render = [&deviceManager]() -> std::optional<nlohmann::json> {
        const auto info = deviceManager.processImageInfo();
        // A reset() clears the count, which reads as a change. There is nothing to report about a
        // bus that has never answered short, so say nothing rather than announce a zero.
        if (info.shortWkcCycles == 0) {
          return std::nullopt;
        }
        // The log line reports ages and the payload absolute times, on purpose. A log line is read
        // where it sits, so it has to carry its own reference point — and printing an absolute time
        // would have to pick a zone, which beside spdlog's local-time prefix reads as a
        // contradiction. A client has a clock and the rest of this API in epoch microseconds.
        const auto nowUs =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count());
        const auto secondsBefore = [nowUs](uint64_t thenUs) {
          return thenUs == 0 || thenUs > nowUs ? 0.0 : static_cast<double>(nowUs - thenUs) / 1e6;
        };
        spdlog::warn(
            "Process data: {} cycle(s) answered with a short working counter since the bus came "
            "up; the first was {:.1f} s and the last {:.1f} s before this line",
            info.shortWkcCycles, secondsBefore(info.firstShortWkcUs),
            secondsBefore(info.lastShortWkcUs));
        return nlohmann::json{
            {"event", kShortWorkingCounterEvent},      {"shortWkcCycles", info.shortWkcCycles},
            {"firstShortWkcUs", info.firstShortWkcUs}, {"lastShortWkcUs", info.lastShortWkcUs},
            {"expectedWkc", info.expectedWkc},         {"lastWkc", info.lastWkc}};
      },
      .interval = interval,
  };
}

}  // namespace mm
