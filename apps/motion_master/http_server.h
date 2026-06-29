#pragma once

#include <uwebsockets/App.h>

#include <atomic>
#include <expected>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "api/web_api.h"

namespace mm::node {
class DeviceManager;
class MonitoringManager;
}  // namespace mm::node

/// @brief HTTPS REST API server.
///
/// Hosts the REST API (version, cert, CORS preflight, device/bus/monitoring routes) on its own
/// TLS port, event loop, and thread.  The monitoring/control WebSocket lives in a
/// separate @c WebSocketServer on its own port and loop, so a slow or blocking HTTP handler here
/// (FoE transfer, SDO, cert fetch) can never stall the WebSocket.
///
/// All public methods are thread-safe.  start() spawns the background thread; stop() tears it down.
class HttpServer {
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

  /// @brief Callback type for `POST /api/cert/refresh`.
  ///
  /// Fetches a fresh TLS certificate/key from the configured source and installs them, returning
  /// an error string on failure.  Lives in main.cc so the server never references the fetch URLs
  /// or cert paths.  The newly installed cert only takes effect after a restart (uSockets loads
  /// the cert once at listen time).
  using RefreshCertFn = std::function<std::expected<void, std::string>()>;

  /// @brief Server configuration.
  struct Config {
    uint16_t port = 61447;  ///< TCP port to listen on (TLS).
    std::string certFile;   ///< Path to the TLS certificate (PEM).
    std::string keyFile;    ///< Path to the TLS private key (PEM).
    std::string version;    ///< Application version string served at `GET /api/version`.
    std::string
        startedConfig;        ///< Serialized JSON of the boot config; served at `GET /api/config`.
    InitDriverFn initDriver;  ///< Handler for `POST /api/init`; required for API-driven init.
    GetLogFn getLog;          ///< Handler for `GET /api/log`; returns buffered log entries.
    RefreshCertFn refreshCert;  ///< Handler for `POST /api/cert/refresh`; fetches+installs a cert.
    /// Value sent in `Access-Control-Allow-Origin`. Defaults to the production PWA origin.
    std::string corsOrigin{"https://motion-master.synapticon.com"};
  };

  /// @brief Constructs the server with the given configuration.
  /// @param config             Server parameters.  The config is copied internally.
  /// @param deviceManager      Device list source; lifetime must exceed that of this object.
  /// @param monitoringManager  Monitoring registry backing the `/api/monitorings` routes; lifetime
  ///                           must exceed that of this object.
  HttpServer(Config config, mm::node::DeviceManager& deviceManager,
             mm::node::MonitoringManager& monitoringManager);

  /// @brief Destructor.  Calls stop() if the server is still running.
  ~HttpServer();

  /// @brief Starts the server background thread and begins accepting connections.
  ///
  /// Idempotent: a second call while already running is a no-op.
  void start();

  /// @brief Stops the server, closes the listen socket, and joins the thread.
  ///
  /// Idempotent: a second call after the server has already stopped is a no-op.
  void stop();

  /// @brief Registers a route plug-in to be wired in when the server starts.
  ///
  /// Each registered module is invoked once on the server's event-loop thread, after the built-in
  /// routes and before the CORS preflight / catch-all 404 / `listen()`, with a
  /// @c mm::api::RouteContext bound to this server's device and monitoring managers. This is the
  /// extension point for application-specific C++ endpoints (e.g. `/api/example/...`).
  ///
  /// Must be called **before** @c start(); modules added after the thread is running are ignored
  /// (a warning is logged). Not thread-safe with respect to a concurrent @c start().
  void addRoutes(mm::api::RegisterRoutesFn module);

 private:
  void run();

  Config config_;
  mm::node::DeviceManager& deviceManager_;
  mm::node::MonitoringManager& monitoringManager_;
  std::vector<mm::api::RegisterRoutesFn> routeModules_;
  std::atomic<bool> running_{false};
  std::thread thread_;
  std::atomic<uWS::Loop*> loop_{nullptr};
  std::atomic<uWS::SSLApp*> app_{nullptr};
};
