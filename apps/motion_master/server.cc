#include "server.h"

#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

static constexpr std::string_view kCorsOrigin = "https://motion-master.synapticon.com";

Server::Server(Config config) : config_(std::move(config)) {}

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
      .key_file_name = config_.key_file.c_str(),
      .cert_file_name = config_.cert_file.c_str(),
  }}
      .get("/api/version",
           [this](auto* res, auto* /*req*/) {
             res->writeHeader("Content-Type", "application/json")
                 ->writeHeader("Access-Control-Allow-Origin", kCorsOrigin)
                 ->end(nlohmann::json{{"version", config_.version}}.dump());
           })
      .options("/api/*",
               [](auto* res, auto* /*req*/) {
                 res->writeHeader("Access-Control-Allow-Origin", kCorsOrigin)
                     ->writeHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS")
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
