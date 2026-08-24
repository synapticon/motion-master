#pragma once

#include <expected>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "node/monitoring.h"

namespace mm {

/// @brief A client request received over the monitoring WebSocket to (un)subscribe to a topic.
struct WsCommand {
  enum class Action { Subscribe, Unsubscribe };
  Action action = Action::Subscribe;
  std::string topic;  ///< Validated URL-safe topic (a monitoring id).
};

/// @brief Parses an inbound monitoring-WebSocket text message into a subscribe/unsubscribe command.
///
/// Accepts exactly @c {"subscribe":"<topic>"} or @c {"unsubscribe":"<topic>"} where @c <topic> is
/// a URL-safe id. Anything else — malformed JSON, a non-object, an unknown key, a non-string or
/// non-URL-safe topic — yields @c nullopt so the caller can simply ignore it. Pure (no I/O), so it
/// is unit-tested directly; the server's message handler calls it and then @c ws->subscribe /
/// @c ws->unsubscribe.
///
/// @param message  The raw WebSocket text frame.
/// @return The parsed command, or @c nullopt if @p message is not a valid (un)subscribe request.
std::optional<WsCommand> parseWsCommand(std::string_view message);

/// @brief Parses a @c POST @c /api/monitorings request body into a @c Monitoring config.
///
/// Expected shape:
/// @code
/// { "topic": "left-leg", "name": "Left Leg" (optional),
///   "interval": 1000,
///   "parameters": [[devicePosition, index, subindex], ...] }
/// @endcode
/// @c interval is the flush cadence in milliseconds. Each parameter is a three-element array of
/// integers. This checks only the request *shape* (presence, JSON types, ranges) and converts it;
/// semantic validation (URL-safe topic, the reserved notification topic, interval bounds,
/// parameter sourcing) is @c MonitoringManager::create's job. (A legacy @c bufferSize key, if
/// present, is ignored.)
///
/// @param body  The parsed request body.
/// @return The configuration, or an error string naming the first shape problem.
std::expected<mm::node::Monitoring, std::string> parseMonitoringRequest(const nlohmann::json& body);

}  // namespace mm
