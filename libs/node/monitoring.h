#pragma once

#include <chrono>
#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <vector>

namespace mm::node {

/// @brief One object a monitoring samples, addressed by bus position and CoE index.
struct MonitoredParameter {
  uint16_t devicePosition;  ///< 1-based bus position of the owning device.
  uint16_t index;           ///< CoE object index.
  uint8_t subindex;         ///< CoE object subindex.
};

/// @brief A client-defined recording of a set of parameters streamed over time.
///
/// Created via @c POST @c /api/monitorings and streamed over the monitoring WebSocket under its
/// @c topic. The stream is **lossless**: every process-data cycle recorded since the last flush is
/// delivered. @c interval is the flush cadence (how often a batch is published), not a sample rate
/// — the manager ships every cycle-row in one positional-array batch per flush (one inner array
/// per cycle, in @c parameters order, prefixed by the cycle timestamp). This struct is the
/// immutable client configuration; the runtime state (read cursor, per-parameter PDO/SDO
/// classification) lives on the manager that owns the monitoring, not here.
struct Monitoring {
  std::string topic;                   ///< URL-safe unique id; also the WebSocket pub/sub topic.
  std::optional<std::string> name;     ///< Optional human-readable label (display only).
  std::chrono::milliseconds interval;  ///< Flush cadence (bounded [10 ms, 1000 ms]).
  std::vector<MonitoredParameter> parameters;  ///< Objects to sample, in positional order.
};

/// @brief Serialises a MonitoredParameter to JSON: @c {devicePosition, index, subindex}.
void to_json(nlohmann::json& j, const MonitoredParameter& p);

/// @brief Serialises a Monitoring to JSON: @c {topic, name?, interval, parameters}.
///
/// @c name is omitted when unset; @c interval is emitted as an integer count of milliseconds.
void to_json(nlohmann::json& j, const Monitoring& m);

}  // namespace mm::node
