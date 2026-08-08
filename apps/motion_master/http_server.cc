#include "http_server.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <expected>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cert_info.h"
#include "comm/al_status_codes.h"
#include "comm/base.h"
#include "comm/esc_registers.h"
#include "comm/fieldbus_driver.h"
#include "comm/foe_error_codes.h"
#include "comm/mailbox_error_codes.h"
#include "comm/object_data_types.h"
#include "comm/sdo_abort_codes.h"
#include "comm/sii.h"
#include "core/system_info.h"
#include "core/user_cache.h"
#include "core/util.h"
#include "etg/esi_request.h"
#include "monitoring_api.h"
#include "node/cia402.h"
#include "node/cia402_control.h"
#include "node/cia402_drive.h"
#include "node/device_manager.h"
#include "node/device_parameter.h"
#include "node/firmware_package.h"
#include "node/monitoring_manager.h"
#include "node/pdo_mapping.h"
#include "node/procedure_catalogue.h"
#include "node/procedure_manager.h"
#include "node/somanet_control.h"
#include "swagger_spec.h"

namespace {

// Parses the optional comma-separated "positions" query into 1-based slave positions. An absent
// or empty parameter yields an empty vector (which the device manager reads as "all devices"). On
// a malformed token it writes a 400 (with the CORS header) to @p res and returns nullopt — the
// caller must return immediately without writing a further response.
// The Router-shaped counterpart of parsePositions below: it returns the failure instead of writing
// it, because a handler that runs off the loop has no response object to write to.
std::expected<std::vector<uint16_t>, std::string> parsePositions(const mm::api::Request& req) {
  std::vector<uint16_t> positions;
  const auto raw = req.query("positions");
  if (!raw || raw->empty()) {
    return positions;  // No filter: every discovered device.
  }
  const std::string text(*raw);
  std::istringstream stream(text);
  std::string token;
  while (std::getline(stream, token, ',')) {
    uint16_t position{};
    auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), position);
    if (ec != std::errc() || ptr != token.data() + token.size()) {
      return std::unexpected("'positions' must be a comma-separated list of numbers, e.g. 1,2");
    }
    positions.push_back(position);
  }
  return positions;
}

template <typename Res, typename Req>
std::optional<std::vector<uint16_t>> parsePositions(Res* res, Req* req,
                                                    std::string_view corsOrigin) {
  std::vector<uint16_t> positions;
  auto posParam = req->getQuery("positions");
  if (posParam.empty()) {
    return positions;
  }
  std::string posStr(posParam);
  std::istringstream ss(posStr);
  std::string token;
  while (std::getline(ss, token, ',')) {
    uint16_t pos{};
    auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), pos);
    if (ec != std::errc() || ptr != token.data() + token.size()) {
      mm::api::sendStatus(res, "400 Bad Request", corsOrigin);
      return std::nullopt;
    }
    positions.push_back(pos);
  }
  return positions;
}

// The JSON/error/status response helpers live in api/web_api.h so route plug-ins can share the
// exact same response shape (content type + CORS) as the built-in routes. Pull them in unqualified
// so the call sites below read the same as before.
using mm::api::sendBytes;
using mm::api::sendError;
using mm::api::sendJson;
using mm::api::sendStatus;
using mm::api::sendTimedJson;
using mm::api::setCorsOrigin;
using mm::api::setWireTime;

// Maps a failed procedure operation to its status line. This translation is the whole reason
// ProcedureError is structured rather than a string: each kind tells a client to do something
// different — wait and retry, fix the address, fix the body — and doing it in one place is what
// keeps the four procedure handlers from each inventing their own mapping.
constexpr std::string_view procedureErrorStatus(mm::node::ProcedureError::Kind kind) {
  switch (kind) {
    case mm::node::ProcedureError::Kind::kBusy:
      return "409 Conflict";
    case mm::node::ProcedureError::Kind::kUnknownDevice:
    case mm::node::ProcedureError::Kind::kUnknownProcedure:
      return "404 Not Found";
    case mm::node::ProcedureError::Kind::kInvalidRequest:
      return "400 Bad Request";
  }
  return "500 Internal Server Error";
}

// Release or engage a device's brake, which differ only in which operation runs — everything around
// it (position, the optional settle, the timed call, the state read back) is identical, so the two
// routes share this rather than duplicating it. A template because the uWS response/request types
// are the loop's, deduced at the route.
template <typename Res, typename Req>
void handleBrakeCommand(Res* res, Req* req, mm::node::DeviceManager& deviceManager,
                        std::string_view corsOrigin, bool release) {
  uint16_t pos{};
  auto posParam = req->getParameter("slavePosition");
  auto [p, ec] = std::from_chars(posParam.data(), posParam.data() + posParam.size(), pos);
  if (ec != std::errc() || p != posParam.data() + posParam.size()) {
    sendStatus(res, "400 Bad Request", corsOrigin);
    return;
  }
  // The wait after the brake is commanded. It is exposed because brake release is open-loop — the
  // firmware reports no confirmation — so this margin is the only thing standing between
  // "commanded" and "assume it let go", and the right value is a property of the machine.
  auto settle = std::chrono::milliseconds(50);
  if (auto q = req->getQuery("settle"); !q.empty()) {
    uint32_t settleMs = 0;
    auto [qp, qec] = std::from_chars(q.data(), q.data() + q.size(), settleMs);
    if (qec != std::errc() || qp != q.data() + q.size()) {
      sendStatus(res, "400 Bad Request", corsOrigin);
      return;
    }
    settle = std::chrono::milliseconds(settleMs);
  }
  if (!deviceManager.findDevice(pos)) {
    sendStatus(res, "404 Not Found", corsOrigin);
    return;
  }
  // Synchronous and timed, like the CiA402 command and FoE handlers: this blocks the HTTP thread
  // for the pull time while the WebSocket (own loop) and the RT loop run on. The wire time is most
  // of the round trip here, since the call is mostly that deliberate wait.
  sendTimedJson(res, corsOrigin, "409 Conflict", [&] {
    return release ? mm::node::releaseBrake(deviceManager, pos, settle)
                   : mm::node::engageBrake(deviceManager, pos, settle);
  });
}

// Parses the "value" field of a smart parameter-write body into a DeviceParameterValue. The exact
// numeric width does not matter here — DeviceManager::writeDeviceParameter coerces the value to the
// parameter's declared data type — so a JSON integer becomes int64/uint64, a real becomes double, a
// string stays a string, and an array becomes a byte vector.
std::expected<mm::node::DeviceParameterValue, std::string> parseParameterValue(
    const nlohmann::json& v) {
  if (v.is_number_unsigned()) {
    return mm::node::DeviceParameterValue{v.get<uint64_t>()};
  }
  if (v.is_number_integer()) {
    return mm::node::DeviceParameterValue{v.get<int64_t>()};
  }
  if (v.is_number_float()) {
    return mm::node::DeviceParameterValue{v.get<double>()};
  }
  if (v.is_boolean()) {
    return mm::node::DeviceParameterValue{static_cast<uint64_t>(v.get<bool>())};
  }
  if (v.is_string()) {
    return mm::node::DeviceParameterValue{v.get<std::string>()};
  }
  if (v.is_array()) {
    return mm::node::DeviceParameterValue{v.get<std::vector<uint8_t>>()};
  }
  return std::unexpected<std::string>("unsupported value type; expected number, string, or array");
}

// Parses a POST /api/process-data/outputs body into stage requests. The body is an array of
// [slavePosition, index, subindex, value] rows — the monitoring [[pos,index,sub]] shape with a
// value appended, so the wire stays compact and consistent. The three ids must be non-negative
// integers; value is anything parseParameterValue accepts and is coerced to the object's declared
// type by the node layer. Returns the first shape problem as an error.
std::expected<std::vector<mm::node::OutputStageRequest>, std::string> parseOutputStageRequests(
    const nlohmann::json& body) {
  if (!body.is_array()) {
    return std::unexpected<std::string>(
        "expected an array of [slavePosition, index, subindex, value] entries");
  }
  std::vector<mm::node::OutputStageRequest> requests;
  requests.reserve(body.size());
  for (const auto& row : body) {
    if (!row.is_array() || row.size() != 4) {
      return std::unexpected<std::string>(
          "each entry must be [slavePosition, index, subindex, value]");
    }
    if (!row[0].is_number_unsigned() || !row[1].is_number_unsigned() ||
        !row[2].is_number_unsigned()) {
      return std::unexpected<std::string>(
          "slavePosition, index, and subindex must be non-negative integers");
    }
    auto value = parseParameterValue(row[3]);
    if (!value) {
      return std::unexpected(value.error());
    }
    mm::node::OutputStageRequest req;
    req.slavePosition = row[0].get<uint16_t>();
    req.index = row[1].get<uint16_t>();
    req.subindex = row[2].get<uint8_t>();
    req.value = std::move(*value);
    requests.push_back(std::move(req));
  }
  return requests;
}

// Parses one direction ("outputs"/"inputs") of a PDO-mapping body into mapping objects. Each object
// is {"pdoIndex": N, "entries": [{"index": N, "subindex": N, "bitLength": N}, ...]}. Numbers are
// decimal unsigned and range-checked to their CoE field widths (pdoIndex/index 16-bit, subindex and
// bitLength 8-bit). Returns the first shape/range problem as an error.
std::expected<std::vector<mm::node::PdoMappingObject>, std::string> parsePdoMappingObjects(
    const nlohmann::json& arr, const std::string& dir) {
  if (!arr.is_array()) {
    return std::unexpected<std::string>("\"" + dir + "\" must be an array of mapping objects");
  }
  std::vector<mm::node::PdoMappingObject> objects;
  objects.reserve(arr.size());
  for (const auto& o : arr) {
    if (!o.is_object() || !o.contains("pdoIndex") || !o.contains("entries")) {
      return std::unexpected<std::string>("each " + dir +
                                          " object must have \"pdoIndex\" and \"entries\"");
    }
    const auto& idx = o.at("pdoIndex");
    const auto& entries = o.at("entries");
    if (!idx.is_number_unsigned() || idx.get<uint64_t>() > 0xFFFF) {
      return std::unexpected<std::string>(dir + " pdoIndex must be a 16-bit unsigned integer");
    }
    if (!entries.is_array()) {
      return std::unexpected<std::string>(dir + " entries must be an array");
    }
    mm::node::PdoMappingObject obj;
    obj.pdoIndex = idx.get<uint16_t>();
    obj.entries.reserve(entries.size());
    for (const auto& e : entries) {
      if (!e.is_object() || !e.contains("index") || !e.contains("subindex") ||
          !e.contains("bitLength")) {
        return std::unexpected<std::string>(
            "each " + dir + " entry must have \"index\", \"subindex\", and \"bitLength\"");
      }
      const auto& ei = e.at("index");
      const auto& es = e.at("subindex");
      const auto& eb = e.at("bitLength");
      if (!ei.is_number_unsigned() || ei.get<uint64_t>() > 0xFFFF || !es.is_number_unsigned() ||
          es.get<uint64_t>() > 0xFF || !eb.is_number_unsigned() || eb.get<uint64_t>() > 0xFF) {
        return std::unexpected<std::string>(
            dir + " entry fields out of range (index 0-65535, subindex 0-255, bitLength 0-255)");
      }
      obj.entries.push_back({.index = ei.get<uint16_t>(),
                             .subindex = es.get<uint8_t>(),
                             .bitLength = eb.get<uint8_t>()});
    }
    objects.push_back(std::move(obj));
  }
  return objects;
}

// Parses a PUT /api/devices/:slavePosition/pdo-mapping body into a PdoMapping. The body is
// {"outputs": [...], "inputs": [...]}; both directions must be present (an empty array clears that
// sync manager's assignment, so an omitted key would be an accidental wipe and is rejected). Deeper
// validation (PRE-OP, per-object counts, read-back) is the node layer's job.
std::expected<mm::node::PdoMapping, std::string> parseDevicePdoMapping(const nlohmann::json& body) {
  if (!body.is_object() || !body.contains("outputs") || !body.contains("inputs")) {
    return std::unexpected<std::string>(
        "expected an object with \"outputs\" and \"inputs\" arrays");
  }
  auto outputs = parsePdoMappingObjects(body.at("outputs"), "outputs");
  if (!outputs) {
    return std::unexpected(outputs.error());
  }
  auto inputs = parsePdoMappingObjects(body.at("inputs"), "inputs");
  if (!inputs) {
    return std::unexpected(inputs.error());
  }
  mm::node::PdoMapping mapping;
  mapping.outputs = std::move(*outputs);
  mapping.inputs = std::move(*inputs);
  return mapping;
}

// Serialises a process-data watchdog configuration. timeoutMs is the human unit the UI edits;
// timeoutNs and the raw divider/ticks expose the exact programmed value (the timeout is rounded
// to the device's watchdog tick base).
nlohmann::json watchdogJson(uint16_t slavePosition, const mm::comm::ProcessDataWatchdogConfig& wd) {
  return {{"slavePosition", slavePosition},
          {"enabled", wd.enabled},
          {"running", wd.running},
          {"timeoutNs", wd.timeout.count()},
          {"timeoutMs", static_cast<double>(wd.timeout.count()) / 1e6},
          {"divider", wd.divider},
          {"ticks", wd.ticks}};
}

// Formats a system_clock time_point as an ISO 8601 UTC timestamp (e.g. "2026-08-01T00:00:00Z").
std::string toIso8601Utc(std::chrono::system_clock::time_point tp) {
  const std::time_t t = std::chrono::system_clock::to_time_t(tp);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

// Builds the GET /api/cert body from a parsed certificate and the path it was read from.
// daysRemaining is whole days until notAfter (negative once expired); expiresSoon trips inside
// the kCertExpiringSoonDays window so the PWA can prompt the user to download a fresh release.
nlohmann::json certInfoJson(const mm::CertInfo& info, const std::string& path) {
  const auto now = std::chrono::system_clock::now();
  const auto daysRemaining =
      std::chrono::duration_cast<std::chrono::hours>(info.notAfter - now).count() / 24;
  const bool expired = now >= info.notAfter;
  auto chain = nlohmann::json::array();
  std::transform(info.chain.begin(), info.chain.end(), std::back_inserter(chain),
                 [](const auto& link) {
                   return nlohmann::json{{"subject", link.subject},
                                         {"issuer", link.issuer},
                                         {"organization", link.organization},
                                         {"issuerOrganization", link.issuerOrganization}};
                 });
  return {{"path", path},
          {"subject", info.subject},
          {"issuer", info.issuer},
          {"dnsNames", info.dnsNames},
          {"notBefore", toIso8601Utc(info.notBefore)},
          {"notAfter", toIso8601Utc(info.notAfter)},
          {"daysRemaining", daysRemaining},
          {"expired", expired},
          {"expiresSoon", expired || daysRemaining < mm::kCertExpiringSoonDays},
          {"chain", chain}};
}

/// The path component of a `/api/user-cache/...` URL, relative to the cache root.
///
/// uWS hands the raw, still percent-encoded URL, so a file called `v5.6.6 (rev 2).xml` arrives as
/// `v5.6.6%20(rev%202).xml` — decoded here so the name on disk is the name the user chose. An
/// invalid escape is left verbatim rather than dropped: it then reaches @c UserCache::resolve as an
/// ordinary character, which either names a real file or fails cleanly. Decoding cannot introduce a
/// traversal that resolve() would miss — resolve() validates the *decoded* path, so a `%2e%2e`
/// spelling of `..` is rejected exactly like the literal one.
std::string userCacheRelPath(std::string_view url) {
  constexpr std::string_view kPrefix = "/api/user-cache/";
  std::string_view encoded = url.starts_with(kPrefix) ? url.substr(kPrefix.size()) : url;
  std::string decoded;
  decoded.reserve(encoded.size());
  for (size_t i = 0; i < encoded.size(); ++i) {
    uint32_t byte = 0;
    const char* first = encoded.data() + i + 1;
    const char* last = encoded.data() + i + 3;
    // Both hex digits must be consumed: from_chars would happily read `%4Z` as 4 and leave the
    // `Z`, which would silently drop a character the user typed.
    if (encoded[i] == '%' && i + 2 < encoded.size() &&
        std::from_chars(first, last, byte, 16) == std::from_chars_result{last, std::errc()}) {
      decoded.push_back(static_cast<char>(byte));
      i += 2;
    } else {
      decoded.push_back(encoded[i]);
    }
  }
  return decoded;
}

/// Writes the headers that make a user-cache download inert in a browser, and returns @p res for
/// chaining.
///
/// The cache serves bytes the user themselves uploaded, from the API's own origin — so a rendered
/// response is stored XSS against the origin that controls the drives, and one that CORS does not
/// help with (a script *on* this origin is same-origin by definition). An earlier draft guessed a
/// content type from the extension so an ESI would display inline; that is exactly the hole, since
/// a browser executes script in an `application/xml` document (an XSLT processing instruction, or
/// inline XHTML). Four headers close it, and none of them cost the real consumers anything — the
/// console and the SDK both read the bytes, never render them:
///
/// - `application/octet-stream` unconditionally — no extension is trusted to name a type.
/// - `Content-Disposition: attachment` — download, never render. Deliberately with **no**
///   `filename`: the path is user-controlled, and a quote or newline in a header value is response
///   splitting. The client names the saved file itself.
/// - `X-Content-Type-Options: nosniff` — stops a browser from second-guessing the type above.
/// - A `default-src 'none'; sandbox` CSP — belt and braces if the response is rendered anyway.
template <typename Res>
Res* setUserCacheDownloadHeaders(Res* res, std::string_view corsOrigin) {
  return mm::api::setCorsOrigin(res, corsOrigin)
      ->writeHeader("Content-Type", "application/octet-stream")
      ->writeHeader("Content-Disposition", "attachment")
      ->writeHeader("X-Content-Type-Options", "nosniff")
      ->writeHeader("Content-Security-Policy", "default-src 'none'; sandbox");
}

}  // namespace

HttpServer::HttpServer(Config config, mm::node::DeviceManager& deviceManager,
                       mm::node::MonitoringManager& monitoringManager,
                       mm::node::ProcedureManager& procedureManager, mm::core::UserCache& userCache)
    : config_(std::move(config)),
      deviceManager_(deviceManager),
      monitoringManager_(monitoringManager),
      procedureManager_(procedureManager),
      userCache_(userCache) {}

HttpServer::~HttpServer() {
  stop();
  // stop() returns early when running_ was already false (listen failed), leaving thread_ joinable.
  if (thread_.joinable()) {
    thread_.join();
  }
}

void HttpServer::addRoutes(mm::api::RegisterRoutesFn module) {
  if (running_.load()) {
    spdlog::warn("HttpServer::addRoutes called after start(); the module will not be registered");
    return;
  }
  routeModules_.push_back(std::move(module));
}

bool HttpServer::start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    return true;  // already running
  }
  thread_ = std::thread([this]() { run(); });
  // Block until the listen callback fires on the loop thread, so start() reports the bind outcome
  // synchronously to the composition root (which treats a failure as fatal).
  return listenResult_.get_future().get();
}

void HttpServer::stop() {
  bool expected = true;
  if (!running_.compare_exchange_strong(expected, false)) {
    return;
  }

  // Before the loop: a worker finishing its device work defers the response onto the loop, so the
  // loop must still be there to receive it. purge() drops what has not started (those responses die
  // with the connections the loop is about to close anyway, and waiting out a queue of mailbox
  // timeouts would make shutdown crawl); wait() then blocks until nothing is still running, which
  // is the guarantee that matters — no worker is left holding a response or about to defer onto a
  // loop that has gone.
  pool_.purge();
  pool_.wait();

  if (auto* loop = loop_.load()) {
    // App::close() closes the listen socket *and* every regular socket (incl. idle HTTP keep-alive
    // connections); each lands on the loop's closed_head queue and is freed on the next loop_post,
    // dropping num_polls so us_loop_run() exits. Manually closing only the listen socket would
    // leave keep-alive connections alive, blocking the loop until each hits its idle timeout —
    // hanging shutdown for minutes after a client has talked to the API.
    loop->defer([this]() {
      if (auto* app = app_.load()) {
        app->close();
      }
    });
  }

  if (thread_.joinable()) {
    thread_.join();
  }
}

void HttpServer::run() {
  loop_.store(uWS::Loop::get());

  uWS::SSLApp app{uWS::SocketContextOptions{
      .key_file_name = config_.keyFile.c_str(),
      .cert_file_name = config_.certFile.c_str(),
  }};
  app_.store(&app);

  // Routes registered through the Router run their handlers on the worker pool rather than on this
  // loop, so a device operation that takes seconds cannot stall every other request. New routes go
  // here; the chained registration below is the pre-Router shape and is being migrated onto this.
  mm::api::Router router(app, loop_.load(), pool_, config_.corsOrigin);

  // The Console polls this continuously for the sidebar's AL-state badge, and it takes the driver's
  // control-plane lock — so on the loop thread it stalled every request behind it for the length of
  // a firmware transfer.
  router.get("/api/devices/state", [this](const mm::api::Request& req) -> mm::api::Response {
    auto positions = parsePositions(req);
    if (!positions) {
      return mm::api::badRequest(positions.error());
    }
    return mm::api::timed([this, &positions] { return deviceManager_.deviceStates(*positions); });
  });

  // The poll a client watches a running procedure through. It takes the bus lock (shared) to decide
  // applicability, so a procedure holding that lock exclusively for a state transition would
  // otherwise freeze the very page reporting its progress.
  router.get("/api/devices/:slavePosition/procedures",
             [this](const mm::api::Request& req) -> mm::api::Response {
               auto position = req.parameterAs<uint16_t>("slavePosition");
               if (!position) {
                 return mm::api::badRequest("slavePosition must be a number");
               }
               auto listings =
                   mm::node::listProcedures(deviceManager_, procedureManager_, *position);
               if (!listings) {
                 return mm::api::error(std::string(procedureErrorStatus(listings.error().kind)),
                                       listings.error().message);
               }
               return mm::api::json(nlohmann::json(*listings));
             });

  // Register the built-in routes as a statement on `app` (not moved), then hand `app` to any
  // registered plug-in modules so they can add their own routes, then finish with the CORS
  // preflight, the catch-all 404, and listen(). All three phases operate on the same `app` object.
  app.get("/",
          [](auto* res, auto* /*req*/) {
            res->writeHeader("Content-Type", "text/html; charset=utf-8")
                ->end(
                    "<!DOCTYPE html><html><head><title>Motion Master API</title></head>"
                    "<body><h1>Motion Master API</h1>"
                    "<p>This is the Motion Master local API server. "
                    "For documentation and the web interface, visit "
                    "<a href=\"https://synapticon.github.io/motion-master/\">"
                    "https://synapticon.github.io/motion-master/</a>.</p>"
                    "<ul>"
                    "<li><a href=\"/api/swagger.yml\">API specification (swagger.yml)</a></li>"
                    "<li><a href=\"/api/log\">Log</a></li>"
                    "<li><a href=\"/api/registers\">ESC registers</a></li>"
                    "</ul>"
                    "</body></html>");
          })
      .get("/api/swagger.yml",
           [this](auto* res, auto* /*req*/) {
             // Embedded at build time (swagger_spec.h). text/yaml renders inline in a browser;
             // a client fetches it to resolve the running server's exact API contract.
             setCorsOrigin(res, config_.corsOrigin)
                 ->writeHeader("Content-Type", "text/yaml; charset=utf-8")
                 ->writeHeader("Content-Disposition", "inline")
                 ->end(mm::kSwaggerYml);
           })
      .get("/api/adapters",
           [this](auto* res, auto* /*req*/) {
             nlohmann::json arr = mm::comm::enumerateNetworkAdapters();
             sendJson(res, config_.corsOrigin, arr);
           })
      .get("/api/version",
           [this](auto* res, auto* /*req*/) {
             sendJson(res, config_.corsOrigin, nlohmann::json{{"version", config_.version}});
           })
      .get("/api/config",
           [this](auto* res, auto* /*req*/) {
             // Pre-serialized at startup; parse back so sendJson sets the JSON content type +
             // CORS.
             sendJson(res, config_.corsOrigin,
                      config_.startedConfig.empty() ? nlohmann::json::object()
                                                    : nlohmann::json::parse(config_.startedConfig));
           })
      .get("/api/system-info",
           [this](auto* res, auto* /*req*/) {
             sendJson(res, config_.corsOrigin, nlohmann::json(mm::core::collectSystemInfo()));
           })
      .get("/api/cert",
           [this](auto* res, auto* /*req*/) {
             auto info = mm::readCertInfo(config_.certFile);
             if (!info) {
               sendError(res, "500 Internal Server Error", config_.corsOrigin, info.error());
               return;
             }
             sendJson(res, config_.corsOrigin, certInfoJson(*info, config_.certFile));
           })
      .post("/api/cert/refresh",
            [this](auto* res, auto* /*req*/) {
              if (!config_.refreshCert) {
                sendError(res, "501 Not Implemented", config_.corsOrigin,
                          "certificate refresh is not configured");
                return;
              }
              // Synchronous network fetch on the HTTP loop thread: it briefly blocks other HTTP
              // requests (the WebSocket runs on a separate loop, so it is unaffected), accepted
              // because refresh is a rare, manual action and the fetch is ~1s. To make it fully
              // non-blocking, move it to a background thread and respond via loop->defer.
              if (auto r = config_.refreshCert(); !r) {
                sendError(res, "500 Internal Server Error", config_.corsOrigin, r.error());
                return;
              }
              // The fresh cert is on disk, but uSockets bound the old one at listen — restart
              // to apply. Report the newly installed cert's details plus the restart hint.
              auto info = mm::readCertInfo(config_.certFile);
              nlohmann::json body =
                  info ? certInfoJson(*info, config_.certFile) : nlohmann::json::object();
              body["restartRequired"] = true;
              sendJson(res, config_.corsOrigin, body);
            })
      .get("/api/log",
           [this](auto* res, auto* /*req*/) {
             auto lines = config_.getLog ? config_.getLog() : std::vector<std::string>{};
             std::string body;
             for (const auto& line : lines) {
               body += line;
               body += '\n';
             }
             sendBytes(res, config_.corsOrigin, "text/plain; charset=utf-8", body);
           })
      .get("/api/meta/esc-registers",
           [this](auto* res, auto* /*req*/) {
             sendJson(res, config_.corsOrigin, nlohmann::json(mm::comm::kEscRegisters));
           })
      .get("/api/meta/al-status-codes",
           [this](auto* res, auto* /*req*/) {
             sendJson(res, config_.corsOrigin, nlohmann::json(mm::comm::kAlStatusCodes));
           })
      .get("/api/meta/foe-error-codes",
           [this](auto* res, auto* /*req*/) {
             sendJson(res, config_.corsOrigin, nlohmann::json(mm::comm::kFoeErrorCodes));
           })
      .get("/api/meta/sdo-abort-codes",
           [this](auto* res, auto* /*req*/) {
             sendJson(res, config_.corsOrigin, nlohmann::json(mm::comm::kSdoAbortCodes));
           })
      .get("/api/meta/mailbox-error-codes",
           [this](auto* res, auto* /*req*/) {
             sendJson(res, config_.corsOrigin, nlohmann::json(mm::comm::kMailboxErrorCodes));
           })
      .get("/api/devices/diagnostics",
           [this](auto* res, auto* req) {
             auto positions = parsePositions(res, req, config_.corsOrigin);
             if (!positions) {
               return;  // parsePositions already wrote the 400 response
             }
             sendTimedJson(res, config_.corsOrigin, "500 Internal Server Error",
                           [&] { return deviceManager_.deviceDiagnostics(*positions); });
           })
      .get("/api/dc-sync",
           [this](auto* res, auto* req) {
             auto positions = parsePositions(res, req, config_.corsOrigin);
             if (!positions) {
               return;  // parsePositions already wrote the 400 response
             }
             sendTimedJson(res, config_.corsOrigin, "500 Internal Server Error",
                           [&] { return deviceManager_.dcSync(*positions); });
           })
      .get("/api/devices",
           [this](auto* res, auto* /*req*/) {
             sendJson(res, config_.corsOrigin, nlohmann::json(deviceManager_));
           })
      .get("/api/process-image",
           [this](auto* res, auto* /*req*/) {
             sendJson(res, config_.corsOrigin, nlohmann::json(deviceManager_.processImageInfo()));
           })
      .get("/api/game-loop",
           [this](auto* res, auto* /*req*/) {
             if (!config_.getGameLoopHealth) {
               sendError(res, "501 Not Implemented", config_.corsOrigin,
                         "game-loop health is not configured");
               return;
             }
             sendJson(res, config_.corsOrigin, nlohmann::json(config_.getGameLoopHealth()));
           })
      .put("/api/game-loop",
           [this](auto* res, auto* /*req*/) {
             auto aborted = std::make_shared<bool>(false);
             auto body = std::make_shared<std::string>();
             res->onAborted([aborted]() { *aborted = true; });
             res->onData([this, res, body, aborted](std::string_view chunk, bool last) {
               body->append(chunk);
               if (!last) return;
               if (*aborted) return;
               if (!config_.setGameLoopPeriod) {
                 sendError(res, "501 Not Implemented", config_.corsOrigin,
                           "game-loop control is not configured");
                 return;
               }
               uint32_t periodUs = 0;
               try {
                 nlohmann::json j = nlohmann::json::parse(*body);
                 periodUs = j.at("periodUs").get<uint32_t>();
               } catch (const nlohmann::json::exception& e) {
                 sendError(res, "400 Bad Request", config_.corsOrigin, e.what());
                 return;
               }
               // Validation lives in the callback (main.cc) — the HTTP layer just forwards. A
               // rejected value (e.g. 0) comes back as an error string.
               auto r = config_.setGameLoopPeriod(periodUs);
               if (!r) {
                 sendError(res, "400 Bad Request", config_.corsOrigin, r.error());
                 return;
               }
               sendJson(res, config_.corsOrigin, nlohmann::json(config_.getGameLoopHealth()));
             });
           })
      .get("/api/bus-config",
           [this](auto* res, auto* /*req*/) {
             sendJson(res, config_.corsOrigin, nlohmann::json(deviceManager_.busConfig()));
           })
      .get("/api/devices/:slavePosition",
           [this](auto* res, auto* req) {
             uint16_t pos{};
             auto param = req->getParameter("slavePosition");
             auto [ptr, ec] = std::from_chars(param.data(), param.data() + param.size(), pos);
             if (ec != std::errc() || ptr != param.data() + param.size()) {
               sendStatus(res, "400 Bad Request", config_.corsOrigin);
               return;
             }
             const auto* device = deviceManager_.findDevice(pos);
             if (!device) {
               sendStatus(res, "404 Not Found", config_.corsOrigin);
               return;
             }
             sendJson(res, config_.corsOrigin, nlohmann::json(*device));
           })
      .get("/api/devices/:slavePosition/registers/:address",
           [this](auto* res, auto* req) {
             uint16_t pos{};
             auto posParam = req->getParameter("slavePosition");
             auto [p1, ec1] =
                 std::from_chars(posParam.data(), posParam.data() + posParam.size(), pos);
             if (ec1 != std::errc() || p1 != posParam.data() + posParam.size()) {
               sendStatus(res, "400 Bad Request", config_.corsOrigin);
               return;
             }
             uint16_t address{};
             auto addrParam = req->getParameter("address");
             auto [p2, ec2] =
                 std::from_chars(addrParam.data(), addrParam.data() + addrParam.size(), address);
             if (ec2 != std::errc() || p2 != addrParam.data() + addrParam.size()) {
               sendStatus(res, "400 Bad Request", config_.corsOrigin);
               return;
             }
             uint16_t length{};
             auto lenParam = req->getQuery("length");
             auto [p3, ec3] =
                 std::from_chars(lenParam.data(), lenParam.data() + lenParam.size(), length);
             if (ec3 != std::errc() || p3 != lenParam.data() + lenParam.size() || length == 0) {
               sendStatus(res, "400 Bad Request", config_.corsOrigin);
               return;
             }
             const auto* device = deviceManager_.findDevice(pos);
             if (!device) {
               sendStatus(res, "404 Not Found", config_.corsOrigin);
               return;
             }
             std::vector<uint8_t> buf(length);
             sendTimedJson(res, config_.corsOrigin, "500 Internal Server Error",
                           [&]() -> std::expected<nlohmann::json, std::string> {
                             if (auto r = device->readRegister(address, buf); !r) {
                               return std::unexpected(r.error());
                             }
                             return nlohmann::json{{"data", buf}};
                           });
           })
      .get("/api/devices/:slavePosition/sii",
           [this](auto* res, auto* req) {
             uint16_t pos{};
             auto posParam = req->getParameter("slavePosition");
             auto [p, ec] =
                 std::from_chars(posParam.data(), posParam.data() + posParam.size(), pos);
             if (ec != std::errc() || p != posParam.data() + posParam.size()) {
               sendStatus(res, "400 Bad Request", config_.corsOrigin);
               return;
             }
             // Content negotiation: the raw EEPROM image is returned only when the client asks
             // for it via Accept: application/octet-stream. Otherwise (Accept:
             // application/json, */*, or absent) the parsed SII structure is returned — the
             // default.
             const bool wantRaw = req->getHeader("accept").find("application/octet-stream") !=
                                  std::string_view::npos;
             const auto* device = deviceManager_.findDevice(pos);
             if (!device) {
               sendStatus(res, "404 Not Found", config_.corsOrigin);
               return;
             }
             // Only the EEPROM read touches the wire; parseSii() is local CPU. Time the read and
             // carry X-Wire-Us on every outcome (raw bytes, parsed JSON, or error).
             const auto t0 = std::chrono::steady_clock::now();
             auto raw = device->readSii();
             const auto wireUs = std::chrono::duration_cast<std::chrono::microseconds>(
                 std::chrono::steady_clock::now() - t0);
             if (!raw) {
               sendError(res, "500 Internal Server Error", config_.corsOrigin, raw.error(), wireUs);
               return;
             }
             if (wantRaw) {
               setWireTime(res, wireUs);
               sendBytes(res, config_.corsOrigin, "application/octet-stream",
                         std::string_view{reinterpret_cast<const char*>(raw->data()), raw->size()});
               return;
             }
             auto parsed = mm::comm::parseSii(*raw);
             if (!parsed) {
               sendError(res, "500 Internal Server Error", config_.corsOrigin, parsed.error(),
                         wireUs);
               return;
             }
             setWireTime(res, wireUs);
             sendJson(res, config_.corsOrigin, nlohmann::json(*parsed));
           })
      .put("/api/devices/:slavePosition/sii",
           [this](auto* res, auto* req) {
             uint16_t pos{};
             auto posParam = req->getParameter("slavePosition");
             auto [p, ec] =
                 std::from_chars(posParam.data(), posParam.data() + posParam.size(), pos);
             bool posOk = (ec == std::errc() && p == posParam.data() + posParam.size());
             auto aborted = std::make_shared<bool>(false);
             auto body = std::make_shared<std::string>();
             res->onAborted([aborted]() { *aborted = true; });
             res->onData([this, res, body, aborted, pos, posOk](std::string_view chunk, bool last) {
               body->append(chunk);
               if (!last) {
                 return;
               }
               if (*aborted) {
                 return;
               }
               if (!posOk) {
                 sendStatus(res, "400 Bad Request", config_.corsOrigin);
                 return;
               }
               std::span<const uint8_t> data{reinterpret_cast<const uint8_t*>(body->data()),
                                             body->size()};
               // Reject an image that does not parse before touching the EEPROM — a guard
               // against bricking the device by writing garbage.
               if (auto parsed = mm::comm::parseSii(data); !parsed) {
                 sendError(res, "400 Bad Request", config_.corsOrigin,
                           std::string("not a valid SII image: ") + parsed.error());
                 return;
               }
               const auto* device = deviceManager_.findDevice(pos);
               if (!device) {
                 sendStatus(res, "404 Not Found", config_.corsOrigin);
                 return;
               }
               sendTimedJson(res, config_.corsOrigin, "500 Internal Server Error",
                             [&]() -> std::expected<nlohmann::json, std::string> {
                               if (auto r = device->writeSii(data); !r) {
                                 return std::unexpected(r.error());
                               }
                               return nlohmann::json{{"ok", true}};
                             });
             });
           })
      .post("/api/sii/parse",
            [this](auto* res, auto* /*req*/) {
              // Bus-independent utility: parse a raw SII image uploaded in the request body
              // (e.g. a previously downloaded .bin) and return the decoded structure. No device
              // involved — the Tools SII page uses this to view EEPROM files offline through
              // the same parser the device read path uses.
              auto aborted = std::make_shared<bool>(false);
              auto body = std::make_shared<std::string>();
              res->onAborted([aborted]() { *aborted = true; });
              res->onData([this, res, body, aborted](std::string_view chunk, bool last) {
                body->append(chunk);
                if (!last) {
                  return;
                }
                if (*aborted) {
                  return;
                }
                std::span<const uint8_t> bytes{reinterpret_cast<const uint8_t*>(body->data()),
                                               body->size()};
                auto parsed = mm::comm::parseSii(bytes);
                if (!parsed) {
                  sendError(res, "400 Bad Request", config_.corsOrigin, parsed.error());
                  return;
                }
                sendJson(res, config_.corsOrigin, nlohmann::json(*parsed));
              });
            })
      .get("/api/firmware-package-name",
           [this](auto* res, auto* req) {
             // Bus-independent utility alongside /api/sii/parse and /api/esi/parse: decode a
             // SOMANET firmware package filename into its five fields (Hardware description
             // specification §3.4.2) and, where the descriptor is the numeric kind, its product id,
             // version, key and fieldbus. A GET with a query rather than a POST with a body —
             // unlike its two siblings there is no file to upload, only a short string, so this is
             // a plain cacheable read that is trivial to curl.
             //
             // The same decoder the firmware installation procedure uses to decide whether a
             // package can be cached, so what this reports is what that will do.
             const std::string filename{req->getQuery("filename")};
             if (filename.empty()) {
               sendError(res, "400 Bad Request", config_.corsOrigin,
                         "a 'filename' query parameter is required");
               return;
             }
             auto name = mm::node::parseFirmwarePackageName(filename);
             if (!name) {
               // The grammar it missed, not a bare rejection: this is a tool whose whole job is to
               // say what a name means, so "why not" is the useful half of a negative answer.
               sendError(res, "400 Bad Request", config_.corsOrigin, name.error());
               return;
             }
             sendJson(res, config_.corsOrigin, nlohmann::json(*name));
           })
      .post("/api/esi/parse",
            [this](auto* res, auto* req) {
              // Bus-independent utility, the ESI counterpart of /api/sii/parse: decode a vendor's
              // EtherCAT Slave Information XML uploaded in the request body. No device involved —
              // the Tools page uses this to inspect an ESI with no hardware present, which is the
              // only way to see descriptions, enum labels, units and min/max bounds, none of which
              // the CoE SDO-Information service reports.
              //
              // The response carries every device with its own assembled entry table. That is
              // affordable because object-level annotation is stored once, on subindex 0, rather
              // than repeated onto every subindex — repeating it made one device's JSON 4.7 MB,
              // 83% of it duplicated description HTML. The optional modules= query narrows the
              // merge where a slot offers a choice of modules.
              //
              // req is only valid synchronously — capture the query before onData.
              mm::etg::EsiParseRequest request;
              std::string selectorError;
              if (const auto q = req->getQuery("modules"); !q.empty()) {
                auto idents = mm::etg::parseIdentList(q);
                if (idents) {
                  request.moduleIdents = std::move(*idents);
                } else {
                  selectorError = std::format("modules: {}", idents.error());
                }
              }

              auto aborted = std::make_shared<bool>(false);
              auto body = std::make_shared<std::string>();
              res->onAborted([aborted]() { *aborted = true; });
              res->onData(
                  [this, res, body, aborted, request = std::move(request),
                   selectorError = std::move(selectorError)](std::string_view chunk, bool last) {
                    body->append(chunk);
                    if (!last || *aborted) {
                      return;
                    }
                    if (!selectorError.empty()) {
                      sendError(res, "400 Bad Request", config_.corsOrigin, selectorError);
                      return;
                    }
                    // Timed like a device operation, through the same X-Wire-Us channel — the
                    // figure is CPU rather than wire time here (parse + assemble every device's
                    // dictionary), which for a megabyte-scale ESI is the part worth watching.
                    // JSON serialisation stays outside it, as it does everywhere else.
                    sendTimedJson(res, config_.corsOrigin, "400 Bad Request",
                                  [&]() { return mm::etg::buildEsiResponse(*body, request); });
                  });
            })
      .post("/api/devices/:slavePosition/registers/:address",
            [this](auto* res, auto* req) {
              uint16_t pos{};
              auto posParam = req->getParameter("slavePosition");
              auto [p1, ec1] =
                  std::from_chars(posParam.data(), posParam.data() + posParam.size(), pos);
              bool posOk = (ec1 == std::errc() && p1 == posParam.data() + posParam.size());
              uint16_t address{};
              auto addrParam = req->getParameter("address");
              auto [p2, ec2] =
                  std::from_chars(addrParam.data(), addrParam.data() + addrParam.size(), address);
              bool addrOk = (ec2 == std::errc() && p2 == addrParam.data() + addrParam.size());
              auto aborted = std::make_shared<bool>(false);
              auto body = std::make_shared<std::string>();
              res->onAborted([aborted]() { *aborted = true; });
              res->onData([this, res, body, aborted, pos, posOk, address, addrOk](
                              std::string_view chunk, bool last) {
                body->append(chunk);
                if (!last) return;
                if (*aborted) return;
                if (!posOk || !addrOk) {
                  sendStatus(res, "400 Bad Request", config_.corsOrigin);
                  return;
                }
                std::vector<uint8_t> data;
                try {
                  nlohmann::json j = nlohmann::json::parse(*body);
                  data = j.at("data").get<std::vector<uint8_t>>();
                } catch (const nlohmann::json::exception& e) {
                  sendError(res, "400 Bad Request", config_.corsOrigin, e.what());
                  return;
                }
                const auto* device = deviceManager_.findDevice(pos);
                if (!device) {
                  sendStatus(res, "404 Not Found", config_.corsOrigin);
                  return;
                }
                sendTimedJson(res, config_.corsOrigin, "500 Internal Server Error",
                              [&]() -> std::expected<nlohmann::json, std::string> {
                                if (auto r = device->writeRegister(address, data); !r) {
                                  return std::unexpected(r.error());
                                }
                                return nlohmann::json{{"ok", true}};
                              });
              });
            })
      .get("/api/devices/:slavePosition/watchdog",
           [this](auto* res, auto* req) {
             uint16_t pos{};
             auto posParam = req->getParameter("slavePosition");
             auto [p1, ec1] =
                 std::from_chars(posParam.data(), posParam.data() + posParam.size(), pos);
             if (ec1 != std::errc() || p1 != posParam.data() + posParam.size()) {
               sendStatus(res, "400 Bad Request", config_.corsOrigin);
               return;
             }
             if (!deviceManager_.findDevice(pos)) {
               sendStatus(res, "404 Not Found", config_.corsOrigin);
               return;
             }
             sendTimedJson(res, config_.corsOrigin, "500 Internal Server Error",
                           [&]() -> std::expected<nlohmann::json, std::string> {
                             auto r = deviceManager_.processDataWatchdog(pos);
                             if (!r) {
                               return std::unexpected(r.error());
                             }
                             return watchdogJson(pos, *r);
                           });
           })
      .put("/api/devices/:slavePosition/watchdog",
           [this](auto* res, auto* req) {
             uint16_t pos{};
             auto posParam = req->getParameter("slavePosition");
             auto [p1, ec1] =
                 std::from_chars(posParam.data(), posParam.data() + posParam.size(), pos);
             bool posOk = (ec1 == std::errc() && p1 == posParam.data() + posParam.size());
             auto aborted = std::make_shared<bool>(false);
             auto body = std::make_shared<std::string>();
             res->onAborted([aborted]() { *aborted = true; });
             res->onData([this, res, body, aborted, pos, posOk](std::string_view chunk, bool last) {
               body->append(chunk);
               if (!last) return;
               if (*aborted) return;
               if (!posOk) {
                 sendStatus(res, "400 Bad Request", config_.corsOrigin);
                 return;
               }
               double timeoutMs = 0;
               try {
                 nlohmann::json j = nlohmann::json::parse(*body);
                 timeoutMs = j.at("timeoutMs").get<double>();
               } catch (const nlohmann::json::exception& e) {
                 sendError(res, "400 Bad Request", config_.corsOrigin, e.what());
                 return;
               }
               if (timeoutMs < 0) {
                 sendError(res, "400 Bad Request", config_.corsOrigin,
                           "timeoutMs must not be negative");
                 return;
               }
               if (!deviceManager_.findDevice(pos)) {
                 sendStatus(res, "404 Not Found", config_.corsOrigin);
                 return;
               }
               auto ns = std::chrono::nanoseconds(static_cast<int64_t>(timeoutMs * 1e6 + 0.5));
               sendTimedJson(res, config_.corsOrigin, "500 Internal Server Error",
                             [&]() -> std::expected<nlohmann::json, std::string> {
                               auto r = deviceManager_.setProcessDataWatchdog(pos, ns);
                               if (!r) {
                                 return std::unexpected(r.error());
                               }
                               return watchdogJson(pos, *r);
                             });
             });
           })
      .get("/api/devices/:slavePosition/pdo-mapping",
           [this](auto* res, auto* req) {
             uint16_t pos{};
             auto posParam = req->getParameter("slavePosition");
             auto [p1, ec1] =
                 std::from_chars(posParam.data(), posParam.data() + posParam.size(), pos);
             if (ec1 != std::errc() || p1 != posParam.data() + posParam.size()) {
               sendStatus(res, "400 Bad Request", config_.corsOrigin);
               return;
             }
             if (!deviceManager_.findDevice(pos)) {
               sendStatus(res, "404 Not Found", config_.corsOrigin);
               return;
             }
             // Reads fresh over SDO, grouped by mapping object; requires the device's mailbox to be
             // active (PRE-OP/SAFE-OP/OP), so a device in INIT/BOOT is a 409.
             sendTimedJson(res, config_.corsOrigin, "409 Conflict",
                           [&] { return deviceManager_.readDevicePdoMapping(pos); });
           })
      .put("/api/devices/:slavePosition/pdo-mapping",
           [this](auto* res, auto* req) {
             uint16_t pos{};
             auto posParam = req->getParameter("slavePosition");
             auto [p1, ec1] =
                 std::from_chars(posParam.data(), posParam.data() + posParam.size(), pos);
             bool posOk = (ec1 == std::errc() && p1 == posParam.data() + posParam.size());
             auto aborted = std::make_shared<bool>(false);
             auto body = std::make_shared<std::string>();
             res->onAborted([aborted]() { *aborted = true; });
             res->onData([this, res, body, aborted, pos, posOk](std::string_view chunk, bool last) {
               body->append(chunk);
               if (!last) {
                 return;
               }
               if (*aborted) {
                 return;
               }
               if (!posOk) {
                 sendStatus(res, "400 Bad Request", config_.corsOrigin);
                 return;
               }
               mm::node::PdoMapping mapping;
               try {
                 auto parsed = parseDevicePdoMapping(nlohmann::json::parse(*body));
                 if (!parsed) {
                   sendError(res, "400 Bad Request", config_.corsOrigin, parsed.error());
                   return;
                 }
                 mapping = std::move(*parsed);
               } catch (const nlohmann::json::exception& e) {
                 sendError(res, "400 Bad Request", config_.corsOrigin, e.what());
                 return;
               }
               if (!deviceManager_.findDevice(pos)) {
                 sendStatus(res, "404 Not Found", config_.corsOrigin);
                 return;
               }
               // Time the whole operation (write + verify read-back), both over the wire, and carry
               // X-Wire-Us on every outcome via elapsed().
               const auto t0 = std::chrono::steady_clock::now();
               auto elapsed = [&] {
                 return std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now() - t0);
               };
               // A failed write is most often a device-state precondition (not in PRE-OP) or a
               // rejected/verify-mismatched mapping — a conflict with the device's current state,
               // not a server fault; report it as 409 with the node layer's detail.
               auto r = deviceManager_.writeDevicePdoMapping(pos, mapping);
               if (!r) {
                 sendError(res, "409 Conflict", config_.corsOrigin, r.error(), elapsed());
                 return;
               }
               // Echo the device's grouped read-back mapping (verified equal to the request), whose
               // entries carry the derived bitOffsets the request did not specify.
               auto readBack = deviceManager_.readDevicePdoMapping(pos);
               if (!readBack) {
                 sendError(res, "500 Internal Server Error", config_.corsOrigin, readBack.error(),
                           elapsed());
                 return;
               }
               setWireTime(res, elapsed());
               sendJson(res, config_.corsOrigin, nlohmann::json(*readBack));
             });
           })
      .get("/api/devices/:slavePosition/cia402",
           [this](auto* res, auto* req) {
             uint16_t pos{};
             auto posParam = req->getParameter("slavePosition");
             auto [p1, ec1] =
                 std::from_chars(posParam.data(), posParam.data() + posParam.size(), pos);
             if (ec1 != std::errc() || p1 != posParam.data() + posParam.size()) {
               sendStatus(res, "400 Bad Request", config_.corsOrigin);
               return;
             }
             if (!deviceManager_.findDevice(pos)) {
               sendStatus(res, "404 Not Found", config_.corsOrigin);
               return;
             }
             // A non-CiA402 device (or one whose OD is not yet enumerated) is a 409 — the node
             // layer's message says which; the client uses the device's isCia402 flag to avoid
             // asking in the first place.
             sendTimedJson(res, config_.corsOrigin, "409 Conflict",
                           [&] { return mm::node::cia402Status(deviceManager_, pos); });
           })
      .post(
          "/api/devices/:slavePosition/cia402/mode",
          [this](auto* res, auto* req) {
            uint16_t pos{};
            auto posParam = req->getParameter("slavePosition");
            auto [p1, ec1] =
                std::from_chars(posParam.data(), posParam.data() + posParam.size(), pos);
            bool posOk = (ec1 == std::errc() && p1 == posParam.data() + posParam.size());
            auto aborted = std::make_shared<bool>(false);
            auto body = std::make_shared<std::string>();
            res->onAborted([aborted]() { *aborted = true; });
            res->onData([this, res, body, aborted, pos, posOk](std::string_view chunk, bool last) {
              body->append(chunk);
              if (!last) {
                return;
              }
              if (*aborted) {
                return;
              }
              if (!posOk) {
                sendStatus(res, "400 Bad Request", config_.corsOrigin);
                return;
              }
              std::optional<mm::node::cia402::OperationMode> mode;
              try {
                nlohmann::json j = nlohmann::json::parse(*body);
                mode = mm::node::cia402::toOperationMode(j.at("mode").get<int>());
              } catch (const nlohmann::json::exception& e) {
                sendError(res, "400 Bad Request", config_.corsOrigin, e.what());
                return;
              }
              if (!mode) {
                sendError(res, "400 Bad Request", config_.corsOrigin,
                          "invalid mode: use 1 (PP), 3 (PV), 4 (PT), 6 (HM), 8 (CSP), 9 (CSV), "
                          "10 (CST), or 0 (NoMode)");
                return;
              }
              if (!deviceManager_.findDevice(pos)) {
                sendStatus(res, "404 Not Found", config_.corsOrigin);
                return;
              }
              auto r = mm::node::setCia402OperationMode(deviceManager_, pos, *mode);
              if (!r) {
                sendError(res, "409 Conflict", config_.corsOrigin, r.error());
                return;
              }
              sendJson(res, config_.corsOrigin, nlohmann::json(*r));
            });
          })
      .post(
          "/api/devices/:slavePosition/cia402/command",
          [this](auto* res, auto* req) {
            uint16_t pos{};
            auto posParam = req->getParameter("slavePosition");
            auto [p1, ec1] =
                std::from_chars(posParam.data(), posParam.data() + posParam.size(), pos);
            bool posOk = (ec1 == std::errc() && p1 == posParam.data() + posParam.size());
            auto aborted = std::make_shared<bool>(false);
            auto body = std::make_shared<std::string>();
            res->onAborted([aborted]() { *aborted = true; });
            res->onData([this, res, body, aborted, pos, posOk](std::string_view chunk, bool last) {
              body->append(chunk);
              if (!last) {
                return;
              }
              if (*aborted) {
                return;
              }
              if (!posOk) {
                sendStatus(res, "400 Bad Request", config_.corsOrigin);
                return;
              }
              std::optional<mm::node::Cia402Command> command;
              try {
                nlohmann::json j = nlohmann::json::parse(*body);
                command = mm::node::parseCia402Command(j.at("command").get<std::string>());
              } catch (const nlohmann::json::exception& e) {
                sendError(res, "400 Bad Request", config_.corsOrigin, e.what());
                return;
              }
              if (!command) {
                sendError(res, "400 Bad Request", config_.corsOrigin,
                          "invalid command: use enable, disable, quickStop, or faultReset");
                return;
              }
              if (!deviceManager_.findDevice(pos)) {
                sendStatus(res, "404 Not Found", config_.corsOrigin);
                return;
              }
              auto r = mm::node::runCia402Command(deviceManager_, pos, *command);
              if (!r) {
                sendError(res, "409 Conflict", config_.corsOrigin, r.error());
                return;
              }
              sendJson(res, config_.corsOrigin, nlohmann::json(*r));
            });
          })
      .post(
          "/api/devices/:slavePosition/cia402/target",
          [this](auto* res, auto* req) {
            uint16_t pos{};
            auto posParam = req->getParameter("slavePosition");
            auto [p1, ec1] =
                std::from_chars(posParam.data(), posParam.data() + posParam.size(), pos);
            bool posOk = (ec1 == std::errc() && p1 == posParam.data() + posParam.size());
            auto aborted = std::make_shared<bool>(false);
            auto body = std::make_shared<std::string>();
            res->onAborted([aborted]() { *aborted = true; });
            res->onData([this, res, body, aborted, pos, posOk](std::string_view chunk, bool last) {
              body->append(chunk);
              if (!last) {
                return;
              }
              if (*aborted) {
                return;
              }
              if (!posOk) {
                sendStatus(res, "400 Bad Request", config_.corsOrigin);
                return;
              }
              std::optional<mm::node::Cia402TargetKind> kind;
              int32_t value{};
              try {
                nlohmann::json j = nlohmann::json::parse(*body);
                kind = mm::node::parseCia402TargetKind(j.at("target").get<std::string>());
                // Signed: target position/velocity are INT32 and target torque INT16, all of
                // which take negative setpoints (reverse motion / regenerative torque).
                value = j.at("value").get<int32_t>();
              } catch (const nlohmann::json::exception& e) {
                sendError(res, "400 Bad Request", config_.corsOrigin, e.what());
                return;
              }
              if (!kind) {
                sendError(res, "400 Bad Request", config_.corsOrigin,
                          "invalid target: use position, velocity, or torque");
                return;
              }
              if (!deviceManager_.findDevice(pos)) {
                sendStatus(res, "404 Not Found", config_.corsOrigin);
                return;
              }
              auto r = mm::node::setCia402Target(deviceManager_, pos, *kind, value);
              if (!r) {
                sendError(res, "409 Conflict", config_.corsOrigin, r.error());
                return;
              }
              sendStatus(res, "204 No Content", config_.corsOrigin);
            });
          })
      // --- brake ----------------------------------------------------------------------------
      //
      // GET reports the brake, POST release/engage command it. Two verbs rather than one
      // PUT {released}: they are not symmetric operations — a release waits out the drive's pull
      // time because the firmware blocks motion until it expires, an engage waits only a short
      // settle — and both answer with the state read back, which is also how a caller learns that
      // nothing happened (a brake on release strategy 0 is not the firmware's to drive).
      .get("/api/devices/:slavePosition/brake",
           [this](auto* res, auto* req) {
             uint16_t pos{};
             auto posParam = req->getParameter("slavePosition");
             auto [p, ec] =
                 std::from_chars(posParam.data(), posParam.data() + posParam.size(), pos);
             if (ec != std::errc() || p != posParam.data() + posParam.size()) {
               sendStatus(res, "400 Bad Request", config_.corsOrigin);
               return;
             }
             if (!deviceManager_.findDevice(pos)) {
               sendStatus(res, "404 Not Found", config_.corsOrigin);
               return;
             }
             auto state = mm::node::brakeState(deviceManager_, pos);
             if (!state) {
               sendError(res, "409 Conflict", config_.corsOrigin, state.error());
               return;
             }
             sendJson(res, config_.corsOrigin, nlohmann::json(*state));
           })
      .post("/api/devices/:slavePosition/brake/release",
            [this](auto* res, auto* req) {
              handleBrakeCommand(res, req, deviceManager_, config_.corsOrigin, /*release=*/true);
            })
      .post("/api/devices/:slavePosition/brake/engage",
            [this](auto* res, auto* req) {
              handleBrakeCommand(res, req, deviceManager_, config_.corsOrigin, /*release=*/false);
            })
      // --- high resolution data
      // ---------------------------------------------------------------------
      //
      // Reads back what the `hrd-streaming` procedure recorded. Separate from the procedure, not
      // its final step, for two reasons: a recording is worth reading more than once, and a run's
      // snapshot — re-sent whole on every poll and retained until a rescan — is no place for ten
      // thousand samples. `data` says how to decode the files, and is required because nothing on
      // the drive records which signal was streamed into them.
      .get(
          "/api/devices/:slavePosition/hrd",
          [this](auto* res, auto* req) {
            uint16_t pos{};
            auto posParam = req->getParameter("slavePosition");
            auto [p, ec] = std::from_chars(posParam.data(), posParam.data() + posParam.size(), pos);
            if (ec != std::errc() || p != posParam.data() + posParam.size()) {
              sendStatus(res, "400 Bad Request", config_.corsOrigin);
              return;
            }
            auto dataParam = req->getQuery("data");
            auto data = mm::node::somanet::parseHrdData(dataParam);
            if (!data) {
              sendError(res, "400 Bad Request", config_.corsOrigin,
                        std::format("'data' must be one of {} or {}",
                                    mm::node::somanet::toString(
                                        mm::node::somanet::HrdData::kEncoderRawData),
                                    mm::node::somanet::toString(
                                        mm::node::somanet::HrdData::kSystemIdentificationData)));
              return;
            }
            // CSV for the spreadsheet-and-script half of the audience: a full recording is ten
            // thousand rows, which is a file to open rather than JSON to read. Same read and the
            // same decode, one rendering or the other — as on the SII endpoint above.
            const bool wantCsv =
                req->getHeader("accept").find("text/csv") != std::string_view::npos;
            if (!deviceManager_.findDevice(pos)) {
              sendStatus(res, "404 Not Found", config_.corsOrigin);
              return;
            }
            if (!wantCsv) {
              sendTimedJson(res, config_.corsOrigin, "409 Conflict",
                            [&] { return mm::node::readHrdRecording(deviceManager_, pos, *data); });
              return;
            }
            // Hand-timed rather than sendTimedJson, because the body is CSV: the wire figure still
            // rides the same X-Wire-Us header, on the error path too.
            const auto t0 = std::chrono::steady_clock::now();
            auto recording = mm::node::readHrdRecording(deviceManager_, pos, *data);
            const auto wireUs = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0);
            if (!recording) {
              sendError(res, "409 Conflict", config_.corsOrigin, recording.error(), wireUs);
              return;
            }
            setWireTime(res, wireUs);
            sendBytes(res, config_.corsOrigin, "text/csv", mm::node::toCsv(*recording));
          })
      // --- procedures -----------------------------------------------------------------------
      //
      // One resource per (device, procedure), addressed by name, with three verbs — POST starts a
      // run, GET returns the snapshot, DELETE cancels — plus a collection GET returning the
      // catalogue. These four handlers serve *every* procedure: the catalogue resolves the name,
      // decides whether the device has it, validates the request and supplies the body, so adding a
      // procedure is a row in that table and touches nothing here. It is also why this file names
      // no profile type — the descriptor text and the parameter rules live with the procedure.
      //
      // Progress is polled, never pushed — each snapshot is accumulating state in which finished
      // steps keep their status and value, so a client cannot miss a result between polls, and one
      // that reconnects sees how the last run went.
      .post("/api/devices/:slavePosition/procedures/:name",
            [this](auto* res, auto* req) {
              uint16_t pos{};
              auto posParam = req->getParameter("slavePosition");
              auto [p, ec] =
                  std::from_chars(posParam.data(), posParam.data() + posParam.size(), pos);
              const bool posOk = (ec == std::errc() && p == posParam.data() + posParam.size());
              // req is valid only for this call, so the name is copied out before onData can run.
              auto name = std::make_shared<std::string>(req->getParameter("name"));
              auto aborted = std::make_shared<bool>(false);
              auto body = std::make_shared<std::string>();
              res->onAborted([aborted]() { *aborted = true; });
              res->onData(
                  [this, res, body, name, aborted, pos, posOk](std::string_view chunk, bool last) {
                    body->append(chunk);
                    if (!last || *aborted) {
                      return;
                    }
                    if (!posOk) {
                      sendStatus(res, "400 Bad Request", config_.corsOrigin);
                      return;
                    }
                    // A procedure that takes no parameters is started with no body at all: an
                    // absent body becomes an empty object, so every validator reads its fields the
                    // same way instead of each one having to accept "nothing" as well.
                    nlohmann::json request = nlohmann::json::object();
                    if (!body->empty()) {
                      request = nlohmann::json::parse(*body, nullptr, false);
                      if (request.is_discarded()) {
                        sendError(res, "400 Bad Request", config_.corsOrigin, "invalid JSON body");
                        return;
                      }
                    }
                    auto snapshot = mm::node::startProcedure(deviceManager_, procedureManager_, pos,
                                                             *name, request);
                    if (!snapshot) {
                      sendError(res, procedureErrorStatus(snapshot.error().kind),
                                config_.corsOrigin, snapshot.error().message);
                      return;
                    }
                    // 202: the run is under way, not finished. Poll the GET for its outcome.
                    mm::api::setCorsOrigin(res->writeStatus("202 Accepted"), config_.corsOrigin)
                        ->writeHeader("Content-Type", "application/json")
                        ->end(nlohmann::json(*snapshot).dump());
                  });
            })
      .get("/api/devices/:slavePosition/procedures/:name",
           [this](auto* res, auto* req) {
             uint16_t pos{};
             auto posParam = req->getParameter("slavePosition");
             auto [p, ec] =
                 std::from_chars(posParam.data(), posParam.data() + posParam.size(), pos);
             if (ec != std::errc() || p != posParam.data() + posParam.size()) {
               sendStatus(res, "400 Bad Request", config_.corsOrigin);
               return;
             }
             // Never having run is not an absence: this reports the all-idle snapshot built from
             // the procedure's step template, so a client renders one shape and polls one loop.
             auto snapshot = mm::node::procedureSnapshot(deviceManager_, procedureManager_, pos,
                                                         req->getParameter("name"));
             if (!snapshot) {
               sendError(res, procedureErrorStatus(snapshot.error().kind), config_.corsOrigin,
                         snapshot.error().message);
               return;
             }
             sendJson(res, config_.corsOrigin, nlohmann::json(*snapshot));
           })
      .del("/api/devices/:slavePosition/procedures/:name",
           [this](auto* res, auto* req) {
             uint16_t pos{};
             auto posParam = req->getParameter("slavePosition");
             auto [p, ec] =
                 std::from_chars(posParam.data(), posParam.data() + posParam.size(), pos);
             if (ec != std::errc() || p != posParam.data() + posParam.size()) {
               sendStatus(res, "400 Bad Request", config_.corsOrigin);
               return;
             }
             // Cancels the run, not the record: the snapshot stays, reporting how far it got.
             auto cancelled = mm::node::cancelProcedure(deviceManager_, procedureManager_, pos,
                                                        req->getParameter("name"));
             if (!cancelled) {
               sendError(res, procedureErrorStatus(cancelled.error().kind), config_.corsOrigin,
                         cancelled.error().message);
               return;
             }
             sendStatus(res, "202 Accepted", config_.corsOrigin);
           })
      .get("/api/devices/:slavePosition/sdo/:index/:subindex",
           [this](auto* res, auto* req) {
             uint16_t pos{};
             auto posParam = req->getParameter("slavePosition");
             auto [p1, ec1] =
                 std::from_chars(posParam.data(), posParam.data() + posParam.size(), pos);
             if (ec1 != std::errc() || p1 != posParam.data() + posParam.size()) {
               sendStatus(res, "400 Bad Request", config_.corsOrigin);
               return;
             }
             auto index = mm::core::parseHexOrDec<uint16_t>(req->getParameter("index"));
             if (!index) {
               sendStatus(res, "400 Bad Request", config_.corsOrigin);
               return;
             }
             auto subindex = mm::core::parseHexOrDec<uint8_t>(req->getParameter("subindex"));
             if (!subindex) {
               sendStatus(res, "400 Bad Request", config_.corsOrigin);
               return;
             }
             const auto* device = deviceManager_.findDevice(pos);
             if (!device) {
               sendStatus(res, "404 Not Found", config_.corsOrigin);
               return;
             }
             // Time the SDO transaction itself so the client can distinguish the wire cost from
             // the (much larger, browser-side) HTTP round-trip. Brackets lock acquire + wire; a
             // warm request is dominated by the mailbox transaction, matching the driver's own log.
             // Reported via the uniform `X-Wire-Us` header (setWireTime), not the JSON body.
             const auto t0 = std::chrono::steady_clock::now();
             auto r = device->readSdo(*index, *subindex);
             const auto wireUs = std::chrono::duration_cast<std::chrono::microseconds>(
                 std::chrono::steady_clock::now() - t0);
             if (!r) {
               sendError(res, "500 Internal Server Error", config_.corsOrigin, r.error(), wireUs);
               return;
             }
             setWireTime(res, wireUs);
             sendJson(res, config_.corsOrigin, nlohmann::json{{"data", *r}});
           })
      .put("/api/devices/:slavePosition/sdo/:index/:subindex",
           [this](auto* res, auto* req) {
             uint16_t pos{};
             auto posParam = req->getParameter("slavePosition");
             auto [p1, ec1] =
                 std::from_chars(posParam.data(), posParam.data() + posParam.size(), pos);
             bool posOk = (ec1 == std::errc() && p1 == posParam.data() + posParam.size());
             // req is only valid synchronously — parse the path params before onData.
             auto index = mm::core::parseHexOrDec<uint16_t>(req->getParameter("index"));
             auto subindex = mm::core::parseHexOrDec<uint8_t>(req->getParameter("subindex"));
             auto aborted = std::make_shared<bool>(false);
             auto body = std::make_shared<std::string>();
             res->onAborted([aborted]() { *aborted = true; });
             res->onData([this, res, body, aborted, pos, posOk, index, subindex](
                             std::string_view chunk, bool last) {
               body->append(chunk);
               if (!last) return;
               if (*aborted) return;
               if (!posOk || !index || !subindex) {
                 sendStatus(res, "400 Bad Request", config_.corsOrigin);
                 return;
               }
               std::vector<uint8_t> data;
               try {
                 nlohmann::json j = nlohmann::json::parse(*body);
                 data = j.at("data").get<std::vector<uint8_t>>();
               } catch (const nlohmann::json::exception& e) {
                 sendError(res, "400 Bad Request", config_.corsOrigin, e.what());
                 return;
               }
               const auto* device = deviceManager_.findDevice(pos);
               if (!device) {
                 sendStatus(res, "404 Not Found", config_.corsOrigin);
                 return;
               }
               const auto t0 = std::chrono::steady_clock::now();
               auto r = device->writeSdo(*index, *subindex, data);
               const auto wireUs = std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now() - t0);
               if (!r) {
                 sendError(res, "500 Internal Server Error", config_.corsOrigin, r.error(), wireUs);
                 return;
               }
               setWireTime(res, wireUs);
               sendJson(res, config_.corsOrigin, nlohmann::json{{"ok", true}});
             });
           })
      // The collection beside the FoE file resource below. SOMANET firmware has no standard listing
      // service — it serves its directory as a pseudo-file read over FoE — so this is a vendor
      // operation, and a device that is not a SOMANET drive is refused rather than probed.
      .get("/api/devices/:slavePosition/files",
           [this](auto* res, auto* req) {
             uint16_t pos{};
             auto posParam = req->getParameter("slavePosition");
             auto [p, ec] =
                 std::from_chars(posParam.data(), posParam.data() + posParam.size(), pos);
             if (ec != std::errc() || p != posParam.data() + posParam.size()) {
               sendStatus(res, "400 Bad Request", config_.corsOrigin);
               return;
             }
             if (!deviceManager_.findDevice(pos)) {
               sendStatus(res, "404 Not Found", config_.corsOrigin);
               return;
             }
             sendTimedJson(res, config_.corsOrigin, "409 Conflict",
                           [&]() -> std::expected<nlohmann::json, std::string> {
                             auto files = mm::node::readFileList(deviceManager_, pos);
                             if (!files) {
                               return std::unexpected(files.error());
                             }
                             return nlohmann::json{{"files", *files}};
                           });
           })
      .get("/api/devices/:slavePosition/files/:filename",
           [this](auto* res, auto* req) {
             uint16_t pos{};
             auto posParam = req->getParameter("slavePosition");
             auto [p, ec] =
                 std::from_chars(posParam.data(), posParam.data() + posParam.size(), pos);
             if (ec != std::errc() || p != posParam.data() + posParam.size()) {
               sendStatus(res, "400 Bad Request", config_.corsOrigin);
               return;
             }
             std::string filename{req->getParameter("filename")};
             const auto* device = deviceManager_.findDevice(pos);
             if (!device) {
               sendStatus(res, "404 Not Found", config_.corsOrigin);
               return;
             }
             // Time the FoE transfer itself so the client can attribute the wire cost to the
             // device and the rest to the (much larger, browser-side) HTTP round-trip. The body is
             // raw octet-stream, so the figure rides the `X-Wire-Us` header (setWireTime) rather
             // than the JSON body — the uniform timing channel across endpoints.
             const auto t0 = std::chrono::steady_clock::now();
             auto r = device->readFile(filename);
             const auto wireUs = std::chrono::duration_cast<std::chrono::microseconds>(
                 std::chrono::steady_clock::now() - t0);
             if (!r) {
               sendError(res, "500 Internal Server Error", config_.corsOrigin, r.error().message,
                         wireUs);
               return;
             }
             setWireTime(res, wireUs);
             sendBytes(res, config_.corsOrigin, "application/octet-stream",
                       std::string_view{reinterpret_cast<const char*>(r->data()), r->size()});
           })
      .put("/api/devices/:slavePosition/files/:filename",
           [this](auto* res, auto* req) {
             uint16_t pos{};
             auto posParam = req->getParameter("slavePosition");
             auto [p, ec] =
                 std::from_chars(posParam.data(), posParam.data() + posParam.size(), pos);
             bool posOk = (ec == std::errc() && p == posParam.data() + posParam.size());
             // req is only valid synchronously — capture the filename before onData.
             std::string filename{req->getParameter("filename")};
             auto aborted = std::make_shared<bool>(false);
             auto body = std::make_shared<std::string>();
             res->onAborted([aborted]() { *aborted = true; });
             res->onData([this, res, body, aborted, pos, posOk, filename](std::string_view chunk,
                                                                          bool last) {
               body->append(chunk);
               if (!last) return;
               if (*aborted) return;
               if (!posOk) {
                 sendStatus(res, "400 Bad Request", config_.corsOrigin);
                 return;
               }
               const auto* device = deviceManager_.findDevice(pos);
               if (!device) {
                 sendStatus(res, "404 Not Found", config_.corsOrigin);
                 return;
               }
               std::span<const uint8_t> data{reinterpret_cast<const uint8_t*>(body->data()),
                                             body->size()};
               const auto t0 = std::chrono::steady_clock::now();
               auto r = device->writeFile(filename, data);
               const auto wireUs = std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now() - t0);
               if (!r) {
                 sendError(res, "500 Internal Server Error", config_.corsOrigin, r.error().message,
                           wireUs);
                 return;
               }
               setWireTime(res, wireUs);
               sendJson(res, config_.corsOrigin, nlohmann::json{{"ok", true}});
             });
           })
      .post("/api/devices/:slavePosition/parameters/init",
            [this](auto* res, auto* req) {
              uint16_t pos{};
              auto posParam = req->getParameter("slavePosition");
              auto [p, ec] =
                  std::from_chars(posParam.data(), posParam.data() + posParam.size(), pos);
              if (ec != std::errc() || p != posParam.data() + posParam.size()) {
                sendStatus(res, "400 Bad Request", config_.corsOrigin);
                return;
              }
              auto rv = req->getQuery("readValues");
              bool readValues = rv == "true" || rv == "1";
              sendTimedJson(
                  res, config_.corsOrigin, "500 Internal Server Error",
                  [&]() -> std::expected<nlohmann::json, std::string> {
                    if (auto r = deviceManager_.initializeDeviceParameters(pos, readValues); !r) {
                      return std::unexpected(r.error());
                    }
                    return nlohmann::json(deviceManager_.findDevice(pos)->parametersOrdered());
                  });
            })
      .post("/api/devices/:slavePosition/parameters/read",
            [this](auto* res, auto* req) {
              uint16_t pos{};
              auto posParam = req->getParameter("slavePosition");
              auto [p, ec] =
                  std::from_chars(posParam.data(), posParam.data() + posParam.size(), pos);
              if (ec != std::errc() || p != posParam.data() + posParam.size()) {
                sendStatus(res, "400 Bad Request", config_.corsOrigin);
                return;
              }
              sendTimedJson(
                  res, config_.corsOrigin, "500 Internal Server Error",
                  [&]() -> std::expected<nlohmann::json, std::string> {
                    if (auto r = deviceManager_.readAllDeviceParameters(pos); !r) {
                      return std::unexpected(r.error());
                    }
                    return nlohmann::json(deviceManager_.findDevice(pos)->parametersOrdered());
                  });
            })
      .get("/api/devices/:slavePosition/parameters",
           [this](auto* res, auto* req) {
             uint16_t pos{};
             auto posParam = req->getParameter("slavePosition");
             auto [p, ec] =
                 std::from_chars(posParam.data(), posParam.data() + posParam.size(), pos);
             if (ec != std::errc() || p != posParam.data() + posParam.size()) {
               sendStatus(res, "400 Bad Request", config_.corsOrigin);
               return;
             }
             const auto* device = deviceManager_.findDevice(pos);
             if (!device) {
               sendStatus(res, "404 Not Found", config_.corsOrigin);
               return;
             }
             sendJson(res, config_.corsOrigin, nlohmann::json(device->parametersOrdered()));
           })
      .get("/api/devices/:slavePosition/parameters/:index/:subindex",
           [this](auto* res, auto* req) {
             uint16_t pos{};
             auto posParam = req->getParameter("slavePosition");
             auto [p1, ec1] =
                 std::from_chars(posParam.data(), posParam.data() + posParam.size(), pos);
             if (ec1 != std::errc() || p1 != posParam.data() + posParam.size()) {
               sendStatus(res, "400 Bad Request", config_.corsOrigin);
               return;
             }
             auto index = mm::core::parseHexOrDec<uint16_t>(req->getParameter("index"));
             auto subindex = mm::core::parseHexOrDec<uint8_t>(req->getParameter("subindex"));
             if (!index || !subindex) {
               sendStatus(res, "400 Bad Request", config_.corsOrigin);
               return;
             }
             // ?source=cache serves the cached value with no bus I/O; anything else (including
             // an absent source) is the smart "auto" read that refreshes from the live PDO
             // image or an SDO upload. Routing lives in the node layer (Device::readParameter).
             bool refreshFromBus = req->getQuery("source") != "cache";
             sendTimedJson(res, config_.corsOrigin, "500 Internal Server Error", [&] {
               return deviceManager_.deviceParameterView(pos, *index, *subindex, refreshFromBus);
             });
           })
      .put("/api/devices/:slavePosition/parameters/:index/:subindex",
           [this](auto* res, auto* req) {
             uint16_t pos{};
             auto posParam = req->getParameter("slavePosition");
             auto [p1, ec1] =
                 std::from_chars(posParam.data(), posParam.data() + posParam.size(), pos);
             bool posOk = (ec1 == std::errc() && p1 == posParam.data() + posParam.size());
             // req is only valid synchronously — parse the path params before onData.
             auto index = mm::core::parseHexOrDec<uint16_t>(req->getParameter("index"));
             auto subindex = mm::core::parseHexOrDec<uint8_t>(req->getParameter("subindex"));
             auto aborted = std::make_shared<bool>(false);
             auto body = std::make_shared<std::string>();
             res->onAborted([aborted]() { *aborted = true; });
             res->onData([this, res, body, aborted, pos, posOk, index, subindex](
                             std::string_view chunk, bool last) {
               body->append(chunk);
               if (!last) return;
               if (*aborted) return;
               if (!posOk || !index || !subindex) {
                 sendStatus(res, "400 Bad Request", config_.corsOrigin);
                 return;
               }
               mm::node::DeviceParameterValue value;
               try {
                 nlohmann::json j = nlohmann::json::parse(*body);
                 auto parsed = parseParameterValue(j.at("value"));
                 if (!parsed) {
                   sendError(res, "400 Bad Request", config_.corsOrigin, parsed.error());
                   return;
                 }
                 value = std::move(*parsed);
               } catch (const nlohmann::json::exception& e) {
                 sendError(res, "400 Bad Request", config_.corsOrigin, e.what());
                 return;
               }
               // Smart write: PDO-staged when the object is output-mapped + exchanging, else
               // SDO, else held in the cache (offline). Coercion to the declared type is done
               // in the node layer (DeviceParameter::setValue). The cache echo below is memory-only
               // (no bus I/O), so timing the whole lambda still reports the write's wire cost.
               sendTimedJson(res, config_.corsOrigin, "500 Internal Server Error",
                             [&]() -> std::expected<nlohmann::json, std::string> {
                               if (auto w = deviceManager_.writeDeviceParameter(
                                       pos, *index, *subindex, std::move(value));
                                   !w) {
                                 return std::unexpected(w.error());
                               }
                               // Echo the updated parameter from the cache (no extra bus I/O) so
                               // the client gets the coerced value and resulting syncState in the
                               // same round-trip.
                               auto r = deviceManager_.deviceParameterView(pos, *index, *subindex,
                                                                           false);
                               if (!r) {
                                 return nlohmann::json{{"ok", true}};
                               }
                               return nlohmann::json(*r);
                             });
             });
           })
      .get("/api/meta/object-data-types",
           [this](auto* res, auto* /*req*/) {
             sendJson(res, config_.corsOrigin, nlohmann::json(mm::comm::kObjectDataTypes));
           })
      .post("/api/init",
            [this](auto* res, auto* /*req*/) {
              auto aborted = std::make_shared<bool>(false);
              auto body = std::make_shared<std::string>();
              res->onAborted([aborted]() { *aborted = true; });
              res->onData([this, res, body, aborted](std::string_view data, bool last) {
                body->append(data);
                if (!last) return;
                if (*aborted) return;
                if (!config_.initDeviceManager) {
                  sendStatus(res, "501 Not Implemented", config_.corsOrigin);
                  return;
                }
                // init() is one-shot — reject a re-init (e.g. a browser refresh
                // replaying the stored session) with 409 so the client can tell
                // "already connected" apart from a genuine init failure (500).
                if (deviceManager_.initialised()) {
                  sendError(res, "409 Conflict", config_.corsOrigin,
                            "already initialised — call reset() first");
                  return;
                }
                try {
                  nlohmann::json j =
                      body->empty() ? nlohmann::json::object() : nlohmann::json::parse(*body);
                  std::string driver = j.value("driver", "soem");
                  std::string adapter = j.value("adapter", "");
                  if (auto r = config_.initDeviceManager(driver, adapter); !r) {
                    sendError(res, "500 Internal Server Error", config_.corsOrigin, r.error());
                    return;
                  }
                  sendJson(res, config_.corsOrigin, nlohmann::json{{"ok", true}});
                } catch (const nlohmann::json::exception& e) {
                  sendError(res, "400 Bad Request", config_.corsOrigin, e.what());
                }
              });
            })
      .post("/api/scan",
            [this](auto* res, auto* /*req*/) {
              auto aborted = std::make_shared<bool>(false);
              res->onAborted([aborted]() { *aborted = true; });
              res->onData([this, res, aborted](std::string_view /*data*/, bool last) {
                if (!last) return;
                if (*aborted) return;
                if (auto r = deviceManager_.scan(); !r) {
                  sendError(res, "500 Internal Server Error", config_.corsOrigin, r.error());
                  return;
                } else {
                  sendJson(res, config_.corsOrigin, nlohmann::json{{"slaves", *r}});
                }
              });
            })
      .post("/api/reset",
            [this](auto* res, auto* /*req*/) {
              auto aborted = std::make_shared<bool>(false);
              res->onAborted([aborted]() { *aborted = true; });
              res->onData([this, res, aborted](std::string_view /*data*/, bool last) {
                if (!last) return;
                if (*aborted) return;
                deviceManager_.reset();
                sendJson(res, config_.corsOrigin, nlohmann::json{{"ok", true}});
              });
            })
      .post("/api/process-data/dump",
            [this](auto* res, auto* /*req*/) {
              auto aborted = std::make_shared<bool>(false);
              res->onAborted([aborted]() { *aborted = true; });
              res->onData([this, res, aborted](std::string_view /*data*/, bool last) {
                if (!last) return;
                if (*aborted) return;
                if (auto r = deviceManager_.dumpProcessData(); !r) {
                  sendError(res, "409 Conflict", config_.corsOrigin, r.error());
                } else {
                  sendJson(res, config_.corsOrigin, nlohmann::json{{"path", *r}});
                }
              });
            })
      .get("/api/process-data/dump",
           [this](auto* res, auto* /*req*/) {
             // Streams the recorder span as a raw `.mmpd` — the binary the client SDK parses.
             // (The POST variant writes the same bytes to a file and returns the path, for
             // terminal users.) Serialisation blocks this HTTP loop, but the WebSocket runs on
             // its own loop, so the monitoring stream is never stalled.
             auto aborted = std::make_shared<bool>(false);
             res->onAborted([aborted]() { *aborted = true; });
             auto r = deviceManager_.dumpProcessDataBuffer();
             if (*aborted) {
               return;
             }
             if (!r) {
               sendError(res, "409 Conflict", config_.corsOrigin, r.error());
               return;
             }
             auto body = std::make_shared<std::string>(std::move(*r));
             setCorsOrigin(res, config_.corsOrigin)
                 ->writeHeader("Content-Type", "application/octet-stream")
                 ->writeHeader("Content-Disposition",
                               "attachment; filename=\"motion-master-recorder.mmpd\"");
             // Backpressure-aware send: tryEnd what the socket accepts now, resume from the
             // acked write offset in onWritable until the whole buffer is flushed.
             std::string_view full{*body};
             auto [ok, done] = res->tryEnd(full, full.size());
             if (!done) {
               res->onWritable([res, body](uintptr_t offset) {
                 std::string_view chunk{body->data() + offset, body->size() - offset};
                 auto [chunkOk, chunkDone] = res->tryEnd(chunk, body->size());
                 return chunkOk;
               });
             }
           })
      .post("/api/process-data/outputs",
            [this](auto* res, auto* /*req*/) {
              auto aborted = std::make_shared<bool>(false);
              auto body = std::make_shared<std::string>();
              res->onAborted([aborted]() { *aborted = true; });
              res->onData([this, res, body, aborted](std::string_view chunk, bool last) {
                body->append(chunk);
                if (!last) return;
                if (*aborted) return;
                nlohmann::json j;
                try {
                  j = body->empty() ? nlohmann::json::array() : nlohmann::json::parse(*body);
                } catch (const nlohmann::json::exception& e) {
                  sendError(res, "400 Bad Request", config_.corsOrigin, e.what());
                  return;
                }
                auto requests = parseOutputStageRequests(j);
                if (!requests) {
                  sendError(res, "400 Bad Request", config_.corsOrigin, requests.error());
                  return;
                }
                // Per-object outcomes (staged vs written-but-not-cyclic vs error); the batch
                // never fails as a whole, so the UI can flag individual objects. 200, not 201 —
                // this stages values, it does not create a resource.
                auto results = deviceManager_.stageProcessDataOutputs(*requests);
                sendJson(res, config_.corsOrigin, nlohmann::json{{"results", results}});
              });
            })
      .post("/api/devices/state",
            [this](auto* res, auto* /*req*/) {
              auto aborted = std::make_shared<bool>(false);
              auto body = std::make_shared<std::string>();
              res->onAborted([aborted]() { *aborted = true; });
              res->onData([this, res, body, aborted](std::string_view chunk, bool last) {
                body->append(chunk);
                if (!last) return;
                if (*aborted) return;
                uint16_t stateVal{};
                std::vector<uint16_t> positions;
                int timeoutMs = 5000;
                try {
                  nlohmann::json j = nlohmann::json::parse(*body);
                  stateVal = j.at("state").get<uint16_t>();
                  if (j.contains("positions")) {
                    positions = j["positions"].get<std::vector<uint16_t>>();
                  }
                  if (j.contains("timeout")) {
                    timeoutMs = j["timeout"].get<int>();
                  }
                } catch (const nlohmann::json::exception& e) {
                  sendError(res, "400 Bad Request", config_.corsOrigin, e.what());
                  return;
                }
                using S = mm::comm::EtherCatState;
                if (stateVal != static_cast<uint16_t>(S::Init) &&
                    stateVal != static_cast<uint16_t>(S::PreOp) &&
                    stateVal != static_cast<uint16_t>(S::Boot) &&
                    stateVal != static_cast<uint16_t>(S::SafeOp) &&
                    stateVal != static_cast<uint16_t>(S::Op)) {
                  sendError(res, "400 Bad Request", config_.corsOrigin,
                            "invalid state: use 1 (Init), 2 (PreOp), 3 (Boot), 4 (SafeOp), or "
                            "8 (Op)");
                  return;
                }
                auto targetState = static_cast<S>(stateVal);
                auto r = deviceManager_.transitionToState(positions, targetState,
                                                          std::chrono::milliseconds(timeoutMs));
                if (!r) {
                  sendError(res, "500 Internal Server Error", config_.corsOrigin, r.error());
                  return;
                }
                // Report each device's settled state plus whether it reached the target, and
                // set the top-level "ok" only when every device did — so the UI no longer
                // reads success while the logs show a device stuck short of the target.
                bool allReached = true;
                nlohmann::json devices = nlohmann::json::array();
                for (const auto& info : *r) {
                  bool reached = !info.error && info.alState == stateVal;
                  allReached = allReached && reached;
                  nlohmann::json d = info;
                  d["reached"] = reached;
                  devices.push_back(std::move(d));
                }
                sendJson(res, config_.corsOrigin,
                         nlohmann::json{{"ok", allReached}, {"devices", devices}});
              });
            })
      .post("/api/monitorings",
            [this](auto* res, auto* /*req*/) {
              auto aborted = std::make_shared<bool>(false);
              auto body = std::make_shared<std::string>();
              res->onAborted([aborted]() { *aborted = true; });
              res->onData([this, res, body, aborted](std::string_view chunk, bool last) {
                body->append(chunk);
                if (!last) {
                  return;
                }
                if (*aborted) {
                  return;
                }
                nlohmann::json j;
                try {
                  j = body->empty() ? nlohmann::json::object() : nlohmann::json::parse(*body);
                } catch (const nlohmann::json::exception& e) {
                  sendError(res, "400 Bad Request", config_.corsOrigin, e.what());
                  return;
                }
                auto config = mm::parseMonitoringRequest(j);
                if (!config) {
                  sendError(res, "400 Bad Request", config_.corsOrigin, config.error());
                  return;
                }
                // Existence is the one conflict (409); every other rejection is a bad request.
                if (monitoringManager_.get(config->topic)) {
                  sendError(res, "409 Conflict", config_.corsOrigin,
                            "monitoring '" + config->topic + "' already exists");
                  return;
                }
                auto created = monitoringManager_.create(*config);
                if (!created) {
                  sendError(res, "400 Bad Request", config_.corsOrigin, created.error());
                  return;
                }
                auto resource = monitoringManager_.get(config->topic);
                setCorsOrigin(res->writeStatus("201 Created"), config_.corsOrigin)
                    ->writeHeader("Content-Type", "application/json")
                    ->end(resource->dump());
              });
            })
      .get("/api/monitorings",
           [this](auto* res, auto* /*req*/) {
             sendJson(res, config_.corsOrigin, monitoringManager_.list());
           })
      .get("/api/monitorings/:topic",
           [this](auto* res, auto* req) {
             auto resource = monitoringManager_.get(std::string(req->getParameter("topic")));
             if (!resource) {
               sendStatus(res, "404 Not Found", config_.corsOrigin);
               return;
             }
             sendJson(res, config_.corsOrigin, *resource);
           })
      .del("/api/monitorings/:topic",
           [this](auto* res, auto* req) {
             if (monitoringManager_.remove(std::string(req->getParameter("topic")))) {
               sendStatus(res, "204 No Content", config_.corsOrigin);
             } else {
               sendStatus(res, "404 Not Found", config_.corsOrigin);
             }
           })
      .get("/api/parameter-cache",
           [this](auto* res, auto* /*req*/) {
             sendJson(res, config_.corsOrigin, deviceManager_.parameterCache().list());
           })
      .get("/api/parameter-cache/:id",
           [this](auto* res, auto* req) {
             auto raw = deviceManager_.parameterCache().readRaw(req->getParameter("id"));
             if (!raw) {
               sendStatus(res, "404 Not Found", config_.corsOrigin);
               return;
             }
             // The file is JSON; serve it verbatim so the client can save it as-is.
             sendBytes(res, config_.corsOrigin, "application/json",
                       std::string_view{reinterpret_cast<const char*>(raw->data()), raw->size()});
           })
      .del("/api/parameter-cache/:id",
           [this](auto* res, auto* req) {
             if (deviceManager_.parameterCache().remove(req->getParameter("id"))) {
               sendStatus(res, "204 No Content", config_.corsOrigin);
             } else {
               sendStatus(res, "404 Not Found", config_.corsOrigin);
             }
           })
      // The user cache: a plain file store under Motion Master's per-user cache directory, with
      // the path after `/api/user-cache/` taken verbatim (percent-decoded) as the path under the
      // root. Sub-directories are implied by the path — there is no create-directory call; a PUT
      // makes whatever parents it needs, and a DELETE prunes whatever it empties. Every path is
      // validated by UserCache::resolve, which is what confines this unauthenticated endpoint to
      // the cache directory.
      // Each route reports its server-side cost via the same `X-Wire-Us` header the fieldbus
      // endpoints use. There is no device here — the figure is the filesystem operation (path
      // validation plus the read/write/list/remove) — but the channel is the uniform one, and the
      // split it enables is just as useful: a 40 MB dump that takes 2 s to download is a transfer
      // cost, not a slow server, and the two figures side by side say so.
      .get("/api/user-cache",
           [this](auto* res, auto* /*req*/) {
             // The root is reported so the page can tell the user where the files actually live —
             // it differs per platform and is overridable in the config.
             mm::api::sendTimedJson(res, config_.corsOrigin, "500 Internal Server Error", [this] {
               return userCache_.list().transform([this](const auto& files) {
                 return nlohmann::json{{"root", userCache_.root().string()}, {"files", files}};
               });
             });
           })
      .get("/api/user-cache/*",
           [this](auto* res, auto* req) {
             const std::string relPath = userCacheRelPath(req->getUrl());
             const auto t0 = std::chrono::steady_clock::now();
             auto data = userCache_.read(relPath);
             const auto wireUs = std::chrono::duration_cast<std::chrono::microseconds>(
                 std::chrono::steady_clock::now() - t0);
             if (!data) {
               sendError(res, "404 Not Found", config_.corsOrigin, data.error(), wireUs);
               return;
             }
             setWireTime(setUserCacheDownloadHeaders(res, config_.corsOrigin), wireUs)
                 ->end(std::string_view{reinterpret_cast<const char*>(data->data()), data->size()});
           })
      .put("/api/user-cache/*",
           [this](auto* res, auto* req) {
             // req is only valid synchronously — capture the path before onData.
             const auto relPath = std::make_shared<std::string>(userCacheRelPath(req->getUrl()));
             auto aborted = std::make_shared<bool>(false);
             auto body = std::make_shared<std::string>();
             res->onAborted([aborted]() { *aborted = true; });
             res->onData([this, res, body, aborted, relPath](std::string_view chunk, bool last) {
               body->append(chunk);
               if (!last) return;
               if (*aborted) return;
               // Only the write is timed — the body upload that precedes it is transport cost the
               // client already sees in its own round-trip figure.
               mm::api::sendTimedJson(
                   res, config_.corsOrigin, "400 Bad Request", [this, relPath, body] {
                     std::span<const uint8_t> data{reinterpret_cast<const uint8_t*>(body->data()),
                                                   body->size()};
                     return userCache_.write(*relPath, data).transform([&] {
                       return nlohmann::json{{"path", *relPath}, {"size", body->size()}};
                     });
                   });
             });
           })
      .del("/api/user-cache/*", [this](auto* res, auto* req) {
        const auto t0 = std::chrono::steady_clock::now();
        auto removed = userCache_.remove(userCacheRelPath(req->getUrl()));
        const auto wireUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0);
        if (!removed) {
          sendError(res, "400 Bad Request", config_.corsOrigin, removed.error(), wireUs);
        } else {
          // A recursive directory delete is the one operation here that can take real time, so it
          // is worth reporting even though the success response carries no body.
          sendStatus(res, *removed ? "204 No Content" : "404 Not Found", config_.corsOrigin,
                     wireUs);
        }
      });

  // Let registered plug-in modules add their own routes (e.g. /api/example/...) on top of the
  // built-in ones, before the CORS preflight and catch-all 404 are wired below.
  mm::api::RouteContext routeContext{deviceManager_, monitoringManager_, config_.corsOrigin};
  for (const auto& module : routeModules_) {
    module(app, routeContext);
  }

  app.options("/api/*",
              [this](auto* res, auto* /*req*/) {
                setCorsOrigin(res, config_.corsOrigin)
                    ->writeHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS")
                    ->writeHeader("Access-Control-Allow-Headers", "Content-Type")
                    // Let the browser cache this preflight so mutating requests (PUT/POST/DELETE
                    // with a JSON body) don't pay an extra OPTIONS round-trip on every call. The
                    // CORS policy here is static for the process lifetime, so a long max-age is
                    // safe; 600 s is the effective ceiling browsers honour (Chromium caps at 600,
                    // Firefox at 86400) — the min of the two keeps behaviour consistent.
                    ->writeHeader("Access-Control-Max-Age", "600")
                    ->writeStatus("204 No Content")
                    ->end();
              })
      .listen(config_.bindAddress, config_.port, LIBUS_LISTEN_EXCLUSIVE_PORT,
              [this](auto* token) {
                if (token) {
                  spdlog::info("HTTP server listening on {}:{}", config_.bindAddress, config_.port);
                  listenResult_.set_value(true);
                } else {
                  spdlog::error("HTTP server failed to listen on {}:{} (already in use?)",
                                config_.bindAddress, config_.port);
                  running_ = false;
                  listenResult_.set_value(false);
                }
              })
      .run();

  app_.store(nullptr);
  spdlog::debug("HTTP server event loop stopped");
}
