#include <curl/curl.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

#include "cert_updater.h"
#include "comm/base.h"
#include "comm/soem_fieldbus_driver.h"
#include "core/platform.h"
#include "core/version.h"
#include "example/example_routes.h"
#include "game_loop.h"
#include "http_server.h"
#include "node/device_manager.h"
#include "node/monitoring_manager.h"
#include "options.h"
#include "process_data_task.h"
#include "ring_log_sink.h"
#include "ws_server.h"

/// @brief Signal-handler target for SIGINT/SIGTERM; set before run(), cleared after.
/// @details Atomic rather than a plain pointer because the handler reads it concurrently with the
///          main thread's writes. A signal handler may only access lock-free atomic objects, and
///          @c std::atomic<GameLoop*> is always lock-free, so the load below is well-defined.
static std::atomic<GameLoop*> gGameLoop{nullptr};

/// @brief Motion Master entry point: parse options, start subsystems, run the RT loop.
/// @details Initialises the HTTP/WebSocket server and game loop, installs SIGINT/SIGTERM
///          handlers, then blocks on GameLoop::run() — the main thread IS the RT thread.
///          Returns only after stop() is signalled and all subsystems are torn down.
/// @param argc Argument count from the OS.
/// @param argv Argument vector from the OS.
/// @return 0 on clean shutdown.
int main(int argc, char** argv) {
  // Replacing the default logger drops its built-in console sink, so re-add it
  // explicitly alongside the ring sink that backs GET /api/log.
  auto ringSink = std::make_shared<mm::RingLogSinkMt>();
  auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  spdlog::set_default_logger(
      std::make_shared<spdlog::logger>("", spdlog::sinks_init_list{consoleSink, ringSink}));

  auto opts = parseOptions(argc, argv);
  spdlog::set_level(spdlog::level::from_str(opts.config.logLevel));

  spdlog::info("Motion Master v{}", mm::core::kVersion);

  // libcurl global state is process-wide and must be initialised exactly once, before any other
  // thread starts — curl_global_init also initialises libraries (OpenSSL) that are unsafe to set up
  // concurrently, so it belongs here at the composition root rather than lazily inside any one cURL
  // user. Every cURL caller (today only the certificate fetch paths) then shares this single
  // init/cleanup. RAII so it is torn down on every return path.
  struct CurlGlobal {
    CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~CurlGlobal() { curl_global_cleanup(); }
    CurlGlobal(const CurlGlobal&) = delete;
    CurlGlobal& operator=(const CurlGlobal&) = delete;
  } curlGlobal;

  mm::node::DeviceManager deviceManager;
  // The parameter cache is a process-level setting (its directory comes from the config file, like
  // the ports), independent of whether/when a driver is initialised — so apply it at startup, not
  // from init(). This keeps the cache directory correct for the management API and every device's
  // parameter load from the get-go.
  deviceManager.configureParameterCache(
      {.cacheAllVendors = opts.config.parameterCache.cacheAllVendors,
       .directory = opts.config.parameterCache.directory,
       .enabled = opts.config.parameterCache.enabled});

  // Runtime tuning for DeviceManager::init, derived once from the config and shared by both the
  // eager startup init below and the POST /api/init callback.
  const mm::node::DeviceManagerConfig deviceManagerConfig{
      .cyclePeriodUs = opts.config.gameLoop.periodUs,
      .readObjectDictionaryOnPreop = opts.config.parameters.readObjectDictionaryOnPreop,
      .useCompleteAccess = opts.config.parameters.useCompleteAccess,
      .recorderDumpDir = opts.config.recorder.dumpDir,
      .recorderHistorySeconds = opts.config.recorder.historySeconds};

  // Resolve the adapter, construct the concrete driver, and hand it to DeviceManager::init. Used
  // both for the optional eager init below and as the POST /api/init callback, so the two paths
  // share one set of driver-creation and adapter-resolution rules. main.cc is the only place that
  // names concrete driver types (the composition root).
  auto initDeviceManager = [&deviceManager, deviceManagerConfig,
                            mailboxStatusFmmu = opts.config.fieldbus.mailboxStatusFmmu](
                               const std::string& type,
                               const std::string& adapter) -> std::expected<void, std::string> {
    std::string ifname;
    if (!adapter.empty()) {
      auto resolved = mm::comm::resolveNetworkAdapter(adapter);
      if (!resolved) {
        // Log the failures this lambda originates (adapter resolution, unsupported driver) so they
        // are recorded once on both the startup and POST /api/init paths. DeviceManager::init logs
        // its own driver-init failures, so those are not re-logged here.
        spdlog::error("Adapter resolution failed: {}", resolved.error());
        return std::unexpected(resolved.error());
      }
      ifname = resolved->adapterName;
    }
    if (type != "soem") {
      // soem is the only driver implemented today (spoe is planned). Config validation accepts the
      // planned names, so be explicit at runtime about why a valid-looking driver is refused.
      spdlog::error(
          "Fieldbus driver '{}' is not implemented in this build — only 'soem' is available", type);
      return std::unexpected("fieldbus driver '" + type +
                             "' is not implemented in this build (only 'soem' is available)");
    }
    return deviceManager.init(std::make_unique<mm::comm::soem::SoemFieldbusDriver>(
                                  mm::comm::soem::SoemFieldbusDriverConfig{
                                      .ifname = ifname, .mailboxStatusFmmu = mailboxStatusFmmu}),
                              deviceManagerConfig);
  };

  // Optional eager init from the config file; otherwise the bus is initialised later via
  // POST /api/init. A failed init is fatal — a configured driver that cannot start is a hard error.
  // A failed scan is not: an empty/unscannable bus (no devices powered) is a valid state, so just
  // start up and let the user power devices on and rescan via POST /api/scan. Both log their own
  // outcome.
  if (!opts.config.fieldbus.driver.empty()) {
    if (!initDeviceManager(opts.config.fieldbus.driver, opts.config.fieldbus.adapter)) {
      return 1;
    }
    [[maybe_unused]] const auto scanResult = deviceManager.scan();
  }

  // The install-dir cert/key — both the default served location and the target the self-heal and
  // --update-cert paths fetch into (it is writable by the same privileges that installed the
  // binary).
  const auto defaultCertPath = mm::core::exeDir() / "cert.pem";
  const auto defaultKeyPath = mm::core::exeDir() / "key.pem";

  // Resolve the TLS cert/key paths: explicit config wins, else discover (bundled → acme.sh), else
  // fall through to the install-dir default so the self-heal below has a target to fetch into. A
  // miss is not fatal here — unlike before, the self-heal can populate it.
  const auto resolvedCert = mm::resolveCertPaths(opts.config.tls.certPath, opts.config.tls.keyPath,
                                                 defaultCertPath, defaultKeyPath);
  opts.config.tls.certPath = resolvedCert.certPath;
  opts.config.tls.keyPath = resolvedCert.keyPath;
  if (!resolvedCert.source.empty()) {
    spdlog::info("TLS: {}", resolvedCert.source);
  }

  // --update-cert: fetch a fresh cert/key into the resolved path, then exit without serving. The
  // explicit path for terminal/headless use.
  if (opts.updateCert) {
    spdlog::info("Fetching TLS certificate from {}", opts.certUrl);
    if (auto r = mm::fetchAndSwapCert(opts.config.tls.certPath, opts.config.tls.keyPath,
                                      opts.certUrl, opts.keyUrl);
        !r) {
      spdlog::error("Certificate update failed: {}", r.error());
      return 1;
    }
    spdlog::info("Installed fresh TLS certificate at {}", opts.config.tls.certPath);
    return 0;
  }

  // Assess the served cert and self-heal it (fetch if missing/expired/expiring soon) unless
  // tls.autoUpdate is false. healCertIfNeeded logs the outcome; it errors only when TLS cannot be
  // served at all.
  if (!mm::healCertIfNeeded(opts.config.tls.certPath, opts.config.tls.keyPath,
                            opts.config.tls.autoUpdate, opts.certUrl, opts.keyUrl)) {
    return 1;
  }

  // Owns the monitoring registry plus its background SDO-refresher and sampler threads. The HTTP
  // server reaches it for the /api/monitorings routes; sampled batches publish over the WebSocket
  // server, which runs on its own port/loop so a slow HTTP handler can never stall the stream.
  mm::node::MonitoringManager monitoringManager{deviceManager};

  // Exchange process data every cycle. No-op until devices are mapped and brought into SAFE-OP/OP
  // via the API, at which point DeviceManager publishes the image and the loop begins driving PDO
  // automatically. Declared before gameLoop so it is destroyed after it — a registered task must
  // outlive every call to GameLoop::run().
  ProcessDataTask processDataTask{deviceManager};

  // The RT game loop is constructed here — before the servers — so HttpServer's GET /api/game-loop
  // callback can borrow it via a lambda. Its constructor is side-effect-free; RT setup and the
  // cycle loop happen in run() at the very end. Declared before httpServer so it outlives the HTTP
  // thread that may invoke the callback.
  GameLoop gameLoop{std::chrono::microseconds{opts.config.gameLoop.periodUs}};
  gameLoop.addTask(&processDataTask);

  HttpServer httpServer{
      HttpServer::Config{
          .port = opts.config.server.httpPort,
          .certFile = opts.config.tls.certPath,
          .keyFile = opts.config.tls.keyPath,
          .version = std::string{mm::core::kVersion},
          .startedConfig = nlohmann::json(opts.config).dump(),
          .initDeviceManager = initDeviceManager,
          .getLog = [ringSink]() { return ringSink->entries(); },
          .refreshCert = [certFile = opts.config.tls.certPath, keyFile = opts.config.tls.keyPath,
                          certUrl = opts.certUrl,
                          keyUrl = opts.keyUrl]() -> std::expected<void, std::string> {
            return mm::fetchAndSwapCert(certFile, keyFile, certUrl, keyUrl);
          },
          .getGameLoopHealth = [&gameLoop] { return gameLoop.health(); },
          .corsOrigin = opts.config.server.corsOrigin,
      },
      deviceManager, monitoringManager};
  // Wire the example C++ route plug-in (/api/example/...) before start(): the composition root is
  // the only place that knows the concrete plug-in. Copy libs/example to add your own.
  httpServer.addRoutes(mm::example::registerRoutes);
  httpServer.start();

  WebSocketServer wsServer{WebSocketServer::Config{
      .port = opts.config.server.wsPort,
      .certFile = opts.config.tls.certPath,
      .keyFile = opts.config.tls.keyPath,
  }};
  wsServer.start();

  // Publish each sampled batch to the WebSocket topic named after its monitoring, then start the
  // sampler + refresher threads. (Not added to the RT GameLoop — sampling runs off the RT thread.)
  monitoringManager.setPublish([&wsServer](std::string topic, std::string json) {
    wsServer.publish(std::move(topic), std::move(json));
  });
  monitoringManager.start();

  if (opts.openBrowser) {
    mm::core::openInBrowser("https://motion-master.synapticon.com/apps/console/");
  }

  gGameLoop.store(&gameLoop, std::memory_order_relaxed);

  // SIGINT/SIGTERM: flip the loop's stop flag so run() returns after the current cycle. Everything
  // touched here is async-signal-safe — a lock-free atomic load of the pointer and stop()'s
  // lock-free atomic store.
  //
  // Both request an orderly shutdown and are catchable (unlike SIGKILL/9, which the kernel enforces
  // with no chance to clean up), so we handle both and route them to the same graceful teardown:
  //   SIGINT  (2)  — interactive interrupt, raised by Ctrl+C in the controlling terminal (delivered
  //                  to the foreground process group). This is the dev-terminal stop path.
  //   SIGTERM (15) — the default `kill <pid>` signal and what service managers (systemd) and
  //                  `docker stop` send to ask a process to exit. This is the production stop path.
  std::signal(SIGINT, [](int) {
    if (auto* loop = gGameLoop.load(std::memory_order_relaxed)) {
      loop->stop();
    }
  });
  std::signal(SIGTERM, [](int) {
    if (auto* loop = gGameLoop.load(std::memory_order_relaxed)) {
      loop->stop();
    }
  });

  gameLoop.run();  // main thread IS the RT loop — blocks until stop()

  gGameLoop.store(nullptr, std::memory_order_relaxed);
  monitoringManager.stop();  // stop sampling/publishing before the server loops go away
  wsServer.stop();
  httpServer.stop();

  spdlog::info("Shutting down");
  return 0;
}
