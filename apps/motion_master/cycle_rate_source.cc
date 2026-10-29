#include "cycle_rate_source.h"

#include <spdlog/spdlog.h>

#include <format>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

#include "game_loop.h"

namespace mm {

std::optional<std::string> CycleRateWatch::observe(uint64_t executedCycles, uint64_t skippedCycles,
                                                   double targetHz) {
  const bool reset = executedCycles < lastExecuted_ || skippedCycles < lastSkipped_;
  if (!started_ || reset) {
    started_ = true;
    lastExecuted_ = executedCycles;
    lastSkipped_ = skippedCycles;
    streak_ = 0;
    return std::nullopt;
  }
  const uint64_t executed = executedCycles - lastExecuted_;
  const uint64_t skipped = skippedCycles - lastSkipped_;
  lastExecuted_ = executedCycles;
  lastSkipped_ = skippedCycles;
  const uint64_t total = executed + skipped;
  if (total == 0) {
    return std::nullopt;
  }
  const double share = static_cast<double>(skipped) / static_cast<double>(total);
  const bool lowWindow = share > kLowShare;
  if (lowWindow == low_) {
    streak_ = 0;
    return std::nullopt;
  }
  if (++streak_ < kWindowsToChange) {
    return std::nullopt;
  }
  streak_ = 0;
  low_ = lowWindow;
  // The share of the grid the loop ran is the achieved rate as a fraction of the target.
  const double achievedHz = targetHz * (1.0 - share);
  if (low_) {
    return std::format(
        "GameLoop: the loop skipped {:.0f} % of its cycles for {} s and runs at about {:.0f} Hz of "
        "{:.0f} Hz; this host does not keep the period",
        share * 100.0, kWindowsToChange, achievedHz, targetHz);
  }
  return std::format("GameLoop: the loop runs at about {:.0f} Hz of {:.0f} Hz again", achievedHz,
                     targetHz);
}

node::NotificationBus::Source cycleRateSource(const GameLoop& gameLoop,
                                              std::chrono::milliseconds interval) {
  auto watch = std::make_shared<CycleRateWatch>();
  return {
      .revision = [&gameLoop] { return gameLoop.executedCycles() + gameLoop.skippedCycles(); },
      .render = [&gameLoop, watch]() -> std::optional<nlohmann::json> {
        const auto health = gameLoop.health();
        if (auto line =
                watch->observe(health.executedCycles, health.skippedCycles, health.targetHz)) {
          if (watch->low()) {
            spdlog::warn("{}", *line);
          } else {
            spdlog::info("{}", *line);
          }
        }
        // Log only: a slow loop is not a client event.
        return std::nullopt;
      },
      .interval = interval,
  };
}

}  // namespace mm
