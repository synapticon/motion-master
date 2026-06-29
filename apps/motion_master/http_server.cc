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
#include "comm/object_data_types.h"
#include "comm/sii.h"
#include "core/system_info.h"
#include "core/util.h"
#include "monitoring_api.h"
#include "node/device_manager.h"
#include "node/device_parameter.h"
#include "node/monitoring_manager.h"

namespace {

// Parses the optional comma-separated "positions" query into 1-based slave positions. An absent
// or empty parameter yields an empty vector (which the device manager reads as "all devices"). On
// a malformed token it writes a 400 (with the CORS header) to @p res and returns nullopt — the
// caller must return immediately without writing a further response.
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
      res->writeStatus("400 Bad Request")
          ->writeHeader("Access-Control-Allow-Origin", corsOrigin)
          ->end();
      return std::nullopt;
    }
    positions.push_back(pos);
  }
  return positions;
}

// The JSON/error/status response helpers live in api/web_api.h so route plug-ins can share the
// exact same response shape (content type + CORS) as the built-in routes. Pull them in unqualified
// so the call sites below read the same as before.
using mm::api::sendError;
using mm::api::sendJson;
using mm::api::sendStatus;

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
// the kCertExpiryWarningDays window so the PWA can prompt the user to download a fresh release.
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
          {"notBefore", toIso8601Utc(info.notBefore)},
          {"notAfter", toIso8601Utc(info.notAfter)},
          {"daysRemaining", daysRemaining},
          {"expired", expired},
          {"expiresSoon", expired || daysRemaining < mm::kCertExpiryWarningDays},
          {"chain", chain}};
}

}  // namespace

HttpServer::HttpServer(Config config, mm::node::DeviceManager& deviceManager,
                       mm::node::MonitoringManager& monitoringManager)
    : config_(std::move(config)),
      deviceManager_(deviceManager),
      monitoringManager_(monitoringManager) {}

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

void HttpServer::start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    return;
  }
  thread_ = std::thread([this]() { run(); });
}

void HttpServer::stop() {
  bool expected = true;
  if (!running_.compare_exchange_strong(expected, false)) {
    return;
  }

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
                    "<li><a href=\"/api/log\">Log</a></li>"
                    "<li><a href=\"/api/registers\">ESC registers</a></li>"
                    "</ul>"
                    "</body></html>");
          })
      .get("/api/adapters",
           [this](auto* res, auto* /*req*/) {
             auto adapterMap = mm::comm::mapMacAddressesToInterfaces();
             nlohmann::json arr = nlohmann::json::array();
             for (const auto& [mac, name] : adapterMap) {
               arr.push_back({{"mac", mac}, {"name", name}});
             }
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
             res->writeHeader("Content-Type", "text/plain; charset=utf-8")
                 ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                 ->end(body);
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
      .get("/api/devices/state",
           [this](auto* res, auto* req) {
             auto positions = parsePositions(res, req, config_.corsOrigin);
             if (!positions) {
               return;  // parsePositions already wrote the 400 response
             }
             auto r = deviceManager_.getDeviceStates(*positions);
             if (!r) {
               sendError(res, "500 Internal Server Error", config_.corsOrigin, r.error());
               return;
             }
             sendJson(res, config_.corsOrigin, nlohmann::json(*r));
           })
      .get("/api/devices/diagnostics",
           [this](auto* res, auto* req) {
             auto positions = parsePositions(res, req, config_.corsOrigin);
             if (!positions) {
               return;  // parsePositions already wrote the 400 response
             }
             auto r = deviceManager_.getDeviceDiagnostics(*positions);
             if (!r) {
               sendError(res, "500 Internal Server Error", config_.corsOrigin, r.error());
               return;
             }
             sendJson(res, config_.corsOrigin, nlohmann::json(*r));
           })
      .get("/api/dc-sync",
           [this](auto* res, auto* req) {
             auto positions = parsePositions(res, req, config_.corsOrigin);
             if (!positions) {
               return;  // parsePositions already wrote the 400 response
             }
             auto r = deviceManager_.getDcSync(*positions);
             if (!r) {
               sendError(res, "500 Internal Server Error", config_.corsOrigin, r.error());
               return;
             }
             sendJson(res, config_.corsOrigin, nlohmann::json(*r));
           })
      .get("/api/devices",
           [this](auto* res, auto* /*req*/) {
             sendJson(res, config_.corsOrigin, nlohmann::json(deviceManager_));
           })
      .get("/api/process-image",
           [this](auto* res, auto* /*req*/) {
             sendJson(res, config_.corsOrigin, nlohmann::json(deviceManager_.processImageInfo()));
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
             if (auto r = device->readRegister(address, buf); !r) {
               sendError(res, "500 Internal Server Error", config_.corsOrigin, r.error());
               return;
             }
             sendJson(res, config_.corsOrigin, nlohmann::json{{"data", buf}});
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
             auto raw = device->readSii();
             if (!raw) {
               sendError(res, "500 Internal Server Error", config_.corsOrigin, raw.error());
               return;
             }
             if (wantRaw) {
               res->writeHeader("Content-Type", "application/octet-stream")
                   ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                   ->end(std::string_view{reinterpret_cast<const char*>(raw->data()), raw->size()});
               return;
             }
             auto parsed = mm::comm::parseSii(*raw);
             if (!parsed) {
               sendError(res, "500 Internal Server Error", config_.corsOrigin, parsed.error());
               return;
             }
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
               if (auto r = device->writeSii(data); !r) {
                 sendError(res, "500 Internal Server Error", config_.corsOrigin, r.error());
                 return;
               }
               sendJson(res, config_.corsOrigin, nlohmann::json{{"ok", true}});
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
                if (auto r = device->writeRegister(address, data); !r) {
                  sendError(res, "500 Internal Server Error", config_.corsOrigin, r.error());
                  return;
                }
                sendJson(res, config_.corsOrigin, nlohmann::json{{"ok", true}});
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
             auto r = deviceManager_.getProcessDataWatchdog(pos);
             if (!r) {
               sendError(res, "500 Internal Server Error", config_.corsOrigin, r.error());
               return;
             }
             sendJson(res, config_.corsOrigin, watchdogJson(pos, *r));
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
               auto r = deviceManager_.setProcessDataWatchdog(pos, ns);
               if (!r) {
                 sendError(res, "500 Internal Server Error", config_.corsOrigin, r.error());
                 return;
               }
               sendJson(res, config_.corsOrigin, watchdogJson(pos, *r));
             });
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
             auto r = device->readSdo(*index, *subindex);
             if (!r) {
               sendError(res, "500 Internal Server Error", config_.corsOrigin, r.error());
               return;
             }
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
               if (auto r = device->writeSdo(*index, *subindex, data); !r) {
                 sendError(res, "500 Internal Server Error", config_.corsOrigin, r.error());
                 return;
               }
               sendJson(res, config_.corsOrigin, nlohmann::json{{"ok", true}});
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
             auto r = device->readFile(filename);
             if (!r) {
               sendError(res, "500 Internal Server Error", config_.corsOrigin, r.error());
               return;
             }
             res->writeHeader("Content-Type", "application/octet-stream")
                 ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                 ->end(std::string_view{reinterpret_cast<const char*>(r->data()), r->size()});
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
               if (auto r = device->writeFile(filename, data); !r) {
                 sendError(res, "500 Internal Server Error", config_.corsOrigin, r.error());
                 return;
               }
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
              if (auto r = deviceManager_.initializeDeviceParameters(pos, readValues); !r) {
                sendError(res, "500 Internal Server Error", config_.corsOrigin, r.error());
                return;
              }
              const auto* device = deviceManager_.findDevice(pos);
              sendJson(res, config_.corsOrigin, nlohmann::json(device->parametersOrdered()));
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
              if (auto r = deviceManager_.readAllDeviceParameters(pos); !r) {
                sendError(res, "500 Internal Server Error", config_.corsOrigin, r.error());
                return;
              }
              const auto* device = deviceManager_.findDevice(pos);
              sendJson(res, config_.corsOrigin, nlohmann::json(device->parametersOrdered()));
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
             auto r = deviceManager_.deviceParameterView(pos, *index, *subindex, refreshFromBus);
             if (!r) {
               sendError(res, "500 Internal Server Error", config_.corsOrigin, r.error());
               return;
             }
             sendJson(res, config_.corsOrigin, nlohmann::json(*r));
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
               // in the node layer (DeviceParameter::setValue).
               if (auto w = deviceManager_.writeDeviceParameter(pos, *index, *subindex,
                                                                std::move(value));
                   !w) {
                 sendError(res, "500 Internal Server Error", config_.corsOrigin, w.error());
                 return;
               }
               // Echo the updated parameter from the cache (no extra bus I/O) so the client
               // gets the coerced value and resulting syncState in the same round-trip.
               auto r = deviceManager_.deviceParameterView(pos, *index, *subindex, false);
               if (!r) {
                 sendJson(res, config_.corsOrigin, nlohmann::json{{"ok", true}});
                 return;
               }
               sendJson(res, config_.corsOrigin, nlohmann::json(*r));
             });
           })
      .get("/api/meta/data-types",
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
                if (!config_.initDriver) {
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
                  if (auto r = config_.initDriver(driver, adapter); !r) {
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
             res->writeHeader("Content-Type", "application/octet-stream")
                 ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
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
                res->writeStatus("201 Created")
                    ->writeHeader("Content-Type", "application/json")
                    ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
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
      .get("/api/parameter-caches",
           [this](auto* res, auto* /*req*/) {
             sendJson(res, config_.corsOrigin, deviceManager_.parameterCache().list());
           })
      .get("/api/parameter-caches/:id",
           [this](auto* res, auto* req) {
             auto raw = deviceManager_.parameterCache().readRaw(req->getParameter("id"));
             if (!raw) {
               sendStatus(res, "404 Not Found", config_.corsOrigin);
               return;
             }
             // The file is JSON; serve it verbatim so the client can save it as-is.
             res->writeHeader("Content-Type", "application/json")
                 ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                 ->end(std::string_view{reinterpret_cast<const char*>(raw->data()), raw->size()});
           })
      .del("/api/parameter-caches/:id", [this](auto* res, auto* req) {
        if (deviceManager_.parameterCache().remove(req->getParameter("id"))) {
          sendStatus(res, "204 No Content", config_.corsOrigin);
        } else {
          sendStatus(res, "404 Not Found", config_.corsOrigin);
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
                res->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                    ->writeHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS")
                    ->writeHeader("Access-Control-Allow-Headers", "Content-Type")
                    ->writeStatus("204 No Content")
                    ->end();
              })
      .listen("127.0.0.1", config_.port,
              [this](auto* token) {
                if (token) {
                  spdlog::info("HTTP server listening on port {}", config_.port);
                } else {
                  spdlog::error("HTTP server failed to listen on port {}", config_.port);
                  running_ = false;
                }
              })
      .run();

  app_.store(nullptr);
  spdlog::debug("HTTP server event loop stopped");
}
