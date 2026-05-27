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
#include "core/util.h"
#include "node/device_manager.h"

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
    // Close the listen socket and all WebSocket connections from the event-loop
    // thread so us_loop_run() exits naturally when num_polls reaches 0.
    // The internal sweep_timer, dateTimer, and wakeup_async are all created with
    // fallthrough=1, so they do not contribute to num_polls and do not prevent
    // loop exit. Calling loop->free() from inside wakeupCb would be a
    // use-after-free: wakeupCb clears the deferred queue after each callback
    // returns, but free() destroys LoopData (which owns those queues) mid-drain.
    // The thread-local LoopCleaner calls loop->free() safely at thread exit.
    loop->defer([this, token = listen_token_.exchange(nullptr)]() {
      if (token) {
        us_listen_socket_close(0, token);
      }
      // Snapshot before iterating: closing triggers the close callback which
      // erases from connections_.
      //
      // close() (hard us_socket_close) — not end() (graceful close frame +
      // half-close). end() only shuts down the write side and leaves the socket
      // in the poll set waiting for the peer's close handshake; an idle or
      // unresponsive client (e.g. the monitoring PWA) never replies, so
      // num_polls never reaches 0, us_loop_run() never returns, and the join()
      // below blocks forever. On shutdown we want the connection gone now.
      auto snapshot = connections_;
      for (auto* ws : snapshot) {
        ws->close();
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

  uWS::SSLApp{uWS::SocketContextOptions{
                  .key_file_name = config_.keyFile.c_str(),
                  .cert_file_name = config_.certFile.c_str(),
              }}
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
                if (auto r = deviceManager_.transitionToState(positions, targetState,
                                                              std::chrono::milliseconds(timeoutMs));
                    !r) {
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
                  listen_token_.store(token);
                  spdlog::info("Server listening on port {}", config_.port);
                } else {
                  spdlog::error("Server failed to listen on port {}", config_.port);
                  running_ = false;
                }
              })
      .run();

  spdlog::debug("Server event loop stopped");
}
