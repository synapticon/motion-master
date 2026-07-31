#pragma once

#include <uwebsockets/App.h>

#include <atomic>
#include <cstdint>
#include <future>
#include <string>
#include <thread>
#include <unordered_set>

/// @brief WebSocket server — the single bidirectional connection between the PWA and the backend.
///
/// Runs on its own TLS port, event loop, and thread, fully isolated from the HTTP API server
/// (@c HttpServer).  Because the two run on separate loops, a slow or blocking HTTP handler can
/// never stall this connection: the loop only ever does non-blocking I/O.  The whole port is the
/// WebSocket, so it accepts the upgrade on any path — clients connect to the bare
/// `wss://host:port`.
///
/// Carries traffic in both directions:
///   - server → client: monitoring batches (published per topic), notifications (slaves changed,
///     watchdog, ...), and long-running procedure progress (firmware, calibration, ...);
///   - client → server: topic subscribe/unsubscribe, and process-data output values (target-map
///     writes staged for the RT loop).
///
/// Today the inbound path handles only subscribe/unsubscribe and the outbound path only monitoring
/// publishes; notifications, procedure progress, and output staging plug in here as those features
/// land.  All public methods are thread-safe — publish() and broadcast() may be called from any
/// thread; the send is marshalled onto this server's event loop via defer() so the caller never
/// blocks.
class WebSocketServer {
 public:
  /// @brief Server configuration.
  struct Config {
    /// Local address to bind. Loopback serves only this machine; "0.0.0.0" serves the network.
    std::string bindAddress{"127.0.0.1"};
    uint16_t port = 62281;  ///< TCP port to listen on (TLS).
    std::string certFile;   ///< Path to the TLS certificate (PEM).
    std::string keyFile;    ///< Path to the TLS private key (PEM).
  };

  /// @brief Constructs the server with the given configuration (copied internally).
  explicit WebSocketServer(Config config);

  /// @brief Destructor.  Calls stop() if the server is still running.
  ~WebSocketServer();

  /// @brief Starts the server background thread and begins accepting connections.
  ///
  /// Blocks until the listen attempt completes and reports whether the port was bound. The port is
  /// claimed exclusively (@c LIBUS_LISTEN_EXCLUSIVE_PORT), so a port already in use fails here
  /// instead of silently sharing it via @c SO_REUSEPORT. Idempotent: a second call while already
  /// running returns @c true.
  /// @return @c true if the server is listening; @c false if the port could not be bound.
  bool start();

  /// @brief Stops the server, closes the listen socket, and joins the thread. Idempotent.
  void stop();

  /// @brief Sends a JSON message to all currently-connected WebSocket clients.
  /// @param json  Serialised JSON string.  Moved into the deferred closure.
  void broadcast(std::string json);

  /// @brief Publishes a JSON message to the clients subscribed to @p topic (native uWS pub/sub).
  /// @param topic  Topic to publish under (a monitoring id).  Moved into the deferred closure.
  /// @param json   Serialised JSON string.  Moved into the deferred closure.
  void publish(std::string topic, std::string json);

 private:
  struct WsData {};

  void run();

  Config config_;
  std::atomic<bool> running_{false};
  std::thread thread_;
  std::atomic<uWS::Loop*> loop_{nullptr};
  std::atomic<uWS::SSLApp*> app_{nullptr};
  std::unordered_set<uWS::WebSocket<true, true, WsData>*> connections_;
  /// Signals the listen outcome from the loop thread back to start(); set exactly once per run().
  std::promise<bool> listenResult_;
};
