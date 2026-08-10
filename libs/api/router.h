#pragma once

#include <uwebsockets/App.h>

#include <BS_thread_pool.hpp>
#include <atomic>
#include <charconv>
#include <chrono>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mm::api {

/// @file
/// @brief Routes whose handlers cannot block the event loop, because they do not run on it.
///
/// **The problem this exists to remove.** uWebSockets runs every handler on the app's single loop
/// thread. A handler that blocks therefore blocks the *whole* HTTP API, not merely its own
/// endpoint — and Motion Master is full of handlers that block for seconds by nature: an FoE
/// transfer, an object-dictionary enumeration, any SDO waiting behind a busy control-plane lock.
/// Measured during a firmware installation: a `GET /api/devices/state` waiting on the control-plane
/// lock for a 12-second file transfer stalled every other request behind it, including
/// `/api/version`, which touches no hardware at all, and the procedure-progress poll — the one
/// thing the user was actually watching.
///
/// It could be fixed by wrapping each blocking handler by hand, and that was tried first. It is the
/// wrong shape: it leaves the dangerous thing (touch the bus directly in a handler) as the default
/// and the safe thing as something to remember, across seventy routes and every future one. So the
/// framework does it instead — **a @c Handler cannot block the loop, because it never runs on it.**
///
/// **What a handler is.** A plain function from a @c Request to a @c Response. No uWS types, no
/// response pointer, no lifetime rules, nothing to remember. It runs on a worker thread and returns
/// a value; the framework writes it. That also makes handlers unit-testable without a server, which
/// the old shape made impossible.
///
/// @code
/// router.get("/api/devices/:slavePosition/state", [&dm](const Request& req) -> Response {
///   auto pos = req.parameterAs<uint16_t>("slavePosition");
///   if (!pos) return badRequest("slavePosition must be a number");
///   return timed([&] { return dm.deviceStates({*pos}); });
/// });
/// @endcode
///
/// **The three rules the framework keeps so a handler does not have to.** A uWS response may only
/// be touched from the loop thread — so every write happens inside @c uWS::Loop::defer. A response
/// dies with its connection — so an abort flag, set by @c onAborted on the loop thread, is checked
/// inside that same deferred callback, where the two cannot interleave. And a request object is
/// valid only for the synchronous call — so it is snapshotted into a @c Request before anything is
/// dispatched.

/// @brief A request, snapshotted on the loop thread so a handler can outlive it.
///
/// uWS's own @c HttpRequest is valid only during the synchronous handler call; everything a handler
/// might read is therefore copied out before any work is dispatched. That is a few string copies
/// per request, against a handler that is about to do milliseconds of wire I/O.
class Request {
 public:
  Request(std::string url, std::vector<std::pair<std::string, std::string>> parameters,
          std::string queryString, std::vector<std::pair<std::string, std::string>> headers,
          std::string body)
      : url_(std::move(url)),
        parameters_(std::move(parameters)),
        queryString_(std::move(queryString)),
        headers_(std::move(headers)),
        body_(std::move(body)) {}

  /// @brief The full request path.
  std::string_view url() const { return url_; }

  /// @brief The request body, empty for methods that carry none. Complete — a handler never sees a
  ///        partial body, because dispatch waits for the last chunk.
  const std::string& body() const { return body_; }

  /// @brief A request header by name, lower-cased as uWS delivers it, or empty when absent.
  ///
  /// Needed for content negotiation: several endpoints return raw bytes or parsed JSON depending
  /// on @c Accept, and one reads @c Content-Type.
  std::string_view header(std::string_view name) const {
    for (const auto& [key, value] : headers_) {
      if (key == name) {
        return value;
      }
    }
    return {};
  }

  /// @brief Whether @c Accept asks for @p contentType. Substring, matching how these endpoints have
  ///        always negotiated: an `Accept` of `*/*` or absent means "the default", not "any".
  bool accepts(std::string_view contentType) const {
    return header("accept").find(contentType) != std::string_view::npos;
  }

  /// @brief A path parameter by the name the route pattern declared (`:slavePosition`), or empty.
  ///
  /// **Verbatim — still percent-encoded.** uWS decodes query values but not path segments, and
  /// neither does this. Harmless for every parameter the API has today, all of which are numbers or
  /// fixed slugs that no client would encode. A route taking free-form text in its path is the case
  /// to watch: pass it through @c percentDecode, as the user-cache routes do, or a name containing
  /// a space arrives as `%20` and names nothing.
  std::string_view parameter(std::string_view name) const {
    for (const auto& [key, value] : parameters_) {
      if (key == name) {
        return value;
      }
    }
    return {};
  }

  /// @brief A query-string value by key, percent-decoded, or @c std::nullopt when it has no value.
  ///
  /// Delegates to uWebSockets' own @c uWS::getDecodedQueryValue — the same function
  /// @c HttpRequest::getQuery(key) uses — so a handler reading a query here gets exactly what it
  /// would have got reading it on the loop thread. **Decoding is the reason this is not a
  /// hand-rolled split:** a client encodes its query values (the generated TypeScript client runs
  /// every one through @c encodeURIComponent), so `positions=1,2` arrives as `positions=1%2C2` and
  /// a raw split yields `1%2C2` — which parses as no number at all.
  ///
  /// Follows uWS's semantics rather than inventing any: a key needs an `=` to be seen at all, and a
  /// present-but-empty value is reported the same as an absent one. So `?flag` alone is not a
  /// usable flag — test a value, not presence.
  ///
  /// Returns an owned string because the decode happens in place: each call decodes into its own
  /// copy of the query, which is what keeps repeated lookups independent of each other.
  std::optional<std::string> query(std::string_view key) const;

  /// @brief A path parameter parsed as an integer, or @c std::nullopt if it is absent or not one.
  ///
  /// Accepts a `0x` prefix, because a CoE index is written that way everywhere else — its
  /// documentation, the Console, the specification — and a route that only took decimal would make
  /// a user convert by hand. Trailing characters are rejected, so `12abc` is not 12.
  template <typename T>
  std::optional<T> parameterAs(std::string_view name) const {
    return parseNumber<T>(parameter(name));
  }

  /// @brief A query value parsed as an integer, or @c std::nullopt if absent or not one.
  template <typename T>
  std::optional<T> queryAs(std::string_view key) const {
    auto value = query(key);
    return value ? parseNumber<T>(*value) : std::nullopt;
  }

 private:
  template <typename T>
  static std::optional<T> parseNumber(std::string_view text) {
    if (text.empty()) {
      return std::nullopt;
    }
    const bool hex = text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
    const std::string_view digits = hex ? text.substr(2) : text;
    T value{};
    const auto [ptr, ec] =
        std::from_chars(digits.data(), digits.data() + digits.size(), value, hex ? 16 : 10);
    if (ec != std::errc{} || ptr != digits.data() + digits.size()) {  // NOLINT(whitespace/braces)
      return std::nullopt;
    }
    return value;
  }

  std::string url_;
  std::vector<std::pair<std::string, std::string>> parameters_;
  std::string queryString_;
  std::vector<std::pair<std::string, std::string>> headers_;
  std::string body_;
};

/// @brief A complete response, produced off the loop and written by the framework.
///
/// A value rather than a sequence of calls on a response object: that is what lets a handler run
/// anywhere, and what makes it testable by comparing the returned value.
struct Response {
  std::string status = "200 OK";
  std::string contentType = "application/json";
  std::string body;
  /// Extra headers beyond content type and CORS — @c Content-Disposition for a download, the
  /// hardening set on a user-supplied file, @c X-Wire-Us for a timed device operation.
  std::vector<std::pair<std::string, std::string>> headers;
};

/// @brief Percent-decodes a URL path component — `%20` to a space, `%2F` to a slash.
///
/// RFC 3986 §2.1 calls the mechanism *percent-encoding* and its unit a *percent-encoded octet*
/// (a triplet: `%` and two hex digits); it names the inverse only as "decoding" those octets. The
/// name here follows the WHATWG URL Standard, which calls the operation *percent-decode*. Not
/// `urlDecode`, deliberately — that reads as though it folds `+` into a space, which is exactly
/// what this must not do.
///
/// For **path** components, and deliberately not shared with query decoding: a query decoder also
/// maps `+` to a space (`application/x-www-form-urlencoded`), which in a path is a literal `+`, so
/// decoding `a+b.zip` that way would look up `a b.zip`. @c Request::query handles queries; this
/// handles paths, which uWS hands over still encoded.
///
/// An invalid escape (`%4Z`, a trailing `%`, `%4`) is left verbatim rather than dropped, so a name
/// containing one arrives intact at whatever resolves it and either names something real or fails
/// cleanly — instead of silently becoming a different name.
///
/// The result may contain a NUL, from `%00`, and its length is authoritative: never treat it as a C
/// string, or such a name truncates into a different one.
std::string percentDecode(std::string_view text);

/// @brief The `:name` tokens of a route pattern, in the order uWS will index them.
///
/// uWS addresses path parameters positionally — `req->getParameter(0)`; naming them is this layer's
/// doing, so a handler asks for `parameter("slavePosition")` and stays correct when a route gains a
/// segment ahead of it. A name runs to the next `/` or to the end of the pattern, so a trailing
/// parameter needs no terminator, and a pattern with none (`/api/user-cache/*`) yields nothing.
///
/// Declared here rather than kept file-local so the mapping can be tested directly: it is the one
/// piece of pattern parsing this layer does itself, and a route whose names come out shifted by one
/// would still compile, still serve, and answer with another parameter's value.
std::vector<std::string> parameterNames(std::string_view pattern);

/// @brief A 200 response carrying @p body as JSON.
///
/// Serialised with the @c replace error handler so a string value carrying non-UTF-8 bytes (a
/// garbage VISIBLE_STRING from a misbehaving device) renders as U+FFFD rather than throwing — and
/// an uncaught throw here would take the server down.
Response json(const nlohmann::json& body);

/// @brief A 200 response carrying @p body verbatim under @p contentType.
Response bytes(std::string contentType, std::string body);

/// @brief A @p status response carrying a `{"error": message}` body.
Response error(std::string status, std::string_view message);

/// @brief A 400 with @p message — the most common failure, so it gets a name.
Response badRequest(std::string_view message);

/// @brief A 404 with @p message.
Response notFound(std::string_view message);

/// @brief A bare @p status response with no body — a 202 that means "under way", a 204 delete.
///
/// Named for what it produces rather than the plain `status`, which collides with the parameter
/// name the neighbouring send* helpers already use for a status line.
Response statusOnly(std::string status);

/// @brief Attaches the server-measured device time (`X-Wire-Us`) to @p response and returns it.
///
/// The uniform timing channel: it rides a header rather than the body, so it works for any response
/// shape, and it is emitted on failure as well as success because a failed device operation still
/// consumed wire time. See the header's own documentation in @c web_api.h.
Response withWireTime(Response response, std::chrono::microseconds wireUs);

/// @brief Runs a device operation, times it, and turns its @c std::expected into a timed response.
///
/// The shape of nearly every device endpoint, in one place: time the call, send the value as JSON
/// with @c X-Wire-Us on success, or @p errorStatus with the same header on failure. Only @p op is
/// timed, so body serialisation stays out of the figure.
template <typename Op>
Response timed(Op&& op, std::string errorStatus = "500 Internal Server Error") {
  const auto t0 = std::chrono::steady_clock::now();
  auto result = std::forward<Op>(op)();
  const auto wireUs =
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0);
  if (!result) {
    return withWireTime(error(std::move(errorStatus), result.error()), wireUs);
  }
  return withWireTime(json(nlohmann::json(*result)), wireUs);
}

/// @brief What a route does: a pure function from a snapshotted request to a response.
using Handler = std::function<Response(const Request&)>;

/// @brief Registers routes whose handlers run off the event loop.
///
/// Holds no ownership: the app, loop, pool and CORS origin all outlive it, and it is used only
/// during registration.
class Router {
 public:
  /// @param app         The uWS app to register on.
  /// @param loop        The app's loop; responses are written by deferring onto it.
  /// @param pool        Workers the handlers run on. Must be drained before @p loop is closed.
  /// @param stopping    Set once shutdown has begun; while it is set, requests are answered 503
  ///                    instead of being dispatched. See @c stopping_.
  /// @param corsOrigin  Value for `Access-Control-Allow-Origin`; must outlive the server.
  Router(uWS::SSLApp& app, uWS::Loop* loop, BS::light_thread_pool& pool,
         const std::atomic<bool>& stopping, std::string_view corsOrigin)
      : app_(app), loop_(loop), pool_(pool), stopping_(stopping), corsOrigin_(corsOrigin) {}

  /// @brief Registers @p handler for GET @p pattern. Path parameters are `:name` as in uWS.
  void get(const std::string& pattern, Handler handler) {
    add("GET", pattern, std::move(handler), /*hasBody=*/false);
  }
  /// @brief Registers @p handler for POST @p pattern; the body is accumulated before dispatch.
  void post(const std::string& pattern, Handler handler) {
    add("POST", pattern, std::move(handler), /*hasBody=*/true);
  }
  /// @brief Registers @p handler for PUT @p pattern; the body is accumulated before dispatch.
  void put(const std::string& pattern, Handler handler) {
    add("PUT", pattern, std::move(handler), /*hasBody=*/true);
  }
  /// @brief Registers @p handler for DELETE @p pattern.
  void del(const std::string& pattern, Handler handler) {
    add("DELETE", pattern, std::move(handler), /*hasBody=*/false);
  }

 private:
  void add(std::string_view method, const std::string& pattern, Handler handler, bool hasBody);

  uWS::SSLApp& app_;
  uWS::Loop* loop_;
  BS::light_thread_pool& pool_;
  /// Whether the server has begun shutting down, owned by the server and only ever set there.
  ///
  /// A handler finishing its work defers the write back onto the loop, so a worker must never
  /// outlive the loop thread. The server drains the pool before closing the loop for that reason —
  /// but draining is not enough on its own, because the loop keeps accepting while it drains, and a
  /// request arriving after the drain would dispatch a *fresh* worker that then defers onto a loop
  /// whose thread has since exited. This flag closes that door: the server sets it on the loop
  /// thread and only then drains, and dispatch happens on that same thread — so once it is set, no
  /// further request can reach the pool, and everything already there is what the drain waits for.
  /// Like @c aborted in @c add, it is serialised by the thread rather than protected by the atomic.
  const std::atomic<bool>& stopping_;
  std::string_view corsOrigin_;
};

}  // namespace mm::api
