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

/// @brief A client-defined recording of a set of parameters sampled over time.
///
/// Created via @c POST @c /api/monitorings and streamed over the monitoring WebSocket under its
/// @c topic. The server samples @c parameters every @c interval off the RT thread, accumulates
/// @c bufferSize samples, and publishes each batch as a positional array (one inner array per
/// sample, in @c parameters order). This struct is the immutable client configuration; the
/// runtime state (batch buffer, per-parameter PDO/SDO classification, subscriber count) lives on
/// the manager that owns the monitoring, not here.
struct Monitoring {
  std::string topic;                   ///< URL-safe unique id; also the WebSocket pub/sub topic.
  std::optional<std::string> name;     ///< Optional human-readable label (display only).
  std::chrono::milliseconds interval;  ///< Sampling period.
  uint32_t bufferSize;                 ///< Samples accumulated before a batch is published.
  std::vector<MonitoredParameter> parameters;  ///< Objects to sample, in positional order.
};

/// @brief Serialises a MonitoredParameter to JSON: @c {devicePosition, index, subindex}.
void to_json(nlohmann::json& j, const MonitoredParameter& p);

/// @brief Serialises a Monitoring to JSON: @c {topic, name?, interval, bufferSize, parameters}.
///
/// @c name is omitted when unset; @c interval is emitted as an integer count of milliseconds.
void to_json(nlohmann::json& j, const Monitoring& m);

}  // namespace mm::node
