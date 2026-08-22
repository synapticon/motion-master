#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <mutex>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <vector>

#include "core/cyclic_task.h"
#include "node/fsoe_connection.h"

namespace mm::node {

class DeviceManager;

/// @brief A connection and the configuration it was opened with, for a listing.
struct FsoeConnectionReport {
  FsoeConnectionConfig config;
  FsoeConnectionState state;
};

/// @brief Serialises one connection: its configuration, its state, and the decoded safe values.
///
/// The safe process values are rendered beside the raw SafeInputs octets rather than instead of
/// them. The octets are what the CRC covered; the decode is this layer's reading of them, and a
/// client chasing a mapping problem needs to see both.
void to_json(nlohmann::json& j, const FsoeConnectionReport& report);

/// @brief Owns every FSoE connection and drives them from the cycle.
///
/// The composition root builds one, the API opens and closes connections on it, and
/// @c FsoeCyclicTask steps it once per bus cycle. It is the same shape as @c MonitoringManager and
/// @c ProcedureManager: a subsystem the HTTP layer talks to, holding no device pointers of its own.
///
/// **Connections are appended, never removed.** The cycle thread walks a fixed array of pointers
/// whose length only grows, published with a release store — so it needs no lock and cannot see a
/// half-built connection. Closing marks a connection inactive and leaves it in place; re-opening
/// one appends a fresh connection and retires the old. The array is small and bounded, and a
/// re-open is a human-scale action, so the cost of never reclaiming a slot is a few hundred bytes.
class FsoeManager {
 public:
  /// @brief Connections one manager can hold, including retired ones.
  static constexpr size_t kMaxConnections = 16;

  explicit FsoeManager(DeviceManager& deviceManager) : deviceManager_(deviceManager) {}

  /// @brief Opens a connection to one drive, replacing any connection already open to it.
  ///
  /// Control plane. Everything expensive happens here: the mapping read, the frame checks, and the
  /// allocation. On success the cycle picks the connection up immediately and starts the handshake.
  std::expected<FsoeConnectionReport, std::string> open(const FsoeConnectionConfig& config);

  /// @brief Stops driving a connection. The drive's watchdog then drops its outputs.
  std::expected<void, std::string> close(uint16_t slavePosition);

  /// @brief The live connection to @p slavePosition, or @c nullptr. Control plane.
  FsoeConnection* find(uint16_t slavePosition);

  /// @brief Every live connection with its current state. Control plane.
  [[nodiscard]] std::vector<FsoeConnectionReport> report() const;

  /// @brief Steps every connection. **Cycle thread only.**
  ///
  /// The elapsed time comes from a steady clock read here rather than from the loop's nominal
  /// period, because the FSoE watchdog measures real time: a loop that skipped ten cycles has been
  /// silent for ten periods, and the watchdog has to see that.
  void execute();

 private:
  DeviceManager& deviceManager_;

  mutable std::mutex mutex_;  ///< Control plane only. The cycle thread never takes it.
  std::vector<std::unique_ptr<FsoeConnection>> owned_;

  // What the cycle thread walks: raw pointers into owned_, appended and never moved.
  std::array<FsoeConnection*, kMaxConnections> published_{};
  std::atomic<size_t> publishedCount_{0};

  std::optional<std::chrono::steady_clock::time_point> lastCycle_;  ///< Cycle thread only.
};

/// @brief Game-loop task that runs every FSoE connection.
///
/// Register it **after** @c ProcessDataCyclicTask: it reads the input image that task just
/// captured, and the frame it writes goes out on the next exchange. That one cycle of delay is what
/// an FSoE master sees on any fieldbus, and the state machine is written for it.
class FsoeCyclicTask : public mm::core::CyclicTask {
 public:
  explicit FsoeCyclicTask(FsoeManager& manager) : manager_(manager) {}

  void execute(const mm::core::CycleContext&) noexcept override { manager_.execute(); }

 private:
  FsoeManager& manager_;
};

}  // namespace mm::node
