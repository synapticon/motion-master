#pragma once

#include <uwebsockets/App.h>

#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace mm::node {
class DeviceManager;
class MonitoringManager;
}  // namespace mm::node

/// @brief HTTP-transport glue shared by the built-in server and route plug-in libs.
///
/// This layer sits above @c mm::node (the transport-agnostic domain layer) and below the app: it is
/// the one place that knows about uWebSockets. @c mm::node must never depend on it. It exists so a
/// route plug-in lib (e.g. @c mm::example) can register endpoints with the exact same response
/// shape (content type + CORS) as the built-in routes without depending on the app.
namespace mm::api {

/// @brief The live collaborators a route plug-in needs, handed to it at registration time.
///
/// A plug-in's @c RegisterRoutesFn receives this by const reference. The referenced objects
/// (@c DeviceManager / @c MonitoringManager) and the storage backing @c corsOrigin all outlive the
/// running server, so a handler may safely capture the individual fields it needs — but it must
/// **never capture the @c RouteContext itself**, which is a temporary that dies once registration
/// returns. Capture the fields instead:
///
/// @code
/// void registerRoutes(uWS::SSLApp& app, const mm::api::RouteContext& ctx) {
///   app.get("/api/example/devices", [&dm = ctx.deviceManager, cors = ctx.corsOrigin](
///                                        auto* res, auto* /*req*/) {
///     mm::api::sendJson(res, cors, summarize(dm));
///   });
/// }
/// @endcode
struct RouteContext {
  /// Device list + SDO/PDO/state/bus access.
  mm::node::DeviceManager& deviceManager;
  /// Monitoring registry (for monitoring-aware plug-ins).
  mm::node::MonitoringManager& monitoringManager;
  /// Value to send in `Access-Control-Allow-Origin`; outlives run().
  std::string_view corsOrigin;
};

/// @brief A route plug-in: registers its routes on the HTTP app.
///
/// Called once, on the HTTP server's event-loop thread, **after** the built-in routes and
/// **before** the CORS preflight, the catch-all 404, and `listen()`. A plug-in should register
/// only its own specific paths (e.g. `/api/example/...`); the server owns the `OPTIONS /api/*`
/// preflight and the `/*` fallthrough. Registration order does not affect matching — uWS routes by
/// specificity — but a plug-in must not claim `/api/*` or `/*` wildcards.
///
/// Wire one up in the composition root with @c HttpServer::addRoutes (before @c start()).
using RegisterRoutesFn = std::function<void(uWS::SSLApp& app, const RouteContext& ctx)>;

/// @brief Writes @p body as a 200 application/json response with the CORS header.
///
/// Uses the `replace` error handler so a string-typed value carrying non-UTF-8 bytes (e.g. a
/// garbage VISIBLE_STRING from a misbehaving device) is rendered with U+FFFD instead of throwing —
/// an uncaught throw on the uWS loop terminates the whole server.
template <typename Res>
void sendJson(Res* res, std::string_view corsOrigin, const nlohmann::json& body) {
  res->writeHeader("Content-Type", "application/json")
      ->writeHeader("Access-Control-Allow-Origin", corsOrigin)
      ->end(body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
}

/// @brief Writes a @p status response carrying a `{"error": message}` JSON body and the CORS
/// header.
template <typename Res>
void sendError(Res* res, std::string_view status, std::string_view corsOrigin,
               std::string_view message) {
  res->writeStatus(status)
      ->writeHeader("Content-Type", "application/json")
      ->writeHeader("Access-Control-Allow-Origin", corsOrigin)
      ->end(nlohmann::json{{"error", std::string(message)}}.dump());
}

/// @brief Writes a bare @p status response (no body) with the CORS header.
template <typename Res>
void sendStatus(Res* res, std::string_view status, std::string_view corsOrigin) {
  res->writeStatus(status)->writeHeader("Access-Control-Allow-Origin", corsOrigin)->end();
}

}  // namespace mm::api
