#include "ws_server.h"

#include <spdlog/spdlog.h>

#include <string>
#include <string_view>
#include <utility>

#include "monitoring_api.h"

WebSocketServer::WebSocketServer(Config config) : config_(std::move(config)) {}

WebSocketServer::~WebSocketServer() {
  stop();
  // stop() returns early when running_ was already false (listen failed), leaving thread_ joinable.
  if (thread_.joinable()) {
    thread_.join();
  }
}

bool WebSocketServer::start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    return true;  // already running
  }
  thread_ = std::thread([this]() { run(); });
  // Block until the listen callback fires on the loop thread, so start() reports the bind outcome
  // synchronously to the composition root (which treats a failure as fatal).
  return listenResult_.get_future().get();
}

void WebSocketServer::stop() {
  bool expected = true;
  if (!running_.compare_exchange_strong(expected, false)) {
    return;
  }

  // Defer App::close() onto the loop so it closes the listen socket and every connection, draining
  // the loop so run() returns. (See HttpServer::stop() for the keep-alive rationale.)
  if (auto* loop = loop_.load()) {
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

void WebSocketServer::broadcast(std::string json) {
  if (auto* loop = loop_.load()) {
    loop->defer([this, json = std::move(json)]() {
      for (auto* ws : connections_) {
        ws->send(json, uWS::OpCode::TEXT);
      }
    });
  }
}

void WebSocketServer::publish(std::string topic, std::string json) {
  // Deliver only to clients subscribed to this topic, via uWebSockets' native pub/sub. Deferred
  // onto the event loop so any thread — the monitoring sampler — can call it without touching the
  // app off-loop.
  if (auto* loop = loop_.load()) {
    loop->defer([this, topic = std::move(topic), json = std::move(json)]() {
      if (auto* app = app_.load()) {
        app->publish(topic, json, uWS::OpCode::TEXT);
      }
    });
  }
}

void WebSocketServer::run() {
  loop_.store(uWS::Loop::get());

  uWS::SSLApp::WebSocketBehavior<WsData> wsBehavior{};
  wsBehavior.open = [this](auto* ws) {
    connections_.insert(ws);
    spdlog::debug("WebSocket connected, total: {}", connections_.size());
  };
  wsBehavior.close = [this](auto* ws, int /*code*/, std::string_view /*msg*/) {
    connections_.erase(ws);
    spdlog::debug("WebSocket disconnected, total: {}", connections_.size());
  };
  // Inbound control messages. Today: topic subscribe/unsubscribe (uWS removes the socket from all
  // its topics on close, so there is nothing to undo). Future inbound commands — notably staging
  // process-data output values for the RT loop — dispatch from here.
  wsBehavior.message = [](auto* ws, std::string_view message, uWS::OpCode /*opCode*/) {
    if (auto cmd = mm::parseWsCommand(message)) {
      if (cmd->action == mm::WsCommand::Action::Subscribe) {
        ws->subscribe(cmd->topic);
      } else {
        ws->unsubscribe(cmd->topic);
      }
    }
  };

  uWS::SSLApp app{uWS::SocketContextOptions{
      .key_file_name = config_.keyFile.c_str(),
      .cert_file_name = config_.certFile.c_str(),
  }};
  app_.store(&app);

  // Path-agnostic: the whole port is the WebSocket, so the URL needs no path (clients connect to
  // the bare `wss://host:<port>`). `/*` still accepts a legacy `/ws` path.
  std::move(app)
      .ws<WsData>("/*", std::move(wsBehavior))
      .listen(config_.bindAddress, config_.port, LIBUS_LISTEN_EXCLUSIVE_PORT,
              [this](auto* token) {
                if (token) {
                  spdlog::info("WebSocket server listening on {}:{}", config_.bindAddress,
                               config_.port);
                  listenResult_.set_value(true);
                } else {
                  spdlog::error("WebSocket server failed to listen on {}:{} (already in use?)",
                                config_.bindAddress, config_.port);
                  running_ = false;
                  listenResult_.set_value(false);
                }
              })
      .run();

  app_.store(nullptr);
  spdlog::debug("WebSocket server event loop stopped");
}
