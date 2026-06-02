#pragma once

#include <uwebsockets/App.h>

#include <atomic>
#include <expected>
#include <functional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace mm::node {
class DeviceManager;
}  // namespace mm::node

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
  /// @brief Callback type for `POST /api/init`.
  ///
  /// Receives the requested driver name (e.g. `"soem"`) and adapter string
  /// (interface name or MAC; may be empty for auto-detect).  The callback is
  /// responsible for constructing the concrete @c FieldbusDriver and calling
  /// @c DeviceManager::init().  Lives in the composition root (main.cc) so that
  /// concrete driver types are never referenced inside the server.
  using InitDriverFn =
      std::function<std::expected<void, std::string>(std::string driver, std::string adapter)>;

  /// @brief Callback type for `GET /api/log`.
  ///
  /// Returns a snapshot of all log entries collected since startup in
  /// chronological order.  Wired to @c RingLogSinkMt::entries() in main.cc.
  using GetLogFn = std::function<std::vector<std::string>()>;

  /// @brief Server configuration.
  struct Config {
    uint16_t port = 8443;     ///< TCP port to listen on (TLS).
    std::string certFile;     ///< Path to the TLS certificate (PEM).
    std::string keyFile;      ///< Path to the TLS private key (PEM).
    std::string version;      ///< Application version string served at `GET /api/version`.
    std::string swaggerFile;  ///< Path to `swagger.yml`; served at `GET /api/swagger.yml`.
    InitDriverFn initDriver;  ///< Handler for `POST /api/init`; required for API-driven init.
    GetLogFn getLog;          ///< Handler for `GET /api/log`; returns buffered log entries.
    /// Value sent in `Access-Control-Allow-Origin`. Defaults to the production PWA origin.
    std::string corsOrigin{"https://motion-master.synapticon.com"};
  };

  /// @brief Constructs the server with the given configuration.
  /// @param config         Server parameters.  The config is copied internally.
  /// @param deviceManager  Device list source; lifetime must exceed that of this object.
  Server(Config config, mm::node::DeviceManager& deviceManager);

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

  /// @brief Publishes a JSON message to the WebSocket clients subscribed to @p topic.
  ///
  /// Uses uWebSockets' native topic pub/sub, so only clients that sent @c {"subscribe":"<topic>"}
  /// receive it (publishing to a topic with no subscribers is a cheap no-op).  Like broadcast(),
  /// the send is deferred onto the event loop, so this is safe to call from any thread — in
  /// particular the monitoring sampler thread.
  ///
  /// @param topic  Topic to publish under (a monitoring id).  Moved into the deferred closure.
  /// @param json   Serialised JSON string.  Moved into the deferred closure.
  void publish(std::string topic, std::string json);

 private:
  struct WsData {};

  void run();

  Config config_;
  mm::node::DeviceManager& deviceManager_;
  std::atomic<bool> running_{false};
  std::thread thread_;
  std::atomic<uWS::Loop*> loop_{nullptr};
  std::atomic<uWS::SSLApp*> app_{nullptr};
  std::unordered_set<uWS::WebSocket<true, true, WsData>*> connections_;
};
