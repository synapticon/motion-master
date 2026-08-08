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

  /// @brief Runs @p op on a worker thread and sends its timed JSON result from the loop thread.
  ///
  /// The off-loop counterpart of @c mm::api::sendTimedJson, and the reason it exists is that uWS
  /// runs handlers on the single loop thread: a handler that blocks on the bus blocks every other
  /// request, including ones that touch no hardware. Wrapping a blocking endpoint in this keeps the
  /// loop free to answer everything else while the device work happens elsewhere.
  ///
  /// **Response lifetime, which is the whole safety argument.** A @c uWS::HttpResponse may only be
  /// touched on the loop thread, and it is destroyed if the client disconnects. Both rules are
  /// honoured by construction here: @p op runs on the worker and touches nothing but the domain,
  /// and every write happens inside @c loop->defer, on the loop thread. The abort flag is set by
  /// @c onAborted — also on the loop thread — and read inside the same deferred callback, so the
  /// two cannot interleave: either the abort ran first and the flag is set, or it did not and the
  /// response is still alive. @c OffLoopPool is joined before the loop is closed, so no worker can
  /// defer onto a loop that has gone.
  ///
  /// @param res          The response, captured for completion on the loop thread.
  /// @param errorStatus  Status line for a failed @p op (e.g. "500 Internal Server Error").
  /// @param op           Callable returning @c std::expected<T, E>; runs off the loop.
  template <typename Res, typename Op>
  void sendTimedJsonOffLoop(Res* res, std::string_view errorStatus, Op op) {
    auto* loop = loop_.load();
    if (loop == nullptr) {
      mm::api::sendError(res, "503 Service Unavailable", config_.corsOrigin, "server is stopping");
      return;
    }
    auto aborted = std::make_shared<std::atomic<bool>>(false);
    res->onAborted([aborted]() { aborted->store(true); });

    pool_.detach_task([this, res, loop, aborted, errorStatus, op = std::move(op)]() mutable {
      const auto t0 = std::chrono::steady_clock::now();
      auto result = op();
      const auto wireUs = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - t0);
      loop->defer([this, res, aborted, errorStatus, wireUs, result = std::move(result)]() {
        if (aborted->load()) {
          return;  // The client is gone and res with it; touching it here would be a
                   // use-after-free.
        }
        if (!result) {
          mm::api::sendError(res, errorStatus, config_.corsOrigin, result.error(), wireUs);
          return;
        }
        mm::api::setWireTime(res, wireUs);
        mm::api::sendJson(res, config_.corsOrigin, *result);
      });
    });
  }

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
  /// Four threads. Bus operations serialise on the driver's control-plane lock regardless, so more
  /// threads buy no parallelism on the wire — but they keep a request blocked on a *different*
  /// resource (the bus lock held by a running procedure, a filesystem read) from queuing behind one
  /// waiting on the wire.
  ///
  /// `light_thread_pool` is the plain variant: no priorities, no pausing, no per-task futures —
  /// this only ever needs `detach_task` and `wait`.
  BS::light_thread_pool pool_{4};
};
