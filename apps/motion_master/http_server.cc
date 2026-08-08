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
// Reads a `{"data": [byte, ...]}` body into bytes, for the endpoints that write raw bytes to a
// device. Non-throwing (allow_exceptions = false) per the no-exceptions mandate; the shape it
// replaces caught a nlohmann exception and forwarded its text, which named the JSON fault rather
// than the expected shape.
std::expected<std::vector<uint8_t>, std::string> parseByteArrayBody(const std::string& text) {
  const auto body = nlohmann::json::parse(text, nullptr, false);
  if (body.is_discarded() || !body.contains("data") || !body["data"].is_array()) {
    return std::unexpected(R"(body must be {"data": [<byte>, ...]})");
  }
  std::vector<uint8_t> data;
  data.reserve(body["data"].size());
  for (const auto& entry : body["data"]) {
    if (!entry.is_number_unsigned() || entry.get<uint64_t>() > 0xFF) {
      return std::unexpected("every entry of 'data' must be a byte value (0-255)");
    }
    data.push_back(static_cast<uint8_t>(entry.get<uint64_t>()));
  }
  return data;
}

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

// setCorsOrigin remains for the two framework-level responses that are not Router routes: the
// OPTIONS preflight and the HTML index. Every actual API route returns a mm::api::Response, and
// the Router writes the CORS header itself.
using mm::api::setCorsOrigin;

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

  // ── Server, certificate and reference tables ────────────────────────────────────────────────
  // Static or near-static, but registered through the Router like everything else: one shape per
  // route means no per-endpoint judgement about whether it is "cheap enough" to run on the loop,
  // and no way for one to quietly acquire a blocking call later.
  router.get("/api/swagger.yml", [](const mm::api::Request&) {
    // Embedded at build time (swagger_spec.h). text/yaml renders inline in a browser; a client
    // fetches it to resolve the running server's exact API contract.
    auto response = mm::api::bytes("text/yaml; charset=utf-8", std::string(mm::kSwaggerYml));
    response.headers.emplace_back("Content-Disposition", "inline");
    return response;
  });

  router.get("/api/adapters", [](const mm::api::Request&) {
    return mm::api::json(nlohmann::json(mm::comm::enumerateNetworkAdapters()));
  });

  router.get("/api/version", [this](const mm::api::Request&) {
    return mm::api::json(nlohmann::json{{"version", config_.version}});
  });

  router.get("/api/config", [this](const mm::api::Request&) {
    // Pre-serialized at startup; parsed back so the response carries the JSON content type.
    return mm::api::json(config_.startedConfig.empty()
                             ? nlohmann::json::object()
                             : nlohmann::json::parse(config_.startedConfig));
  });

  router.get("/api/system-info", [](const mm::api::Request&) {
    return mm::api::json(nlohmann::json(mm::core::collectSystemInfo()));
  });

  router.get("/api/cert", [this](const mm::api::Request&) {
    auto info = mm::readCertInfo(config_.certFile);
    if (!info) {
      return mm::api::error("500 Internal Server Error", info.error());
    }
    return mm::api::json(certInfoJson(*info, config_.certFile));
  });

  // A ~1 s network fetch. It used to block every other request for its duration; off the loop it
  // is simply a slow endpoint, which is what it always should have been.
  router.post("/api/cert/refresh", [this](const mm::api::Request&) {
    if (!config_.refreshCert) {
      return mm::api::error("501 Not Implemented", "certificate refresh is not configured");
    }
    if (auto r = config_.refreshCert(); !r) {
      return mm::api::error("500 Internal Server Error", r.error());
    }
    // The fresh cert is on disk, but uSockets bound the old one at listen — restart to apply.
    auto info = mm::readCertInfo(config_.certFile);
    nlohmann::json body = info ? certInfoJson(*info, config_.certFile) : nlohmann::json::object();
    body["restartRequired"] = true;
    return mm::api::json(body);
  });

  router.get("/api/log", [this](const mm::api::Request&) {
    auto lines = config_.getLog ? config_.getLog() : std::vector<std::string>{};
    std::string body;
    for (const auto& line : lines) {
      body += line;
      body += '\n';
    }
    return mm::api::bytes("text/plain; charset=utf-8", std::move(body));
  });

  router.get("/api/meta/esc-registers", [](const mm::api::Request&) {
    return mm::api::json(nlohmann::json(mm::comm::kEscRegisters));
  });
  router.get("/api/meta/al-status-codes", [](const mm::api::Request&) {
    return mm::api::json(nlohmann::json(mm::comm::kAlStatusCodes));
  });
  router.get("/api/meta/foe-error-codes", [](const mm::api::Request&) {
    return mm::api::json(nlohmann::json(mm::comm::kFoeErrorCodes));
  });
  router.get("/api/meta/sdo-abort-codes", [](const mm::api::Request&) {
    return mm::api::json(nlohmann::json(mm::comm::kSdoAbortCodes));
  });
  router.get("/api/meta/mailbox-error-codes", [](const mm::api::Request&) {
    return mm::api::json(nlohmann::json(mm::comm::kMailboxErrorCodes));
  });

  // ── Bus-level reads and the game loop ───────────────────────────────────────────────────────
  router.get("/api/devices/diagnostics", [this](const mm::api::Request& req) -> mm::api::Response {
    auto positions = parsePositions(req);
    if (!positions) {
      return mm::api::badRequest(positions.error());
    }
    return mm::api::timed(
        [this, &positions] { return deviceManager_.deviceDiagnostics(*positions); });
  });

  router.get("/api/dc-sync", [this](const mm::api::Request& req) -> mm::api::Response {
    auto positions = parsePositions(req);
    if (!positions) {
      return mm::api::badRequest(positions.error());
    }
    return mm::api::timed([this, &positions] { return deviceManager_.dcSync(*positions); });
  });

  router.get("/api/devices", [this](const mm::api::Request&) {
    return mm::api::json(nlohmann::json(deviceManager_));
  });

  router.get("/api/process-image", [this](const mm::api::Request&) {
    return mm::api::json(nlohmann::json(deviceManager_.processImageInfo()));
  });

  router.get("/api/bus-config", [this](const mm::api::Request&) {
    return mm::api::json(nlohmann::json(deviceManager_.busConfig()));
  });

  router.get("/api/game-loop", [this](const mm::api::Request&) -> mm::api::Response {
    if (!config_.getGameLoopHealth) {
      return mm::api::error("501 Not Implemented", "game-loop health is not configured");
    }
    return mm::api::json(nlohmann::json(config_.getGameLoopHealth()));
  });

  router.put("/api/game-loop", [this](const mm::api::Request& req) -> mm::api::Response {
    if (!config_.setGameLoopPeriod) {
      return mm::api::error("501 Not Implemented", "game-loop control is not configured");
    }
    // parse(..., allow_exceptions = false) rather than a try/catch: the no-exceptions mandate, and
    // a discarded value says everything a caught exception would.
    const auto body = nlohmann::json::parse(req.body(), nullptr, false);
    if (body.is_discarded() || !body.contains("periodUs") ||
        !body["periodUs"].is_number_unsigned()) {
      return mm::api::badRequest("body must be {\"periodUs\": <microseconds>}");
    }
    // Validation lives in the callback (main.cc) — the HTTP layer just forwards.
    auto applied = config_.setGameLoopPeriod(body["periodUs"].get<uint32_t>());
    if (!applied) {
      return mm::api::badRequest(applied.error());
    }
    return mm::api::json(nlohmann::json(config_.getGameLoopHealth()));
  });

  router.get("/api/devices/:slavePosition",
             [this](const mm::api::Request& req) -> mm::api::Response {
               auto position = req.parameterAs<uint16_t>("slavePosition");
               if (!position) {
                 return mm::api::badRequest("slavePosition must be a number");
               }
               const auto* device = deviceManager_.findDevice(*position);
               if (device == nullptr) {
                 return mm::api::notFound("no device at that bus position");
               }
               return mm::api::json(nlohmann::json(*device));
             });

  // ── Offline tools: no device, no bus ────────────────────────────────────────────────────────
  // Off the loop for a different reason from the device endpoints: these are CPU-bound rather than
  // I/O-bound, and an ESI parse of a megabyte-scale file is seconds of work that used to run on the
  // loop thread. The Tools pages use them to inspect files with no hardware present.
  router.post("/api/sii/parse", [](const mm::api::Request& req) -> mm::api::Response {
    std::span<const uint8_t> bytes{reinterpret_cast<const uint8_t*>(req.body().data()),
                                   req.body().size()};
    auto parsed = mm::comm::parseSii(bytes);
    if (!parsed) {
      return mm::api::badRequest(parsed.error());
    }
    return mm::api::json(nlohmann::json(*parsed));
  });

  // Decodes a SOMANET firmware package filename into its five fields (Hardware description
  // specification 3.4.2) and, where the descriptor is numeric, its product id, version, key and
  // fieldbus. A GET with a query rather than a POST with a body: unlike its two siblings there is
  // no file to upload, only a short string. The same decoder firmware installation uses to decide
  // whether a package can be cached, so what this reports is what that will do.
  router.get("/api/firmware-package-name", [](const mm::api::Request& req) -> mm::api::Response {
    const auto filename = req.query("filename");
    if (!filename || filename->empty()) {
      return mm::api::badRequest("a 'filename' query parameter is required");
    }
    auto name = mm::node::parseFirmwarePackageName(*filename);
    if (!name) {
      // The grammar it missed, not a bare rejection: this is a tool whose whole job is to say what
      // a name means, so "why not" is the useful half of a negative answer.
      return mm::api::badRequest(name.error());
    }
    return mm::api::json(nlohmann::json(*name));
  });

  // The response carries every device with its own assembled entry table. That is affordable
  // because object-level annotation is stored once, on subindex 0, rather than repeated onto every
  // subindex. The optional modules= query narrows the merge where a slot offers a choice.
  router.post("/api/esi/parse", [](const mm::api::Request& req) -> mm::api::Response {
    mm::etg::EsiParseRequest request;
    if (const auto modules = req.query("modules"); modules && !modules->empty()) {
      auto idents = mm::etg::parseIdentList(*modules);
      if (!idents) {
        return mm::api::badRequest(std::format("modules: {}", idents.error()));
      }
      request.moduleIdents = std::move(*idents);
    }
    // Timed through the same X-Wire-Us channel as a device operation. The figure is CPU rather than
    // wire time here (parse + assemble every device's dictionary), which for a megabyte-scale ESI
    // is the part worth watching. JSON serialisation stays outside it, as everywhere else.
    return mm::api::timed([&] { return mm::etg::buildEsiResponse(req.body(), request); },
                          "400 Bad Request");
  });

  // ── Raw device access: ESC registers, EEPROM, watchdog ──────────────────────────────────────
  // Every one of these is a wire transaction behind the control-plane lock, so every one of them
  // could stall the whole API for as long as the bus was busy.
  router.get("/api/devices/:slavePosition/registers/:address",
             [this](const mm::api::Request& req) -> mm::api::Response {
               auto position = req.parameterAs<uint16_t>("slavePosition");
               auto address = req.parameterAs<uint16_t>("address");
               auto length = req.queryAs<uint16_t>("length");
               if (!position || !address) {
                 return mm::api::badRequest("slavePosition and address must be numbers");
               }
               if (!length || *length == 0) {
                 return mm::api::badRequest("'length' must be a non-zero byte count");
               }
               const auto* device = deviceManager_.findDevice(*position);
               if (device == nullptr) {
                 return mm::api::notFound("no device at that bus position");
               }
               std::vector<uint8_t> buffer(*length);
               return mm::api::timed([&]() -> std::expected<nlohmann::json, std::string> {
                 if (auto r = device->readRegister(*address, buffer); !r) {
                   return std::unexpected(r.error());
                 }
                 return nlohmann::json{{"data", buffer}};
               });
             });

  router.post("/api/devices/:slavePosition/registers/:address",
              [this](const mm::api::Request& req) -> mm::api::Response {
                auto position = req.parameterAs<uint16_t>("slavePosition");
                auto address = req.parameterAs<uint16_t>("address");
                if (!position || !address) {
                  return mm::api::badRequest("slavePosition and address must be numbers");
                }
                auto data = parseByteArrayBody(req.body());
                if (!data) {
                  return mm::api::badRequest(data.error());
                }
                const auto* device = deviceManager_.findDevice(*position);
                if (device == nullptr) {
                  return mm::api::notFound("no device at that bus position");
                }
                return mm::api::timed([&]() -> std::expected<nlohmann::json, std::string> {
                  if (auto r = device->writeRegister(*address, *data); !r) {
                    return std::unexpected(r.error());
                  }
                  return nlohmann::json{{"ok", true}};
                });
              });

  // Content negotiation: the raw EEPROM image only when the client asks via
  // `Accept: application/octet-stream`, otherwise the parsed structure, which is the default. Only
  // the EEPROM read touches the wire (parseSii is local CPU), so the read is what is timed and
  // X-Wire-Us rides every outcome.
  router.get(
      "/api/devices/:slavePosition/sii", [this](const mm::api::Request& req) -> mm::api::Response {
        auto position = req.parameterAs<uint16_t>("slavePosition");
        if (!position) {
          return mm::api::badRequest("slavePosition must be a number");
        }
        const auto* device = deviceManager_.findDevice(*position);
        if (device == nullptr) {
          return mm::api::notFound("no device at that bus position");
        }
        const auto t0 = std::chrono::steady_clock::now();
        auto raw = device->readSii();
        const auto wireUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0);
        if (!raw) {
          return mm::api::withWireTime(mm::api::error("500 Internal Server Error", raw.error()),
                                       wireUs);
        }
        if (req.accepts("application/octet-stream")) {
          return mm::api::withWireTime(
              mm::api::bytes("application/octet-stream", std::string(raw->begin(), raw->end())),
              wireUs);
        }
        auto parsed = mm::comm::parseSii(*raw);
        if (!parsed) {
          return mm::api::withWireTime(mm::api::error("500 Internal Server Error", parsed.error()),
                                       wireUs);
        }
        return mm::api::withWireTime(mm::api::json(nlohmann::json(*parsed)), wireUs);
      });

  router.put(
      "/api/devices/:slavePosition/sii", [this](const mm::api::Request& req) -> mm::api::Response {
        auto position = req.parameterAs<uint16_t>("slavePosition");
        if (!position) {
          return mm::api::badRequest("slavePosition must be a number");
        }
        std::span<const uint8_t> data{reinterpret_cast<const uint8_t*>(req.body().data()),
                                      req.body().size()};
        // Rejected before the EEPROM is touched: a malformed image can leave the device
        // unidentifiable, and the damage only shows after a power cycle.
        if (auto parsed = mm::comm::parseSii(data); !parsed) {
          return mm::api::badRequest(std::string("not a valid SII image: ") + parsed.error());
        }
        const auto* device = deviceManager_.findDevice(*position);
        if (device == nullptr) {
          return mm::api::notFound("no device at that bus position");
        }
        return mm::api::timed([&]() -> std::expected<nlohmann::json, std::string> {
          if (auto r = device->writeSii(data); !r) {
            return std::unexpected(r.error());
          }
          return nlohmann::json{{"ok", true}};
        });
      });

  router.get("/api/devices/:slavePosition/watchdog",
             [this](const mm::api::Request& req) -> mm::api::Response {
               auto position = req.parameterAs<uint16_t>("slavePosition");
               if (!position) {
                 return mm::api::badRequest("slavePosition must be a number");
               }
               if (deviceManager_.findDevice(*position) == nullptr) {
                 return mm::api::notFound("no device at that bus position");
               }
               return mm::api::timed([&]() -> std::expected<nlohmann::json, std::string> {
                 auto r = deviceManager_.processDataWatchdog(*position);
                 if (!r) {
                   return std::unexpected(r.error());
                 }
                 return watchdogJson(*position, *r);
               });
             });

  router.put(
      "/api/devices/:slavePosition/watchdog",
      [this](const mm::api::Request& req) -> mm::api::Response {
        auto position = req.parameterAs<uint16_t>("slavePosition");
        if (!position) {
          return mm::api::badRequest("slavePosition must be a number");
        }
        const auto body = nlohmann::json::parse(req.body(), nullptr, false);
        if (body.is_discarded() || !body.contains("timeoutMs") || !body["timeoutMs"].is_number()) {
          return mm::api::badRequest(R"(body must be {"timeoutMs": <milliseconds>})");
        }
        const double timeoutMs = body["timeoutMs"].get<double>();
        if (timeoutMs < 0) {
          return mm::api::badRequest("timeoutMs must not be negative");
        }
        if (deviceManager_.findDevice(*position) == nullptr) {
          return mm::api::notFound("no device at that bus position");
        }
        const auto timeout = std::chrono::nanoseconds(static_cast<int64_t>(timeoutMs * 1e6 + 0.5));
        return mm::api::timed([&]() -> std::expected<nlohmann::json, std::string> {
          auto r = deviceManager_.setProcessDataWatchdog(*position, timeout);
          if (!r) {
            return std::unexpected(r.error());
          }
          return watchdogJson(*position, *r);
        });
      });

  // Both brake verbs are the same operation with a different flag, as the pre-Router
  // handleBrakeCommand was. `settle` is exposed because brake release is open-loop — the firmware
  // reports no confirmation — so that margin is the only thing between "commanded" and "assume it
  // let go", and the right value is a property of the machine.
  auto brakeCommand = [this](const mm::api::Request& req, bool release) -> mm::api::Response {
    auto position = req.parameterAs<uint16_t>("slavePosition");
    if (!position) {
      return mm::api::badRequest("slavePosition must be a number");
    }
    auto settle = std::chrono::milliseconds(50);
    if (req.query("settle")) {
      auto settleMs = req.queryAs<uint32_t>("settle");
      if (!settleMs) {
        return mm::api::badRequest("'settle' must be a number of milliseconds");
      }
      settle = std::chrono::milliseconds(*settleMs);
    }
    if (deviceManager_.findDevice(*position) == nullptr) {
      return mm::api::notFound("no device at that bus position");
    }
    // Most of the round trip is the deliberate wait, which is what the wire time reports.
    return mm::api::timed(
        [&] {
          return release ? mm::node::releaseBrake(deviceManager_, *position, settle)
                         : mm::node::engageBrake(deviceManager_, *position, settle);
        },
        "409 Conflict");
  };

  // ── PDO mapping, CiA402 control and the brake ───────────────────────────────────────────────
  router.get("/api/devices/:slavePosition/pdo-mapping",
             [this](const mm::api::Request& req) -> mm::api::Response {
               auto position = req.parameterAs<uint16_t>("slavePosition");
               if (!position) {
                 return mm::api::badRequest("slavePosition must be a number");
               }
               if (deviceManager_.findDevice(*position) == nullptr) {
                 return mm::api::notFound("no device at that bus position");
               }
               // Read fresh over SDO, so the mailbox must be live: INIT/BOOT is a 409.
               return mm::api::timed([&] { return deviceManager_.readDevicePdoMapping(*position); },
                                     "409 Conflict");
             });

  router.put("/api/devices/:slavePosition/pdo-mapping",
             [this](const mm::api::Request& req) -> mm::api::Response {
               auto position = req.parameterAs<uint16_t>("slavePosition");
               if (!position) {
                 return mm::api::badRequest("slavePosition must be a number");
               }
               const auto body = nlohmann::json::parse(req.body(), nullptr, false);
               if (body.is_discarded()) {
                 return mm::api::badRequest("body must be JSON");
               }
               auto mapping = parseDevicePdoMapping(body);
               if (!mapping) {
                 return mm::api::badRequest(mapping.error());
               }
               if (deviceManager_.findDevice(*position) == nullptr) {
                 return mm::api::notFound("no device at that bus position");
               }
               // Both the write and the verifying read-back are on the wire, so both are timed.
               const auto t0 = std::chrono::steady_clock::now();
               auto elapsed = [&t0] {
                 return std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now() - t0);
               };
               // A failed write is usually a device-state precondition (not in PRE-OP) or a
               // rejected/verify-mismatched mapping — a conflict with the device's state rather
               // than a server fault.
               if (auto r = deviceManager_.writeDevicePdoMapping(*position, *mapping); !r) {
                 return mm::api::withWireTime(mm::api::error("409 Conflict", r.error()), elapsed());
               }
               // Echo the grouped read-back, whose entries carry the derived bitOffsets the
               // request did not specify.
               auto readBack = deviceManager_.readDevicePdoMapping(*position);
               if (!readBack) {
                 return mm::api::withWireTime(
                     mm::api::error("500 Internal Server Error", readBack.error()), elapsed());
               }
               return mm::api::withWireTime(mm::api::json(nlohmann::json(*readBack)), elapsed());
             });

  router.get("/api/devices/:slavePosition/cia402",
             [this](const mm::api::Request& req) -> mm::api::Response {
               auto position = req.parameterAs<uint16_t>("slavePosition");
               if (!position) {
                 return mm::api::badRequest("slavePosition must be a number");
               }
               if (deviceManager_.findDevice(*position) == nullptr) {
                 return mm::api::notFound("no device at that bus position");
               }
               // A non-CiA402 device (or one whose OD is not yet enumerated) is a 409; the node
               // layer's message says which, and the device's isCia402 flag lets a client not ask.
               return mm::api::timed(
                   [&] { return mm::node::cia402Status(deviceManager_, *position); },
                   "409 Conflict");
             });

  router.post("/api/devices/:slavePosition/cia402/mode",
              [this](const mm::api::Request& req) -> mm::api::Response {
                auto position = req.parameterAs<uint16_t>("slavePosition");
                if (!position) {
                  return mm::api::badRequest("slavePosition must be a number");
                }
                const auto body = nlohmann::json::parse(req.body(), nullptr, false);
                if (body.is_discarded() || !body.contains("mode") || !body["mode"].is_number()) {
                  return mm::api::badRequest(R"(body must be {"mode": <CiA402 mode number>})");
                }
                auto mode = mm::node::cia402::toOperationMode(body["mode"].get<int>());
                if (!mode) {
                  return mm::api::badRequest("unknown CiA402 operation mode");
                }
                if (deviceManager_.findDevice(*position) == nullptr) {
                  return mm::api::notFound("no device at that bus position");
                }
                auto r = mm::node::setCia402OperationMode(deviceManager_, *position, *mode);
                if (!r) {
                  return mm::api::error("409 Conflict", r.error());
                }
                return mm::api::json(nlohmann::json(*r));
              });

  router.post(
      "/api/devices/:slavePosition/cia402/command",
      [this](const mm::api::Request& req) -> mm::api::Response {
        auto position = req.parameterAs<uint16_t>("slavePosition");
        if (!position) {
          return mm::api::badRequest("slavePosition must be a number");
        }
        const auto body = nlohmann::json::parse(req.body(), nullptr, false);
        if (body.is_discarded() || !body.contains("command") || !body["command"].is_string()) {
          return mm::api::badRequest(R"(body must be {"command": "<name>"})");
        }
        auto command = mm::node::parseCia402Command(body["command"].get<std::string>());
        if (!command) {
          return mm::api::badRequest(
              "invalid command: use enable, disable, quickStop, or faultReset");
        }
        if (deviceManager_.findDevice(*position) == nullptr) {
          return mm::api::notFound("no device at that bus position");
        }
        auto r = mm::node::runCia402Command(deviceManager_, *position, *command);
        if (!r) {
          return mm::api::error("409 Conflict", r.error());
        }
        return mm::api::json(nlohmann::json(*r));
      });

  router.post(
      "/api/devices/:slavePosition/cia402/target",
      [this](const mm::api::Request& req) -> mm::api::Response {
        auto position = req.parameterAs<uint16_t>("slavePosition");
        if (!position) {
          return mm::api::badRequest("slavePosition must be a number");
        }
        const auto body = nlohmann::json::parse(req.body(), nullptr, false);
        if (body.is_discarded() || !body.contains("target") || !body["target"].is_string() ||
            !body.contains("value") || !body["value"].is_number_integer()) {
          return mm::api::badRequest(R"(body must be {"target": "<name>", "value": <integer>})");
        }
        auto kind = mm::node::parseCia402TargetKind(body["target"].get<std::string>());
        if (!kind) {
          return mm::api::badRequest("invalid target: use position, velocity, or torque");
        }
        // Signed: target position/velocity are INT32 and target torque INT16, all of which
        // take negative setpoints (reverse motion / regenerative torque).
        const auto value = body["value"].get<int32_t>();
        if (deviceManager_.findDevice(*position) == nullptr) {
          return mm::api::notFound("no device at that bus position");
        }
        auto r = mm::node::setCia402Target(deviceManager_, *position, *kind, value);
        if (!r) {
          return mm::api::error("409 Conflict", r.error());
        }
        return mm::api::statusOnly("204 No Content");
      });

  // GET reports the brake, POST release/engage command it. Two verbs rather than one
  // PUT {released}: they are not symmetric operations — a release waits out the drive's pull time
  // because the firmware blocks motion until it expires, an engage waits only a short settle — and
  // both answer with the state read back, which is also how a caller learns nothing happened (a
  // brake on release strategy 0 is not the firmware's to drive).
  router.get("/api/devices/:slavePosition/brake",
             [this](const mm::api::Request& req) -> mm::api::Response {
               auto position = req.parameterAs<uint16_t>("slavePosition");
               if (!position) {
                 return mm::api::badRequest("slavePosition must be a number");
               }
               if (deviceManager_.findDevice(*position) == nullptr) {
                 return mm::api::notFound("no device at that bus position");
               }
               auto state = mm::node::brakeState(deviceManager_, *position);
               if (!state) {
                 return mm::api::error("409 Conflict", state.error());
               }
               return mm::api::json(nlohmann::json(*state));
             });

  router.post("/api/devices/:slavePosition/brake/release",
              [brakeCommand](const mm::api::Request& req) { return brakeCommand(req, true); });
  router.post("/api/devices/:slavePosition/brake/engage",
              [brakeCommand](const mm::api::Request& req) { return brakeCommand(req, false); });

  // ── High resolution data and procedures ─────────────────────────────────────────────────────
  // Reads back what the `hrd-streaming` procedure recorded. Separate from the procedure, not its
  // final step: a recording is worth reading more than once, and a run's snapshot — re-sent whole
  // on every poll and retained until a rescan — is no place for ten thousand samples. `data` says
  // how to decode the files and is required, because nothing on the drive records which signal was
  // streamed into them.
  router.get(
      "/api/devices/:slavePosition/hrd", [this](const mm::api::Request& req) -> mm::api::Response {
        auto position = req.parameterAs<uint16_t>("slavePosition");
        if (!position) {
          return mm::api::badRequest("slavePosition must be a number");
        }
        auto data = mm::node::somanet::parseHrdData(req.query("data").value_or(""));
        if (!data) {
          return mm::api::badRequest(std::format(
              "'data' must be one of {} or {}",
              mm::node::somanet::toString(mm::node::somanet::HrdData::kEncoderRawData),
              mm::node::somanet::toString(mm::node::somanet::HrdData::kSystemIdentificationData)));
        }
        if (deviceManager_.findDevice(*position) == nullptr) {
          return mm::api::notFound("no device at that bus position");
        }
        // CSV for the spreadsheet-and-script half of the audience: a full recording is ten
        // thousand rows, which is a file to open rather than JSON to read. Same read and the
        // same decode, one rendering or the other — as on the SII endpoint above.
        if (!req.accepts("text/csv")) {
          return mm::api::timed(
              [&] { return mm::node::readHrdRecording(deviceManager_, *position, *data); },
              "409 Conflict");
        }
        const auto t0 = std::chrono::steady_clock::now();
        auto recording = mm::node::readHrdRecording(deviceManager_, *position, *data);
        const auto wireUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0);
        if (!recording) {
          return mm::api::withWireTime(mm::api::error("409 Conflict", recording.error()), wireUs);
        }
        return mm::api::withWireTime(mm::api::bytes("text/csv", mm::node::toCsv(*recording)),
                                     wireUs);
      });

  // One resource per (device, procedure), addressed by name, with three verbs — POST starts a run,
  // GET returns the snapshot, DELETE cancels. These four handlers serve *every* procedure: the
  // catalogue resolves the name, decides whether the device has it, validates the request and
  // supplies the body, so adding a procedure is a row in that table and touches nothing here.
  //
  // Progress is polled, never pushed — each snapshot is accumulating state in which finished steps
  // keep their status and value, so a client cannot miss a result between polls.
  router.post("/api/devices/:slavePosition/procedures/:name",
              [this](const mm::api::Request& req) -> mm::api::Response {
                auto position = req.parameterAs<uint16_t>("slavePosition");
                if (!position) {
                  return mm::api::badRequest("slavePosition must be a number");
                }
                // A procedure taking no parameters is started with no body at all: an absent body
                // becomes an empty object, so every validator reads its fields the same way
                // instead of each having to accept "nothing" as well.
                nlohmann::json body = nlohmann::json::object();
                if (!req.body().empty()) {
                  body = nlohmann::json::parse(req.body(), nullptr, false);
                  if (body.is_discarded()) {
                    return mm::api::badRequest("invalid JSON body");
                  }
                }
                auto snapshot = mm::node::startProcedure(deviceManager_, procedureManager_,
                                                         *position, req.parameter("name"), body);
                if (!snapshot) {
                  return mm::api::error(std::string(procedureErrorStatus(snapshot.error().kind)),
                                        snapshot.error().message);
                }
                // 202: the run is under way, not finished. Poll the GET for its outcome.
                auto response = mm::api::json(nlohmann::json(*snapshot));
                response.status = "202 Accepted";
                return response;
              });

  router.get("/api/devices/:slavePosition/procedures/:name",
             [this](const mm::api::Request& req) -> mm::api::Response {
               auto position = req.parameterAs<uint16_t>("slavePosition");
               if (!position) {
                 return mm::api::badRequest("slavePosition must be a number");
               }
               // Never having run is not an absence: this reports the all-idle snapshot built from
               // the procedure's step template, so a client renders one shape and polls one loop.
               auto snapshot = mm::node::procedureSnapshot(deviceManager_, procedureManager_,
                                                           *position, req.parameter("name"));
               if (!snapshot) {
                 return mm::api::error(std::string(procedureErrorStatus(snapshot.error().kind)),
                                       snapshot.error().message);
               }
               return mm::api::json(nlohmann::json(*snapshot));
             });

  router.del("/api/devices/:slavePosition/procedures/:name",
             [this](const mm::api::Request& req) -> mm::api::Response {
               auto position = req.parameterAs<uint16_t>("slavePosition");
               if (!position) {
                 return mm::api::badRequest("slavePosition must be a number");
               }
               // Cancels the run, not the record: the snapshot stays, reporting how far it got.
               auto cancelled = mm::node::cancelProcedure(deviceManager_, procedureManager_,
                                                          *position, req.parameter("name"));
               if (!cancelled) {
                 return mm::api::error(std::string(procedureErrorStatus(cancelled.error().kind)),
                                       cancelled.error().message);
               }
               return mm::api::statusOnly("202 Accepted");
             });

  // ── SDO access and File over EtherCAT ───────────────────────────────────────────────────────
  // Both are single mailbox transactions whose wire cost is worth separating from the browser-side
  // round trip, which is much larger — hence X-Wire-Us on every outcome, success or failure. A
  // failed SDO is the clearest case: it can spend the full 700 ms mailbox timeout getting no
  // answer.
  router.get("/api/devices/:slavePosition/sdo/:index/:subindex",
             [this](const mm::api::Request& req) -> mm::api::Response {
               auto position = req.parameterAs<uint16_t>("slavePosition");
               auto index = req.parameterAs<uint16_t>("index");
               auto subindex = req.parameterAs<uint8_t>("subindex");
               if (!position || !index || !subindex) {
                 return mm::api::badRequest("slavePosition, index and subindex must be numbers");
               }
               const auto* device = deviceManager_.findDevice(*position);
               if (device == nullptr) {
                 return mm::api::notFound("no device at that bus position");
               }
               return mm::api::timed([&]() -> std::expected<nlohmann::json, std::string> {
                 auto r = device->readSdo(*index, *subindex);
                 if (!r) {
                   return std::unexpected(r.error());
                 }
                 return nlohmann::json{{"data", *r}};
               });
             });

  router.put("/api/devices/:slavePosition/sdo/:index/:subindex",
             [this](const mm::api::Request& req) -> mm::api::Response {
               auto position = req.parameterAs<uint16_t>("slavePosition");
               auto index = req.parameterAs<uint16_t>("index");
               auto subindex = req.parameterAs<uint8_t>("subindex");
               if (!position || !index || !subindex) {
                 return mm::api::badRequest("slavePosition, index and subindex must be numbers");
               }
               auto data = parseByteArrayBody(req.body());
               if (!data) {
                 return mm::api::badRequest(data.error());
               }
               const auto* device = deviceManager_.findDevice(*position);
               if (device == nullptr) {
                 return mm::api::notFound("no device at that bus position");
               }
               return mm::api::timed([&]() -> std::expected<nlohmann::json, std::string> {
                 if (auto r = device->writeSdo(*index, *subindex, *data); !r) {
                   return std::unexpected(r.error());
                 }
                 return nlohmann::json{{"ok", true}};
               });
             });

  // EtherCAT defines no directory service, so this is the SOMANET `fs-getlist` pseudo-file read
  // over FoE and parsed by the server; a device that is not a SOMANET drive is refused rather than
  // probed.
  router.get("/api/devices/:slavePosition/files",
             [this](const mm::api::Request& req) -> mm::api::Response {
               auto position = req.parameterAs<uint16_t>("slavePosition");
               if (!position) {
                 return mm::api::badRequest("slavePosition must be a number");
               }
               if (deviceManager_.findDevice(*position) == nullptr) {
                 return mm::api::notFound("no device at that bus position");
               }
               return mm::api::timed(
                   [&]() -> std::expected<nlohmann::json, std::string> {
                     auto files = mm::node::readFileList(deviceManager_, *position);
                     if (!files) {
                       return std::unexpected(files.error());
                     }
                     return nlohmann::json(*files);
                   },
                   "409 Conflict");
             });

  router.get("/api/devices/:slavePosition/files/:filename",
             [this](const mm::api::Request& req) -> mm::api::Response {
               auto position = req.parameterAs<uint16_t>("slavePosition");
               if (!position) {
                 return mm::api::badRequest("slavePosition must be a number");
               }
               const auto* device = deviceManager_.findDevice(*position);
               if (device == nullptr) {
                 return mm::api::notFound("no device at that bus position");
               }
               const auto t0 = std::chrono::steady_clock::now();
               auto contents = device->readFile(std::string(req.parameter("filename")));
               const auto wireUs = std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now() - t0);
               if (!contents) {
                 return mm::api::withWireTime(
                     mm::api::error("500 Internal Server Error", contents.error().message), wireUs);
               }
               return mm::api::withWireTime(
                   mm::api::bytes("application/octet-stream",
                                  std::string(contents->begin(), contents->end())),
                   wireUs);
             });

  router.put("/api/devices/:slavePosition/files/:filename",
             [this](const mm::api::Request& req) -> mm::api::Response {
               auto position = req.parameterAs<uint16_t>("slavePosition");
               if (!position) {
                 return mm::api::badRequest("slavePosition must be a number");
               }
               const auto* device = deviceManager_.findDevice(*position);
               if (device == nullptr) {
                 return mm::api::notFound("no device at that bus position");
               }
               std::span<const uint8_t> data{reinterpret_cast<const uint8_t*>(req.body().data()),
                                             req.body().size()};
               const std::string filename(req.parameter("filename"));
               return mm::api::timed([&]() -> std::expected<nlohmann::json, std::string> {
                 if (auto r = device->writeFile(filename, data); !r) {
                   return std::unexpected(r.error().message);
                 }
                 return nlohmann::json{{"ok", true}};
               });
             });

  // ── CoE parameters ──────────────────────────────────────────────────────────────────────────
  // Enumerating an object dictionary is hundreds of mailbox round-trips and takes seconds, which
  // made this the second-worst offender after a firmware transfer.
  router.post("/api/devices/:slavePosition/parameters/init",
              [this](const mm::api::Request& req) -> mm::api::Response {
                auto position = req.parameterAs<uint16_t>("slavePosition");
                if (!position) {
                  return mm::api::badRequest("slavePosition must be a number");
                }
                const auto readValues = req.query("readValues").value_or("");
                const bool withValues = readValues == "true" || readValues == "1";
                return mm::api::timed([&]() -> std::expected<nlohmann::json, std::string> {
                  if (auto r = deviceManager_.initializeDeviceParameters(*position, withValues);
                      !r) {
                    return std::unexpected(r.error());
                  }
                  return nlohmann::json(deviceManager_.findDevice(*position)->parametersOrdered());
                });
              });

  router.post("/api/devices/:slavePosition/parameters/read",
              [this](const mm::api::Request& req) -> mm::api::Response {
                auto position = req.parameterAs<uint16_t>("slavePosition");
                if (!position) {
                  return mm::api::badRequest("slavePosition must be a number");
                }
                return mm::api::timed([&]() -> std::expected<nlohmann::json, std::string> {
                  if (auto r = deviceManager_.readAllDeviceParameters(*position); !r) {
                    return std::unexpected(r.error());
                  }
                  return nlohmann::json(deviceManager_.findDevice(*position)->parametersOrdered());
                });
              });

  router.get("/api/devices/:slavePosition/parameters",
             [this](const mm::api::Request& req) -> mm::api::Response {
               auto position = req.parameterAs<uint16_t>("slavePosition");
               if (!position) {
                 return mm::api::badRequest("slavePosition must be a number");
               }
               const auto* device = deviceManager_.findDevice(*position);
               if (device == nullptr) {
                 return mm::api::notFound("no device at that bus position");
               }
               return mm::api::json(nlohmann::json(device->parametersOrdered()));
             });

  router.get("/api/devices/:slavePosition/parameters/:index/:subindex",
             [this](const mm::api::Request& req) -> mm::api::Response {
               auto position = req.parameterAs<uint16_t>("slavePosition");
               auto index = req.parameterAs<uint16_t>("index");
               auto subindex = req.parameterAs<uint8_t>("subindex");
               if (!position || !index || !subindex) {
                 return mm::api::badRequest("slavePosition, index and subindex must be numbers");
               }
               // ?source=cache serves the cached value with no bus I/O; anything else (including an
               // absent source) is the smart "auto" read that refreshes from the live PDO image or
               // an SDO upload. The routing itself lives in Device::readParameter.
               const bool refreshFromBus = req.query("source").value_or("") != "cache";
               return mm::api::timed([&] {
                 return deviceManager_.deviceParameterView(*position, *index, *subindex,
                                                           refreshFromBus);
               });
             });

  router.put("/api/devices/:slavePosition/parameters/:index/:subindex",
             [this](const mm::api::Request& req) -> mm::api::Response {
               auto position = req.parameterAs<uint16_t>("slavePosition");
               auto index = req.parameterAs<uint16_t>("index");
               auto subindex = req.parameterAs<uint8_t>("subindex");
               if (!position || !index || !subindex) {
                 return mm::api::badRequest("slavePosition, index and subindex must be numbers");
               }
               const auto body = nlohmann::json::parse(req.body(), nullptr, false);
               if (body.is_discarded() || !body.contains("value")) {
                 return mm::api::badRequest(R"(body must be {"value": <value>})");
               }
               auto value = parseParameterValue(body["value"]);
               if (!value) {
                 return mm::api::badRequest(value.error());
               }
               // Smart write: PDO-staged when the object is output-mapped and exchanging, else SDO,
               // else held in the cache (offline). Coercion to the declared type happens in
               // DeviceParameter::setValue. The echo below is memory-only, so timing the whole
               // lambda still reports the write's wire cost.
               return mm::api::timed([&]() -> std::expected<nlohmann::json, std::string> {
                 if (auto w = deviceManager_.writeDeviceParameter(*position, *index, *subindex,
                                                                  std::move(*value));
                     !w) {
                   return std::unexpected(w.error());
                 }
                 // Echo the updated parameter from the cache so the client gets the coerced value
                 // and the resulting syncState in the same round trip.
                 auto r = deviceManager_.deviceParameterView(*position, *index, *subindex, false);
                 if (!r) {
                   return nlohmann::json{{"ok", true}};
                 }
                 return nlohmann::json(*r);
               });
             });

  router.get("/api/meta/object-data-types", [](const mm::api::Request&) {
    return mm::api::json(nlohmann::json(mm::comm::kObjectDataTypes));
  });

  // ── Fieldbus lifecycle ──────────────────────────────────────────────────────────────────────
  // scan() walks the bus and can enumerate object dictionaries, so it is seconds of work; init()
  // opens the raw socket. Both take the bus lock exclusively, which is what used to make every
  // other request wait behind them.
  router.post("/api/init", [this](const mm::api::Request& req) -> mm::api::Response {
    if (!config_.initDeviceManager) {
      return mm::api::statusOnly("501 Not Implemented");
    }
    // init() is one-shot — a re-init (a browser refresh replaying the stored session) is a 409 so
    // the client can tell "already connected" apart from a genuine init failure.
    if (deviceManager_.initialised()) {
      return mm::api::error("409 Conflict", "already initialised — call reset() first");
    }
    nlohmann::json body = nlohmann::json::object();
    if (!req.body().empty()) {
      body = nlohmann::json::parse(req.body(), nullptr, false);
      if (body.is_discarded() || !body.is_object()) {
        return mm::api::badRequest("body must be a JSON object");
      }
    }
    if (auto r = config_.initDeviceManager(body.value("driver", "soem"), body.value("adapter", ""));
        !r) {
      return mm::api::error("500 Internal Server Error", r.error());
    }
    return mm::api::json(nlohmann::json{{"ok", true}});
  });

  router.post("/api/scan", [this](const mm::api::Request&) -> mm::api::Response {
    auto slaves = deviceManager_.scan();
    if (!slaves) {
      return mm::api::error("500 Internal Server Error", slaves.error());
    }
    return mm::api::json(nlohmann::json{{"slaves", *slaves}});
  });

  router.post("/api/reset", [this](const mm::api::Request&) {
    deviceManager_.reset();
    return mm::api::json(nlohmann::json{{"ok", true}});
  });

  // ── Process data and AL state ───────────────────────────────────────────────────────────────
  router.post("/api/process-data/dump", [this](const mm::api::Request&) -> mm::api::Response {
    auto path = deviceManager_.dumpProcessData();
    if (!path) {
      return mm::api::error("409 Conflict", path.error());
    }
    return mm::api::json(nlohmann::json{{"path", *path}});
  });

  // Streams the recorder span as a raw `.mmpd` — the binary the client SDK parses. (The POST
  // variant writes the same bytes to a file and returns the path, for terminal users.) A dump is
  // tens of megabytes; the hand-written tryEnd/onWritable loop this used to carry is gone, because
  // the Router writes every response backpressure-aware.
  router.get("/api/process-data/dump", [this](const mm::api::Request&) -> mm::api::Response {
    auto buffer = deviceManager_.dumpProcessDataBuffer();
    if (!buffer) {
      return mm::api::error("409 Conflict", buffer.error());
    }
    auto response = mm::api::bytes("application/octet-stream", std::move(*buffer));
    response.headers.emplace_back("Content-Disposition",
                                  R"(attachment; filename="motion-master-recorder.mmpd")");
    return response;
  });

  router.post("/api/process-data/outputs",
              [this](const mm::api::Request& req) -> mm::api::Response {
                nlohmann::json body = nlohmann::json::array();
                if (!req.body().empty()) {
                  body = nlohmann::json::parse(req.body(), nullptr, false);
                  if (body.is_discarded()) {
                    return mm::api::badRequest("body must be a JSON array of output requests");
                  }
                }
                auto requests = parseOutputStageRequests(body);
                if (!requests) {
                  return mm::api::badRequest(requests.error());
                }
                // Per-object outcomes (staged vs written-but-not-cyclic vs error); the batch never
                // fails as a whole, so the UI can flag individual objects. 200 rather than 201 —
                // this stages values, it does not create a resource.
                return mm::api::json(
                    nlohmann::json{{"results", deviceManager_.stageProcessDataOutputs(*requests)}});
              });

  router.post("/api/devices/state", [this](const mm::api::Request& req) -> mm::api::Response {
    const auto body = nlohmann::json::parse(req.body(), nullptr, false);
    if (body.is_discarded() || !body.contains("state") || !body["state"].is_number_unsigned()) {
      return mm::api::badRequest(R"(body must be {"state": <AL state>, ...})");
    }
    const auto requested = body["state"].get<uint16_t>();
    using S = mm::comm::EtherCatState;
    if (requested != static_cast<uint16_t>(S::Init) &&
        requested != static_cast<uint16_t>(S::PreOp) &&
        requested != static_cast<uint16_t>(S::Boot) &&
        requested != static_cast<uint16_t>(S::SafeOp) &&
        requested != static_cast<uint16_t>(S::Op)) {
      return mm::api::badRequest(
          "invalid state: use 1 (Init), 2 (PreOp), 3 (Boot), 4 (SafeOp), or 8 (Op)");
    }
    std::vector<uint16_t> positions;
    if (body.contains("positions")) {
      if (!body["positions"].is_array()) {
        return mm::api::badRequest("'positions' must be an array of bus positions");
      }
      positions = body["positions"].get<std::vector<uint16_t>>();
    }
    int timeoutMs = 5000;
    if (body.contains("timeout")) {
      if (!body["timeout"].is_number_integer()) {
        return mm::api::badRequest("'timeout' must be a number of milliseconds");
      }
      timeoutMs = body["timeout"].get<int>();
    }
    auto states = deviceManager_.transitionToState(positions, static_cast<S>(requested),
                                                   std::chrono::milliseconds(timeoutMs));
    if (!states) {
      return mm::api::error("500 Internal Server Error", states.error());
    }
    // Report each device's settled state plus whether it reached the target, and set the top-level
    // "ok" only when every device did — so a client no longer reads success while the log shows a
    // device stuck short of it.
    bool allReached = true;
    nlohmann::json devices = nlohmann::json::array();
    for (const auto& info : *states) {
      const bool reached = !info.error && info.alState == requested;
      allReached = allReached && reached;
      nlohmann::json entry = info;
      entry["reached"] = reached;
      devices.push_back(std::move(entry));
    }
    return mm::api::json(nlohmann::json{{"ok", allReached}, {"devices", devices}});
  });

  // ── Monitorings and the two file stores ─────────────────────────────────────────────────────
  router.post("/api/monitorings", [this](const mm::api::Request& req) -> mm::api::Response {
    nlohmann::json body = nlohmann::json::object();
    if (!req.body().empty()) {
      body = nlohmann::json::parse(req.body(), nullptr, false);
      if (body.is_discarded()) {
        return mm::api::badRequest("body must be a JSON object");
      }
    }
    auto config = mm::parseMonitoringRequest(body);
    if (!config) {
      return mm::api::badRequest(config.error());
    }
    // Existence is the one conflict (409); every other rejection is a bad request.
    if (monitoringManager_.get(config->topic)) {
      return mm::api::error("409 Conflict", "monitoring '" + config->topic + "' already exists");
    }
    auto created = monitoringManager_.create(*config);
    if (!created) {
      return mm::api::badRequest(created.error());
    }
    auto response = mm::api::json(*monitoringManager_.get(config->topic));
    response.status = "201 Created";
    return response;
  });

  router.get("/api/monitorings",
             [this](const mm::api::Request&) { return mm::api::json(monitoringManager_.list()); });

  router.get("/api/monitorings/:topic", [this](const mm::api::Request& req) -> mm::api::Response {
    auto resource = monitoringManager_.get(std::string(req.parameter("topic")));
    if (!resource) {
      return mm::api::notFound("no monitoring with that topic");
    }
    return mm::api::json(*resource);
  });

  router.del("/api/monitorings/:topic", [this](const mm::api::Request& req) -> mm::api::Response {
    if (!monitoringManager_.remove(std::string(req.parameter("topic")))) {
      return mm::api::notFound("no monitoring with that topic");
    }
    return mm::api::statusOnly("204 No Content");
  });

  router.get("/api/parameter-cache", [this](const mm::api::Request&) {
    return mm::api::json(deviceManager_.parameterCache().list());
  });

  router.get("/api/parameter-cache/:id", [this](const mm::api::Request& req) -> mm::api::Response {
    auto raw = deviceManager_.parameterCache().readRaw(req.parameter("id"));
    if (!raw) {
      return mm::api::notFound("no cache entry with that id");
    }
    // The file is JSON; served verbatim so a client can save it as-is.
    return mm::api::bytes("application/json", std::string(raw->begin(), raw->end()));
  });

  router.del("/api/parameter-cache/:id", [this](const mm::api::Request& req) -> mm::api::Response {
    if (!deviceManager_.parameterCache().remove(req.parameter("id"))) {
      return mm::api::notFound("no cache entry with that id");
    }
    return mm::api::statusOnly("204 No Content");
  });

  // The user cache: a plain file store under Motion Master's per-user cache directory, with the
  // path after `/api/user-cache/` taken verbatim (percent-decoded) as the path under the root.
  // Sub-directories are implied by the path — a PUT makes whatever parents it needs and a DELETE
  // prunes whatever it empties. Every path goes through UserCache::resolve, which is what confines
  // this unauthenticated endpoint to the cache directory.
  //
  // Each route reports its server-side cost through the same X-Wire-Us header the fieldbus
  // endpoints use. There is no device here — the figure is the filesystem operation — but the split
  // it enables is just as useful: a 40 MB dump taking 2 s to download is a transfer cost, not a
  // slow server, and the two figures side by side say so.
  router.get("/api/user-cache", [this](const mm::api::Request&) {
    // The root is reported so the page can tell the user where the files actually live — it differs
    // per platform and is overridable in the config.
    return mm::api::timed([this] {
      return userCache_.list().transform([this](const auto& files) {
        return nlohmann::json{{"root", userCache_.root().string()}, {"files", files}};
      });
    });
  });

  router.get("/api/user-cache/*", [this](const mm::api::Request& req) -> mm::api::Response {
    const std::string relPath = userCacheRelPath(req.url());
    const auto t0 = std::chrono::steady_clock::now();
    auto data = userCache_.read(relPath);
    const auto wireUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0);
    if (!data) {
      return mm::api::withWireTime(mm::api::error("404 Not Found", data.error()), wireUs);
    }
    // The headers that make a user-cache download inert in a browser.
    //
    // The cache serves bytes the user themselves uploaded, from the API's own origin — so a
    // rendered response is stored XSS against the origin that controls the drives, and one CORS
    // does not help with (a script *on* this origin is same-origin by definition). An earlier
    // draft guessed a content type from the extension so an ESI would display inline; that is
    // exactly the hole, since a browser executes script in an `application/xml` document (an XSLT
    // processing instruction, or inline XHTML). These close it, and cost the real consumers
    // nothing — the Console and the SDK both read the bytes, never render them:
    //
    //  - `application/octet-stream` unconditionally: no extension is trusted to name a type.
    //  - `Content-Disposition: attachment`: download, never render. Deliberately with **no**
    //    filename — the path is user-controlled, and a quote or newline in a header value is
    //    response splitting. The client names the saved file itself.
    //  - `X-Content-Type-Options: nosniff`: stops a browser second-guessing the type above.
    //  - a Content-Security-Policy permitting nothing, as the last line of defence.
    auto response =
        mm::api::bytes("application/octet-stream", std::string(data->begin(), data->end()));
    response.headers.emplace_back("Content-Disposition", "attachment");
    response.headers.emplace_back("X-Content-Type-Options", "nosniff");
    response.headers.emplace_back("Content-Security-Policy", "default-src 'none'; sandbox");
    return mm::api::withWireTime(std::move(response), wireUs);
  });

  router.put("/api/user-cache/*", [this](const mm::api::Request& req) {
    const std::string relPath = userCacheRelPath(req.url());
    // Only the write is timed — the body upload that precedes it is transport cost the client
    // already sees in its own round-trip figure.
    return mm::api::timed(
        [this, &relPath, &req] {
          std::span<const uint8_t> data{reinterpret_cast<const uint8_t*>(req.body().data()),
                                        req.body().size()};
          return userCache_.write(relPath, data).transform([&] {
            return nlohmann::json{{"path", relPath}, {"size", req.body().size()}};
          });
        },
        "400 Bad Request");
  });

  router.del("/api/user-cache/*", [this](const mm::api::Request& req) -> mm::api::Response {
    const auto t0 = std::chrono::steady_clock::now();
    auto removed = userCache_.remove(userCacheRelPath(req.url()));
    const auto wireUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0);
    if (!removed) {
      return mm::api::withWireTime(mm::api::error("400 Bad Request", removed.error()), wireUs);
    }
    // A recursive directory delete is the one operation here that can take real time, so it is
    // worth reporting even though the success response carries no body.
    return mm::api::withWireTime(mm::api::statusOnly(*removed ? "204 No Content" : "404 Not Found"),
                                 wireUs);
  });

  // ── end of Router registrations ─────────────────────────────────────────────────────────────

  // Register the built-in routes as a statement on `app` (not moved), then hand `app` to any
  // registered plug-in modules so they can add their own routes, then finish with the CORS
  // preflight, the catch-all 404, and listen(). All three phases operate on the same `app` object.
  app.get("/", [](auto* res, auto* /*req*/) {
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
  });

  // Let registered plug-in modules add their own routes (e.g. /api/example/...) on top of the
  // built-in ones, before the CORS preflight and catch-all 404 are wired below.
  mm::api::RouteContext routeContext{deviceManager_, monitoringManager_, config_.corsOrigin};
  for (const auto& module : routeModules_) {
    module(router, routeContext);
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
