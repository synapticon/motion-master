#pragma once

#include <uwebsockets/App.h>

#include <BS_thread_pool.hpp>
#include <atomic>
#include <chrono>
#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "api/router.h"
#include "api/web_api.h"
#include "game_loop.h"  // GameLoopHealth (returned by the GET /api/game-loop callback)

namespace mm::core {
class UserCache;
}  // namespace mm::core

namespace mm::node {
class DeviceManager;
class MonitoringManager;
class ProcedureManager;
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
  using InitDeviceManagerFn =
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

  /// @brief Callback type for `GET /api/game-loop`.
  ///
  /// Returns a snapshot of the RT game-loop's health (cycle counters, achieved
  /// rate, task-execution timing, RT-scheduling flags).  Wired to
  /// @c GameLoop::health() in main.cc so the server never references the loop
  /// itself — the same composition-root pattern as @c GetLogFn.
  using GetGameLoopHealthFn = std::function<GameLoopHealth()>;

  /// @brief Callback type for `PUT /api/game-loop`.
  ///
  /// Applies a new RT cycle period (microseconds). Validates the value and, on
  /// success, retimes the running loop and updates the period recorded for the
  /// recorder dumps.  Lives in main.cc so the server references neither the
  /// @c GameLoop nor the @c DeviceManager for this — the same composition-root
  /// pattern as @c GetGameLoopHealthFn.  Returns an error string on an invalid
  /// value.
  using SetGameLoopPeriodFn = std::function<std::expected<void, std::string>(uint32_t periodUs)>;

  /// @brief Server configuration.
  struct Config {
    /// Local address to bind. Loopback serves only this machine; "0.0.0.0" serves the network.
    std::string bindAddress{"127.0.0.1"};
    uint16_t port = 61447;  ///< TCP port to listen on (TLS).
    std::string certFile;   ///< Path to the TLS certificate (PEM).
    std::string keyFile;    ///< Path to the TLS private key (PEM).
    std::string version;    ///< Application version string served at `GET /api/version`.
    std::string
        startedConfig;  ///< Serialized JSON of the boot config; served at `GET /api/config`.
    InitDeviceManagerFn
        initDeviceManager;      ///< Handler for `POST /api/init`; required for API-driven init.
    GetLogFn getLog;            ///< Handler for `GET /api/log`; returns buffered log entries.
    RefreshCertFn refreshCert;  ///< Handler for `POST /api/cert/refresh`; fetches+installs a cert.
    GetGameLoopHealthFn
        getGameLoopHealth;  ///< Handler for `GET /api/game-loop`; RT loop health snapshot.
    SetGameLoopPeriodFn
        setGameLoopPeriod;  ///< Handler for `PUT /api/game-loop`; retimes the RT loop.
    /// Value sent in `Access-Control-Allow-Origin`. Defaults to the production PWA origin.
    std::string corsOrigin{"https://motion-master.synapticon.com"};
  };

  /// @brief Constructs the server with the given configuration.
  /// @param config             Server parameters.  The config is copied internally.
  /// @param deviceManager      Device list source; lifetime must exceed that of this object.
  /// @param monitoringManager  Monitoring registry backing the `/api/monitorings` routes; lifetime
  ///                           must exceed that of this object.
  /// @param procedureManager   Procedure runner backing the `/api/devices/:pos/procedures` routes;
  ///                           lifetime must exceed that of this object.
  /// @param userCache          File store backing the `/api/user-cache` routes; lifetime must
  ///                           exceed that of this object.
  HttpServer(Config config, mm::node::DeviceManager& deviceManager,
             mm::node::MonitoringManager& monitoringManager,
             mm::node::ProcedureManager& procedureManager, mm::core::UserCache& userCache);

  /// @brief Destructor.  Calls stop() if the server is still running.
  ~HttpServer();

  /// @brief Starts the server background thread and begins accepting connections.
  ///
  /// Blocks until the listen attempt completes, so the caller learns synchronously whether the
  /// port was bound. The port is claimed exclusively (@c LIBUS_LISTEN_EXCLUSIVE_PORT), so a port
  /// already in use fails here instead of silently sharing it via @c SO_REUSEPORT.
  ///
  /// Idempotent: a second call while already running is a no-op that returns @c true.
  /// @return @c true if the server is listening; @c false if the port could not be bound.
  bool start();

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
  mm::node::ProcedureManager& procedureManager_;
  mm::core::UserCache& userCache_;
  std::vector<mm::api::RegisterRoutesFn> routeModules_;
  std::atomic<bool> running_{false};
  std::thread thread_;
  std::atomic<uWS::Loop*> loop_{nullptr};
  std::atomic<uWS::SSLApp*> app_{nullptr};
  /// Signals the listen outcome from the loop thread back to start(); set exactly once per run().
  std::promise<bool> listenResult_;
  /// Set by stop(), on the loop thread, to close the Router's dispatch before the pool is drained.
  /// Read only by the Router, also on that thread. See @c mm::api::Router::stopping_ for why the
  /// drain alone is not enough and why the store has to happen there rather than here.
  std::atomic<bool> stopping_{false};
  /// Workers for handlers that would otherwise block the event loop.
  ///
  /// uWebSockets runs every handler on the app's single loop thread, so a handler that blocks
  /// blocks the *whole* HTTP API rather than just its own endpoint. Several of Motion Master's
  /// block for seconds by nature — an FoE transfer, an object-dictionary enumeration, any SDO
  /// behind a busy control-plane lock. Measured during a firmware installation, a
  /// `GET /api/devices/state` waiting on the control-plane lock for a 12-second file transfer
  /// stalled every request behind it, including `/api/version`, which touches no hardware at all.
  /// The two-port split already gives the WebSocket this protection; this is the same for HTTP
  /// requests against each other.
  ///
  /// **Sized so it cannot be exhausted, which is what makes running every route here safe.**
  ///
  /// The usual objection to putting *all* handlers on a pool — and it is a fair one for a
  /// high-throughput service — is that quick requests end up queued behind slow ones, competing for
  /// worker slots. That is the original bug in miniature, and it is the only one of the objections
  /// with real force here: the thread hop itself costs microseconds against browser TLS round trips
  /// of milliseconds, at the handful of requests per second this API actually sees.
  ///
  /// It is answered by arithmetic rather than by classification. HTTP/1.1 browsers cap at about six
  /// connections per origin, so the ~5 simultaneous clients this is sized for can have at most
  /// ~30 requests in flight at once. A pool wider than that can never be saturated by them, so no
  /// request ever waits for a worker — a luxury of a bounded, local client population that a public
  /// service does not get.
  ///
  /// The alternative, running "fast" handlers inline and only offloading blocking ones, was
  /// considered and rejected on asymmetry: misjudging a blocking handler as fast freezes the entire
  /// API, misjudging a fast one as blocking costs 30 µs. Six orders of magnitude apart — and the
  /// judgement is genuinely hard here, since most handlers reach @c DeviceManager and whether one
  /// blocks depends on what another thread is holding at the time. A boundary that erodes toward
  /// the catastrophic side is not worth microseconds.
  ///
  /// A worker parked on a mutex costs a stack reservation and a kernel task, so 32 is free.
  ///
  /// `light_thread_pool` is the plain variant: no priorities, no pausing, no per-task futures —
  /// this only ever needs `detach_task`, `purge` and `wait`.
  BS::light_thread_pool pool_{32};
};
