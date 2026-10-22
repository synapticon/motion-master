#include "late_frame_source.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>

#include "node/device_manager.h"

namespace mm {

node::NotificationBus::Source lateFrameSource(node::DeviceManager& deviceManager,
                                              std::chrono::milliseconds interval) {
  // The band last reported, in microseconds: 0, then 20 ms, then 100 ms. Shared by the two
  // lambdas, which the bus may copy.
  auto reported = std::make_shared<int64_t>(0);
  return {
      .revision =
          [interval] {
            // Not a counter: the counts live in the transport, behind the bus lock, and the
            // revision must not take one. A tick per interval makes render read them.
            return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch() /
                                              interval);
          },
      .render = [&deviceManager, reported]() -> std::optional<nlohmann::json> {
        const auto stats = deviceManager.frameStats();
        if (!stats) {
          return std::nullopt;  // no bus, or a transport without frames
        }
        // A reset of the counts starts the bands again, so the next crossing is reported again.
        if (stats->latestReturnUs < *reported) {
          *reported = 0;
        }
        constexpr int64_t kBandsUs[] = {20'000, 100'000};
        int64_t crossed = 0;
        for (const int64_t band : kBandsUs) {
          if (stats->latestReturnUs > band && *reported < band) {
            crossed = band;
          }
        }
        if (crossed == 0) {
          return std::nullopt;
        }
        *reported = crossed;
        spdlog::warn(
            "A control-plane frame came back {:.1f} ms after it was sent, later than the {} ms "
            "wait. Since the bus came up: {} frames measured, {} later than 2 ms, {} later than "
            "20 ms, {} later than 100 ms, {} never came back",
            static_cast<double>(stats->latestReturnUs) / 1000.0, crossed / 1000, stats->returned,
            stats->over2Ms, stats->over20Ms, stats->over100Ms, stats->lost);
        nlohmann::json data = *stats;
        data["event"] = kLateFrameEvent;
        data["bandUs"] = crossed;
        return data;
      },
      .interval = interval,
  };
}

}  // namespace mm
