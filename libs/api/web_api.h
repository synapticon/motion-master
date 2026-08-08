#pragma once

#include <uwebsockets/App.h>

#include <chrono>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "api/router.h"

namespace mm::node {
class DeviceManager;
class MonitoringManager;
}  // namespace mm::node

/// @brief HTTP-transport glue shared by the built-in server and route plug-in libs.
///
/// The response helpers here write directly to a @c uWS::HttpResponse, which is only valid on the
/// event-loop thread — so they are for the handful of framework-level responses that genuinely run
/// there (the OPTIONS preflight, the HTML index, the catch-all 404). **Everything that serves the
/// API goes through @c mm::api::Router instead**, whose handlers return a @c Response value and run
/// off the loop. A timed variant of these once existed and was used by every device endpoint; it
/// was removed with the last of them, because a shared header offering the blocking shape is how a
/// fixed bug comes back.
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
/// void registerRoutes(mm::api::Router& router, const mm::api::RouteContext& ctx) {
///   router.get("/api/example/devices", [&dm = ctx.deviceManager](const mm::api::Request&) {
///     return mm::api::json(summarize(dm));
///   });
/// }
/// @endcode
struct RouteContext {
  /// Device list + SDO/PDO/state/bus access.
  mm::node::DeviceManager& deviceManager;
  /// Monitoring registry (for monitoring-aware plug-ins).
  mm::node::MonitoringManager& monitoringManager;
  /// Value to send in `Access-Control-Allow-Origin`; outlives run().
  ///
  /// Rarely needed now: a @c Router handler returns a @c Response and the framework writes the
  /// header. It remains for a plug-in that hand-rolls a response on the loop thread.
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
/// A plug-in is handed the @c Router rather than the raw app, so its handlers run off the event
/// loop like every built-in route. That is the point of passing it: a plug-in that took the app
/// could register a handler doing bus I/O on the loop thread and stall the whole API, which is the
/// bug the Router exists to make unrepresentable — and a shared header offering the unsafe path is
/// how it would come back.
using RegisterRoutesFn = std::function<void(Router& router, const RouteContext& ctx)>;

/// @brief Writes the `Access-Control-Allow-Origin` header and returns @p res for chaining.
///
/// The single home for the origin-policy header. sendJson/sendError/sendStatus emit it for the
/// common JSON paths; call this directly for a hand-rolled response that can't use them — a raw
/// octet-stream body, a text/yaml dump, or a bespoke status line — so the header name and policy
/// live in exactly one place. Order-independent among headers; call before end().
template <typename Res>
Res* setCorsOrigin(Res* res, std::string_view corsOrigin) {
  return res->writeHeader("Access-Control-Allow-Origin", corsOrigin);
}

/// @brief Attaches the server-measured wire-time header (`X-Wire-Us`, microseconds) to a response.
///
/// @p wireUs is the server-measured time spent on the device-side operation itself — control-plane
/// lock acquire plus the mailbox/ESC wire transaction(s) — *not* the end-to-end HTTP round-trip,
/// which a cross-origin browser client observes as much larger (TLS + transport overhead).
/// Reporting it lets the client attribute the device cost to the device and the remainder to the
/// browser/transport. For a single-transaction endpoint (one SDO/FoE/register access) it is
/// essentially the pure wire round-trip; for a multi-transaction one (object-dictionary
/// enumeration, PDO-mapping read/write) it is the total across all of the operation's transactions.
/// Because the value rides a header rather than the body, it is the one uniform timing channel that
/// works for any response shape (JSON or raw octet-stream) without touching each endpoint's body
/// schema. The header is CORS-exposed so the PWA can read it cross-origin. Emitted on **both**
/// success and failure — a failed operation still consumed device time (e.g. an SDO read that waits
/// out the mailbox timeout), and the client shows it the same way; failures route through
/// sendError()'s @c wireUs parameter so the header lands after writeStatus(). Call **before** the
/// body/end() (uWS requires all headers written before the body). Returns @p res for chaining,
/// mirroring setCorsOrigin.
template <typename Res>
Res* setWireTime(Res* res, std::chrono::microseconds wireUs) {
  return res->writeHeader("Access-Control-Expose-Headers", "X-Wire-Us")
      ->writeHeader("X-Wire-Us", std::to_string(wireUs.count()));
}

/// @brief Writes @p body as a 200 application/json response with the CORS header.
///
/// Uses the `replace` error handler so a string-typed value carrying non-UTF-8 bytes (e.g. a
/// garbage VISIBLE_STRING from a misbehaving device) is rendered with U+FFFD instead of throwing —
/// an uncaught throw on the uWS loop terminates the whole server.
template <typename Res>
void sendJson(Res* res, std::string_view corsOrigin, const nlohmann::json& body) {
  setCorsOrigin(res, corsOrigin)
      ->writeHeader("Content-Type", "application/json")
      ->end(body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
}

/// @brief Writes @p body verbatim as a 200 response with the given @p contentType and the CORS
/// header.
///
/// The raw-bytes analogue of sendJson: for a response whose body is already serialized (an
/// octet-stream dump, a verbatim on-disk file, a text/yaml spec) rather than a @c nlohmann::json
/// value — @p body is sent as-is. For anything richer — an extra header (e.g. Content-Disposition),
/// a non-200 status, or backpressure-aware streaming via @c tryEnd — drop to setCorsOrigin +
/// writeHeader directly instead.
template <typename Res>
void sendBytes(Res* res, std::string_view corsOrigin, std::string_view contentType,
               std::string_view body) {
  setCorsOrigin(res, corsOrigin)->writeHeader("Content-Type", contentType)->end(body);
}

/// @brief Writes a @p status response carrying a `{"error": message}` JSON body and the CORS
/// header.
///
/// Pass @p wireUs to attach the `X-Wire-Us` timing header to the failure the same way a success
/// carries it — a failed device transaction still consumed wire time (an SDO read that waits out
/// the mailbox timeout, a partial FoE transfer), and the client renders it identically. The header
/// is written after writeStatus() so uWS keeps the non-200 status (headers-before-status would
/// force a default 200).
template <typename Res>
void sendError(Res* res, std::string_view status, std::string_view corsOrigin,
               std::string_view message,
               std::optional<std::chrono::microseconds> wireUs = std::nullopt) {
  auto* r = setCorsOrigin(res->writeStatus(status), corsOrigin);
  if (wireUs) {
    r = setWireTime(r, *wireUs);
  }
  r->writeHeader("Content-Type", "application/json")
      ->end(nlohmann::json{{"error", std::string(message)}}.dump());
}

/// @brief Writes a bare @p status response (no body) with the CORS header.
///
/// @p wireUs optionally attaches the server-measured operation time, exactly as @c sendError does —
/// an endpoint whose success case carries no body (a `204 No Content` delete) still spent time
/// doing the work, and the client shows it the same way as for a body-bearing response.
template <typename Res>
void sendStatus(Res* res, std::string_view status, std::string_view corsOrigin,
                std::optional<std::chrono::microseconds> wireUs = std::nullopt) {
  auto* r = setCorsOrigin(res->writeStatus(status), corsOrigin);
  if (wireUs) {
    r = setWireTime(r, *wireUs);
  }
  r->end();
}

}  // namespace mm::api
