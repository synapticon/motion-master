#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include "node/notification_bus.h"

class GameLoop;

/// @file
/// @brief The log line for a real-time loop that skips cycles for longer than a moment.

namespace mm {

/// @brief Decides, one reading at a time, when the loop has run slow long enough to say so.
///
/// It reads the cumulative counts of executed and skipped cycles once per window. A window is low
/// when more than @c kLowShare of its cycles were skipped. After @c kWindowsToChange low windows in
/// a row it warns once, and after as many good windows in a row it says the rate is back. So a
/// burst of skips at start-up or after a stall never reaches the log, and a rate near the limit
/// does not flap.
///
/// A period change starts a fresh health epoch, which sets both counts back to zero. A reading
/// below the one before is that reset: it becomes the new baseline and is not judged.
class CycleRateWatch {
 public:
  /// The share of skipped cycles above which a window is low.
  static constexpr double kLowShare = 0.10;
  /// The windows in a row that change the verdict.
  static constexpr int kWindowsToChange = 3;

  /// @brief Takes the counts at the end of a window, and returns the line to log, if any.
  std::optional<std::string> observe(uint64_t executedCycles, uint64_t skippedCycles,
                                     double targetHz);

  /// @brief Whether the last line said the loop runs slow.
  bool low() const { return low_; }

 private:
  bool started_ = false;
  uint64_t lastExecuted_ = 0;
  uint64_t lastSkipped_ = 0;
  int streak_ = 0;  // windows in a row that disagree with the current verdict
  bool low_ = false;
};

/// @brief The notification source that runs a CycleRateWatch on the loop once a second.
///
/// It only logs, and never sends a message: a slow loop is for the support log, not for clients
/// (#163). Its revision is the total cycle count, which changes every cycle, so it is read on every
/// interval and can say when the rate is back as well as when it falls.
///
/// @param gameLoop  Borrowed. Must outlive the bus this source is registered with.
/// @param interval  The window. A second keeps the line close to what caused it.
node::NotificationBus::Source cycleRateSource(
    const GameLoop& gameLoop, std::chrono::milliseconds interval = std::chrono::seconds{1});

}  // namespace mm
