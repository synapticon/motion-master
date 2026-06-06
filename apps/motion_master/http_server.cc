#include "http_server.h"

#include <spdlog/spdlog.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <ctime>
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

// Writes @p body as a 200 application/json response with the CORS header.
template <typename Res>
void sendJson(Res* res, std::string_view corsOrigin, const nlohmann::json& body) {
  res->writeHeader("Content-Type", "application/json")
      ->writeHeader("Access-Control-Allow-Origin", corsOrigin)
      ->end(body.dump());
}

// Writes a @p status response carrying a {"error": message} JSON body and the CORS header.
template <typename Res>
void sendError(Res* res, std::string_view status, std::string_view corsOrigin,
               std::string_view message) {
  res->writeStatus(status)
      ->writeHeader("Content-Type", "application/json")
      ->writeHeader("Access-Control-Allow-Origin", corsOrigin)
      ->end(nlohmann::json{{"error", std::string(message)}}.dump());
}

// Writes a bare @p status response (no body) with the CORS header.
template <typename Res>
void sendStatus(Res* res, std::string_view status, std::string_view corsOrigin) {
  res->writeStatus(status)->writeHeader("Access-Control-Allow-Origin", corsOrigin)->end();
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
  return {{"path", path},
          {"subject", info.subject},
          {"issuer", info.issuer},
          {"notBefore", toIso8601Utc(info.notBefore)},
          {"notAfter", toIso8601Utc(info.notAfter)},
          {"daysRemaining", daysRemaining},
          {"expired", expired},
          {"expiresSoon", expired || daysRemaining < mm::kCertExpiryWarningDays}};
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

  std::move(app)
      .get("/",
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
              // The fresh cert is on disk, but uSockets bound the old one at listen — restart to
              // apply. Report the newly installed cert's details plus the restart hint.
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
      .get("/api/devices/:slavePosition/online",
           [this](auto* res, auto* req) {
             uint16_t pos{};
             auto param = req->getParameter("slavePosition");
             auto [ptr, ec] = std::from_chars(param.data(), param.data() + param.size(), pos);
             if (ec != std::errc() || ptr != param.data() + param.size()) {
               sendStatus(res, "400 Bad Request", config_.corsOrigin);
               return;
             }
             if (!deviceManager_.findDevice(pos)) {
               sendStatus(res, "404 Not Found", config_.corsOrigin);
               return;
             }
             auto r = deviceManager_.isDeviceOnline(pos);
             if (!r) {
               sendError(res, "500 Internal Server Error", config_.corsOrigin, r.error());
               return;
             }
             sendJson(res, config_.corsOrigin,
                      nlohmann::json{{"slavePosition", pos}, {"online", *r}});
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
             auto r = device->upload(*index, *subindex);
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
               if (auto r = device->download(*index, *subindex, data); !r) {
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
                  sendError(
                      res, "400 Bad Request", config_.corsOrigin,
                      "invalid state: use 1 (Init), 2 (PreOp), 3 (Boot), 4 (SafeOp), or 8 (Op)");
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
      .options("/api/*",
               [this](auto* res, auto* /*req*/) {
                 res->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                     ->writeHeader("Access-Control-Allow-Methods",
                                   "GET, POST, PUT, DELETE, OPTIONS")
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
