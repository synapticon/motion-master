#pragma once

#include <uwebsockets/App.h>

#include <atomic>
#include <string>
#include <thread>
#include <unordered_set>

/// @brief Combined HTTPS + WebSocket server.
///
/// Hosts the REST API (swagger spec, version endpoint, CORS preflight) and a
/// single monitoring WebSocket at `/ws`.  Runs on a dedicated background thread
/// started by start() and torn down by stop().
///
/// All public methods are thread-safe.  broadcast() may be called from any
/// thread, including the RT GameLoop thread; the message is forwarded to the
/// uWebSockets event loop via defer() so that the caller never blocks.
class Server {
 public:
  /// @brief Server configuration.
  struct Config {
    uint16_t port = 8443;      ///< TCP port to listen on (TLS).
    std::string cert_file;     ///< Path to the TLS certificate (PEM).
    std::string key_file;      ///< Path to the TLS private key (PEM).
    std::string version;       ///< Application version string served at `GET /api/version`.
    std::string swagger_file;  ///< Path to `swagger.yml`; served at `GET /api/swagger.yml`.
  };

  /// @brief Constructs the server with the given configuration.
  /// @param config  Server parameters.  The config is copied internally.
  explicit Server(Config config);

  /// @brief Destructor.  Calls stop() if the server is still running.
  ~Server();

  /// @brief Starts the server background thread and begins accepting connections.
  ///
  /// Idempotent: a second call while already running is a no-op.
  void start();

  /// @brief Stops the server, closes the listen socket, and joins the thread.
  ///
  /// Idempotent: a second call after the server has already stopped is a no-op.
  void stop();

  /// @brief Sends a JSON message to all currently-connected WebSocket clients.
  ///
  /// The message is queued on the server's event loop via defer() and delivered
  /// asynchronously.  Safe to call from the RT loop thread.
  ///
  /// @param json  Serialised JSON string.  Moved into the deferred closure.
  void broadcast(std::string json);

 private:
  struct WsData {};

  void run();

  Config config_;
  std::atomic<bool> running_{false};
  std::thread thread_;
  std::atomic<uWS::Loop*> loop_{nullptr};
  std::atomic<us_listen_socket_t*> listen_token_{nullptr};
  std::unordered_set<uWS::WebSocket<true, true, WsData>*> connections_;
};
