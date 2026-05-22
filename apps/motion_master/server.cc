#include "server.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <utility>

#include "comm/base.h"
#include "node/device_manager.h"

static constexpr std::string_view kCorsOrigin = "https://motion-master.synapticon.com";

Server::Server(Config config, mm::node::DeviceManager& deviceManager)
    : config_(std::move(config)), deviceManager_(deviceManager) {}

Server::~Server() { stop(); }

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
    if (auto* token = listen_token_.exchange(nullptr)) {
      loop->defer([token]() { us_listen_socket_close(0, token); });
    } else {
      loop->defer([]() {});
    }
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
      .get("/api/swagger.yml",
           [swaggerContent](auto* res, auto* /*req*/) {
             if (swaggerContent.empty()) {
               res->writeStatus("404 Not Found")->end();
               return;
             }
             // text/* renders inline in the browser; non-text MIME types trigger a download.
             res->writeHeader("Content-Type", "text/yaml; charset=utf-8")
                 ->writeHeader("Content-Disposition", "inline")
                 ->writeHeader("Access-Control-Allow-Origin", kCorsOrigin)
                 ->end(swaggerContent);
           })
      .get("/api/adapters",
           [](auto* res, auto* /*req*/) {
             auto adapterMap = mm::comm::mapMacAddressesToInterfaces();
             nlohmann::json arr = nlohmann::json::array();
             for (const auto& [mac, name] : adapterMap) {
               arr.push_back({{"mac", mac}, {"name", name}});
             }
             res->writeHeader("Content-Type", "application/json")
                 ->writeHeader("Access-Control-Allow-Origin", kCorsOrigin)
                 ->end(arr.dump());
           })
      .get("/api/version",
           [this](auto* res, auto* /*req*/) {
             res->writeHeader("Content-Type", "application/json")
                 ->writeHeader("Access-Control-Allow-Origin", kCorsOrigin)
                 ->end(nlohmann::json{{"version", config_.version}}.dump());
           })
      .get("/api/devices",
           [this](auto* res, auto* /*req*/) {
             res->writeHeader("Content-Type", "application/json")
                 ->writeHeader("Access-Control-Allow-Origin", kCorsOrigin)
                 ->end(nlohmann::json(deviceManager_).dump());
           })
      .get("/api/devices/:slavePosition",
           [this](auto* res, auto* req) {
             uint16_t pos{};
             auto param = req->getParameter("slavePosition");
             auto [ptr, ec] = std::from_chars(param.data(), param.data() + param.size(), pos);
             if (ec != std::errc() || ptr != param.data() + param.size()) {
               res->writeStatus("400 Bad Request")
                   ->writeHeader("Access-Control-Allow-Origin", kCorsOrigin)
                   ->end();
               return;
             }
             const auto& devices = deviceManager_.devices();
             auto it = std::find_if(devices.begin(), devices.end(),
                                    [pos](const auto& d) { return d.slavePosition() == pos; });
             if (it == devices.end()) {
               res->writeStatus("404 Not Found")
                   ->writeHeader("Access-Control-Allow-Origin", kCorsOrigin)
                   ->end();
               return;
             }
             res->writeHeader("Content-Type", "application/json")
                 ->writeHeader("Access-Control-Allow-Origin", kCorsOrigin)
                 ->end(nlohmann::json(*it).dump());
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
                      ->writeHeader("Access-Control-Allow-Origin", kCorsOrigin)
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
                        ->writeHeader("Access-Control-Allow-Origin", kCorsOrigin)
                        ->end(nlohmann::json{{"error", r.error()}}.dump());
                    return;
                  }
                  res->writeHeader("Content-Type", "application/json")
                      ->writeHeader("Access-Control-Allow-Origin", kCorsOrigin)
                      ->end(nlohmann::json{{"ok", true}}.dump());
                } catch (const nlohmann::json::exception& e) {
                  res->writeStatus("400 Bad Request")
                      ->writeHeader("Content-Type", "application/json")
                      ->writeHeader("Access-Control-Allow-Origin", kCorsOrigin)
                      ->end(nlohmann::json{{"error", e.what()}}.dump());
                }
              });
            })
      .post("/api/configure",
            [this](auto* res, auto* /*req*/) {
              auto aborted = std::make_shared<bool>(false);
              res->onAborted([aborted]() { *aborted = true; });
              res->onData([this, res, aborted](std::string_view /*data*/, bool last) {
                if (!last) return;
                if (*aborted) return;
                if (auto r = deviceManager_.configure(); !r) {
                  res->writeStatus("500 Internal Server Error")
                      ->writeHeader("Content-Type", "application/json")
                      ->writeHeader("Access-Control-Allow-Origin", kCorsOrigin)
                      ->end(nlohmann::json{{"error", r.error()}}.dump());
                  return;
                } else {
                  res->writeHeader("Content-Type", "application/json")
                      ->writeHeader("Access-Control-Allow-Origin", kCorsOrigin)
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
                    ->writeHeader("Access-Control-Allow-Origin", kCorsOrigin)
                    ->end(nlohmann::json{{"ok", true}}.dump());
              });
            })
      .options("/api/*",
               [](auto* res, auto* /*req*/) {
                 res->writeHeader("Access-Control-Allow-Origin", kCorsOrigin)
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
