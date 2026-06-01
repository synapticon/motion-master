#include "server.h"

#include <spdlog/spdlog.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "comm/al_status_codes.h"
#include "comm/base.h"
#include "comm/esc_registers.h"
#include "comm/fieldbus_driver.h"
#include "comm/foe_error_codes.h"
#include "comm/object_data_types.h"
#include "core/util.h"
#include "node/device_manager.h"
#include "node/device_parameter.h"

Server::Server(Config config, mm::node::DeviceManager& deviceManager)
    : config_(std::move(config)), deviceManager_(deviceManager) {}

Server::~Server() {
  stop();
  // stop() returns early when running_ was already false (listen failed), leaving thread_ joinable.
  if (thread_.joinable()) {
    thread_.join();
  }
}

void Server::start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    return;
  }
  thread_ = std::thread([this]() { run(); });
}

void Server::stop() {
  bool expected = true;
  if (!running_.compare_exchange_strong(expected, false)) {
    return;
  }

  if (auto* loop = loop_.load()) {
    // App::close() iterates the HTTP context and every WebSocket context and calls
    // us_socket_context_close on each, which closes the listen socket *and* every
    // regular socket (incl. idle HTTP keep-alive connections) in those contexts.
    // Each closed socket lands on the loop's closed_head queue and gets freed in
    // the next loop_post, dropping num_polls and letting us_loop_run() exit.
    //
    // Manually closing only the listen socket leaves keep-alive HTTP connections
    // alive in httpContext->head_sockets — they aren't tracked in connections_
    // (which only holds WebSockets), so num_polls stays > 0 and the loop blocks
    // until each connection hits its idle timeout, hanging shutdown for minutes
    // after a client has talked to the API.
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

void Server::broadcast(std::string json) {
  if (auto* loop = loop_.load()) {
    loop->defer([this, json = std::move(json)]() {
      for (auto* ws : connections_) {
        ws->send(json, uWS::OpCode::TEXT);
      }
    });
  }
}

void Server::run() {
  // Read once at startup: the spec is static for the lifetime of the server, and
  // failing early here surfaces a missing file before any client connects.
  std::string swaggerContent;
  if (!config_.swaggerFile.empty()) {
    std::ifstream f{config_.swaggerFile};
    if (f) {
      std::ostringstream ss;
      ss << f.rdbuf();
      swaggerContent = ss.str();
    } else {
      spdlog::warn("Could not read swagger file: {}", config_.swaggerFile);
    }
  }

  loop_.store(uWS::Loop::get());

  uWS::SSLApp::WebSocketBehavior<WsData> ws_behavior{};
  ws_behavior.open = [this](auto* ws) {
    connections_.insert(ws);
    spdlog::debug("WebSocket connected, total: {}", connections_.size());
  };
  ws_behavior.close = [this](auto* ws, int /*code*/, std::string_view /*msg*/) {
    connections_.erase(ws);
    spdlog::debug("WebSocket disconnected, total: {}", connections_.size());
  };

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
                     "<li><a href=\"/api/swagger.yml\">API specification (swagger.yml)</a></li>"
                     "<li><a href=\"/api/log\">Log</a></li>"
                     "<li><a href=\"/api/registers\">ESC registers</a></li>"
                     "</ul>"
                     "</body></html>");
           })
      .get("/api/swagger.yml",
           [this, swaggerContent](auto* res, auto* /*req*/) {
             if (swaggerContent.empty()) {
               res->writeStatus("404 Not Found")->end();
               return;
             }
             // text/* renders inline in the browser; non-text MIME types trigger a download.
             res->writeHeader("Content-Type", "text/yaml; charset=utf-8")
                 ->writeHeader("Content-Disposition", "inline")
                 ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                 ->end(swaggerContent);
           })
      .get("/api/adapters",
           [this](auto* res, auto* /*req*/) {
             auto adapterMap = mm::comm::mapMacAddressesToInterfaces();
             nlohmann::json arr = nlohmann::json::array();
             for (const auto& [mac, name] : adapterMap) {
               arr.push_back({{"mac", mac}, {"name", name}});
             }
             res->writeHeader("Content-Type", "application/json")
                 ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                 ->end(arr.dump());
           })
      .get("/api/version",
           [this](auto* res, auto* /*req*/) {
             res->writeHeader("Content-Type", "application/json")
                 ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                 ->end(nlohmann::json{{"version", config_.version}}.dump());
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
             res->writeHeader("Content-Type", "application/json")
                 ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                 ->end(nlohmann::json(mm::comm::kEscRegisters).dump());
           })
      .get("/api/meta/al-status-codes",
           [this](auto* res, auto* /*req*/) {
             res->writeHeader("Content-Type", "application/json")
                 ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                 ->end(nlohmann::json(mm::comm::kAlStatusCodes).dump());
           })
      .get("/api/meta/foe-error-codes",
           [this](auto* res, auto* /*req*/) {
             res->writeHeader("Content-Type", "application/json")
                 ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                 ->end(nlohmann::json(mm::comm::kFoeErrorCodes).dump());
           })
      .get("/api/devices/state",
           [this](auto* res, auto* req) {
             std::vector<uint16_t> positions;
             auto posParam = req->getQuery("positions");
             if (!posParam.empty()) {
               std::string posStr(posParam);
               std::istringstream ss(posStr);
               std::string token;
               while (std::getline(ss, token, ',')) {
                 uint16_t pos{};
                 auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), pos);
                 if (ec != std::errc() || ptr != token.data() + token.size()) {
                   res->writeStatus("400 Bad Request")
                       ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                       ->end();
                   return;
                 }
                 positions.push_back(pos);
               }
             }
             auto r = deviceManager_.getDeviceStates(positions);
             if (!r) {
               res->writeStatus("500 Internal Server Error")
                   ->writeHeader("Content-Type", "application/json")
                   ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                   ->end(nlohmann::json{{"error", r.error()}}.dump());
               return;
             }
             res->writeHeader("Content-Type", "application/json")
                 ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                 ->end(nlohmann::json(*r).dump());
           })
      .get("/api/devices",
           [this](auto* res, auto* /*req*/) {
             res->writeHeader("Content-Type", "application/json")
                 ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                 ->end(nlohmann::json(deviceManager_).dump());
           })
      .get("/api/process-image",
           [this](auto* res, auto* /*req*/) {
             res->writeHeader("Content-Type", "application/json")
                 ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                 ->end(nlohmann::json(deviceManager_.processImageInfo()).dump());
           })
      .get("/api/bus-config",
           [this](auto* res, auto* /*req*/) {
             res->writeHeader("Content-Type", "application/json")
                 ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                 ->end(nlohmann::json(deviceManager_.busConfig()).dump());
           })
      .get("/api/devices/:slavePosition",
           [this](auto* res, auto* req) {
             uint16_t pos{};
             auto param = req->getParameter("slavePosition");
             auto [ptr, ec] = std::from_chars(param.data(), param.data() + param.size(), pos);
             if (ec != std::errc() || ptr != param.data() + param.size()) {
               res->writeStatus("400 Bad Request")
                   ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                   ->end();
               return;
             }
             const auto* device = deviceManager_.findDevice(pos);
             if (!device) {
               res->writeStatus("404 Not Found")
                   ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                   ->end();
               return;
             }
             res->writeHeader("Content-Type", "application/json")
                 ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                 ->end(nlohmann::json(*device).dump());
           })
      .get("/api/devices/:slavePosition/online",
           [this](auto* res, auto* req) {
             uint16_t pos{};
             auto param = req->getParameter("slavePosition");
             auto [ptr, ec] = std::from_chars(param.data(), param.data() + param.size(), pos);
             if (ec != std::errc() || ptr != param.data() + param.size()) {
               res->writeStatus("400 Bad Request")
                   ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                   ->end();
               return;
             }
             if (!deviceManager_.findDevice(pos)) {
               res->writeStatus("404 Not Found")
                   ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                   ->end();
               return;
             }
             auto r = deviceManager_.isDeviceOnline(pos);
             if (!r) {
               res->writeStatus("500 Internal Server Error")
                   ->writeHeader("Content-Type", "application/json")
                   ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                   ->end(nlohmann::json{{"error", r.error()}}.dump());
               return;
             }
             res->writeHeader("Content-Type", "application/json")
                 ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                 ->end(nlohmann::json{{"slavePosition", pos}, {"online", *r}}.dump());
           })
      .get("/api/devices/:slavePosition/registers/:address",
           [this](auto* res, auto* req) {
             uint16_t pos{};
             auto posParam = req->getParameter("slavePosition");
             auto [p1, ec1] =
                 std::from_chars(posParam.data(), posParam.data() + posParam.size(), pos);
             if (ec1 != std::errc() || p1 != posParam.data() + posParam.size()) {
               res->writeStatus("400 Bad Request")
                   ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                   ->end();
               return;
             }
             uint16_t address{};
             auto addrParam = req->getParameter("address");
             auto [p2, ec2] =
                 std::from_chars(addrParam.data(), addrParam.data() + addrParam.size(), address);
             if (ec2 != std::errc() || p2 != addrParam.data() + addrParam.size()) {
               res->writeStatus("400 Bad Request")
                   ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                   ->end();
               return;
             }
             uint16_t length{};
             auto lenParam = req->getQuery("length");
             auto [p3, ec3] =
                 std::from_chars(lenParam.data(), lenParam.data() + lenParam.size(), length);
             if (ec3 != std::errc() || p3 != lenParam.data() + lenParam.size() || length == 0) {
               res->writeStatus("400 Bad Request")
                   ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                   ->end();
               return;
             }
             const auto* device = deviceManager_.findDevice(pos);
             if (!device) {
               res->writeStatus("404 Not Found")
                   ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                   ->end();
               return;
             }
             std::vector<uint8_t> buf(length);
             if (auto r = device->readRegister(address, buf); !r) {
               res->writeStatus("500 Internal Server Error")
                   ->writeHeader("Content-Type", "application/json")
                   ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                   ->end(nlohmann::json{{"error", r.error()}}.dump());
               return;
             }
             res->writeHeader("Content-Type", "application/json")
                 ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                 ->end(nlohmann::json{{"data", buf}}.dump());
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
                  res->writeStatus("400 Bad Request")
                      ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                      ->end();
                  return;
                }
                std::vector<uint8_t> data;
                try {
                  nlohmann::json j = nlohmann::json::parse(*body);
                  data = j.at("data").get<std::vector<uint8_t>>();
                } catch (const nlohmann::json::exception& e) {
                  res->writeStatus("400 Bad Request")
                      ->writeHeader("Content-Type", "application/json")
                      ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                      ->end(nlohmann::json{{"error", e.what()}}.dump());
                  return;
                }
                const auto* device = deviceManager_.findDevice(pos);
                if (!device) {
                  res->writeStatus("404 Not Found")
                      ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                      ->end();
                  return;
                }
                if (auto r = device->writeRegister(address, data); !r) {
                  res->writeStatus("500 Internal Server Error")
                      ->writeHeader("Content-Type", "application/json")
                      ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                      ->end(nlohmann::json{{"error", r.error()}}.dump());
                  return;
                }
                res->writeHeader("Content-Type", "application/json")
                    ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                    ->end(nlohmann::json{{"ok", true}}.dump());
              });
            })
      .get("/api/devices/:slavePosition/sdo/:index/:subindex",
           [this](auto* res, auto* req) {
             uint16_t pos{};
             auto posParam = req->getParameter("slavePosition");
             auto [p1, ec1] =
                 std::from_chars(posParam.data(), posParam.data() + posParam.size(), pos);
             if (ec1 != std::errc() || p1 != posParam.data() + posParam.size()) {
               res->writeStatus("400 Bad Request")
                   ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                   ->end();
               return;
             }
             auto index = mm::core::parseHexOrDec<uint16_t>(req->getParameter("index"));
             if (!index) {
               res->writeStatus("400 Bad Request")
                   ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                   ->end();
               return;
             }
             auto subindex = mm::core::parseHexOrDec<uint8_t>(req->getParameter("subindex"));
             if (!subindex) {
               res->writeStatus("400 Bad Request")
                   ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                   ->end();
               return;
             }
             const auto* device = deviceManager_.findDevice(pos);
             if (!device) {
               res->writeStatus("404 Not Found")
                   ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                   ->end();
               return;
             }
             auto r = device->upload(*index, *subindex);
             if (!r) {
               res->writeStatus("500 Internal Server Error")
                   ->writeHeader("Content-Type", "application/json")
                   ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                   ->end(nlohmann::json{{"error", r.error()}}.dump());
               return;
             }
             res->writeHeader("Content-Type", "application/json")
                 ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                 ->end(nlohmann::json{{"data", *r}}.dump());
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
                 res->writeStatus("400 Bad Request")
                     ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                     ->end();
                 return;
               }
               std::vector<uint8_t> data;
               try {
                 nlohmann::json j = nlohmann::json::parse(*body);
                 data = j.at("data").get<std::vector<uint8_t>>();
               } catch (const nlohmann::json::exception& e) {
                 res->writeStatus("400 Bad Request")
                     ->writeHeader("Content-Type", "application/json")
                     ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                     ->end(nlohmann::json{{"error", e.what()}}.dump());
                 return;
               }
               const auto* device = deviceManager_.findDevice(pos);
               if (!device) {
                 res->writeStatus("404 Not Found")
                     ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                     ->end();
                 return;
               }
               if (auto r = device->download(*index, *subindex, data); !r) {
                 res->writeStatus("500 Internal Server Error")
                     ->writeHeader("Content-Type", "application/json")
                     ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                     ->end(nlohmann::json{{"error", r.error()}}.dump());
                 return;
               }
               res->writeHeader("Content-Type", "application/json")
                   ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                   ->end(nlohmann::json{{"ok", true}}.dump());
             });
           })
      .get("/api/devices/:slavePosition/files/:filename",
           [this](auto* res, auto* req) {
             uint16_t pos{};
             auto posParam = req->getParameter("slavePosition");
             auto [p, ec] =
                 std::from_chars(posParam.data(), posParam.data() + posParam.size(), pos);
             if (ec != std::errc() || p != posParam.data() + posParam.size()) {
               res->writeStatus("400 Bad Request")
                   ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                   ->end();
               return;
             }
             std::string filename{req->getParameter("filename")};
             const auto* device = deviceManager_.findDevice(pos);
             if (!device) {
               res->writeStatus("404 Not Found")
                   ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                   ->end();
               return;
             }
             auto r = device->readFile(filename);
             if (!r) {
               res->writeStatus("500 Internal Server Error")
                   ->writeHeader("Content-Type", "application/json")
                   ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                   ->end(nlohmann::json{{"error", r.error()}}.dump());
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
                 res->writeStatus("400 Bad Request")
                     ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                     ->end();
                 return;
               }
               const auto* device = deviceManager_.findDevice(pos);
               if (!device) {
                 res->writeStatus("404 Not Found")
                     ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                     ->end();
                 return;
               }
               std::span<const uint8_t> data{reinterpret_cast<const uint8_t*>(body->data()),
                                             body->size()};
               if (auto r = device->writeFile(filename, data); !r) {
                 res->writeStatus("500 Internal Server Error")
                     ->writeHeader("Content-Type", "application/json")
                     ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                     ->end(nlohmann::json{{"error", r.error()}}.dump());
                 return;
               }
               res->writeHeader("Content-Type", "application/json")
                   ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                   ->end(nlohmann::json{{"ok", true}}.dump());
             });
           })
      .post("/api/devices/:slavePosition/parameters/init",
            [this](auto* res, auto* req) {
              uint16_t pos{};
              auto posParam = req->getParameter("slavePosition");
              auto [p, ec] =
                  std::from_chars(posParam.data(), posParam.data() + posParam.size(), pos);
              if (ec != std::errc() || p != posParam.data() + posParam.size()) {
                res->writeStatus("400 Bad Request")
                    ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                    ->end();
                return;
              }
              auto rv = req->getQuery("readValues");
              bool readValues = rv == "true" || rv == "1";
              if (auto r = deviceManager_.initializeDeviceParameters(pos, readValues); !r) {
                res->writeStatus("500 Internal Server Error")
                    ->writeHeader("Content-Type", "application/json")
                    ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                    ->end(nlohmann::json{{"error", r.error()}}.dump());
                return;
              }
              const auto* device = deviceManager_.findDevice(pos);
              res->writeHeader("Content-Type", "application/json")
                  ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                  ->end(nlohmann::json(device->parametersOrdered()).dump());
            })
      .get("/api/devices/:slavePosition/parameters",
           [this](auto* res, auto* req) {
             uint16_t pos{};
             auto posParam = req->getParameter("slavePosition");
             auto [p, ec] =
                 std::from_chars(posParam.data(), posParam.data() + posParam.size(), pos);
             if (ec != std::errc() || p != posParam.data() + posParam.size()) {
               res->writeStatus("400 Bad Request")
                   ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                   ->end();
               return;
             }
             const auto* device = deviceManager_.findDevice(pos);
             if (!device) {
               res->writeStatus("404 Not Found")
                   ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                   ->end();
               return;
             }
             res->writeHeader("Content-Type", "application/json")
                 ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                 ->end(nlohmann::json(device->parametersOrdered()).dump());
           })
      .get("/api/meta/data-types",
           [this](auto* res, auto* /*req*/) {
             res->writeHeader("Content-Type", "application/json")
                 ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                 ->end(nlohmann::json(mm::comm::kObjectDataTypes).dump());
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
                  res->writeStatus("501 Not Implemented")
                      ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                      ->end();
                  return;
                }
                // init() is one-shot — reject a re-init (e.g. a browser refresh
                // replaying the stored session) with 409 so the client can tell
                // "already connected" apart from a genuine init failure (500).
                if (deviceManager_.initialised()) {
                  res->writeStatus("409 Conflict")
                      ->writeHeader("Content-Type", "application/json")
                      ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                      ->end(nlohmann::json{{"error", "already initialised — call reset() first"}}
                                .dump());
                  return;
                }
                try {
                  nlohmann::json j =
                      body->empty() ? nlohmann::json::object() : nlohmann::json::parse(*body);
                  std::string driver = j.value("driver", "soem");
                  std::string adapter = j.value("adapter", "");
                  if (auto r = config_.initDriver(driver, adapter); !r) {
                    res->writeStatus("500 Internal Server Error")
                        ->writeHeader("Content-Type", "application/json")
                        ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                        ->end(nlohmann::json{{"error", r.error()}}.dump());
                    return;
                  }
                  res->writeHeader("Content-Type", "application/json")
                      ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                      ->end(nlohmann::json{{"ok", true}}.dump());
                } catch (const nlohmann::json::exception& e) {
                  res->writeStatus("400 Bad Request")
                      ->writeHeader("Content-Type", "application/json")
                      ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                      ->end(nlohmann::json{{"error", e.what()}}.dump());
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
                  res->writeStatus("500 Internal Server Error")
                      ->writeHeader("Content-Type", "application/json")
                      ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                      ->end(nlohmann::json{{"error", r.error()}}.dump());
                  return;
                } else {
                  res->writeHeader("Content-Type", "application/json")
                      ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                      ->end(nlohmann::json{{"slaves", *r}}.dump());
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
                res->writeHeader("Content-Type", "application/json")
                    ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                    ->end(nlohmann::json{{"ok", true}}.dump());
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
                  res->writeStatus("400 Bad Request")
                      ->writeHeader("Content-Type", "application/json")
                      ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                      ->end(nlohmann::json{{"error", e.what()}}.dump());
                  return;
                }
                using S = mm::comm::EtherCatState;
                if (stateVal != static_cast<uint16_t>(S::Init) &&
                    stateVal != static_cast<uint16_t>(S::PreOp) &&
                    stateVal != static_cast<uint16_t>(S::Boot) &&
                    stateVal != static_cast<uint16_t>(S::SafeOp) &&
                    stateVal != static_cast<uint16_t>(S::Op)) {
                  res->writeStatus("400 Bad Request")
                      ->writeHeader("Content-Type", "application/json")
                      ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                      ->end(nlohmann::json{{"error",
                                            "invalid state: use 1 (Init), 2 (PreOp),"
                                            " 3 (Boot), 4 (SafeOp), or 8 (Op)"}}
                                .dump());
                  return;
                }
                auto targetState = static_cast<S>(stateVal);
                auto r = deviceManager_.transitionToState(positions, targetState,
                                                          std::chrono::milliseconds(timeoutMs));
                if (!r) {
                  res->writeStatus("500 Internal Server Error")
                      ->writeHeader("Content-Type", "application/json")
                      ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                      ->end(nlohmann::json{{"error", r.error()}}.dump());
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
                res->writeHeader("Content-Type", "application/json")
                    ->writeHeader("Access-Control-Allow-Origin", config_.corsOrigin)
                    ->end(nlohmann::json{{"ok", allReached}, {"devices", devices}}.dump());
              });
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
      .ws<WsData>("/ws", std::move(ws_behavior))
      .listen("127.0.0.1", config_.port,
              [this](auto* token) {
                if (token) {
                  spdlog::info("Server listening on port {}", config_.port);
                } else {
                  spdlog::error("Server failed to listen on port {}", config_.port);
                  running_ = false;
                }
              })
      .run();

  app_.store(nullptr);
  spdlog::debug("Server event loop stopped");
}
