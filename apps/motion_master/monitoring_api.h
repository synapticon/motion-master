#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace mm {

/// @brief A client request received over the monitoring WebSocket to (un)subscribe to a topic.
struct WsCommand {
  enum class Action { Subscribe, Unsubscribe };
  Action action;
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

}  // namespace mm
