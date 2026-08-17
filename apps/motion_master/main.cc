#include <curl/curl.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

#include "bus_health_reporter.h"
#include "cert_updater.h"
#include "comm/base.h"
#include "comm/soem_fieldbus_driver.h"
#include "core/platform.h"
#include "core/user_cache.h"
#include "core/version.h"
#include "example/example_cyclic_task.h"
#include "example/example_routes.h"
#include "game_loop.h"
#include "http_server.h"
#include "node/device_manager.h"
#include "node/monitoring_manager.h"
#include "node/procedure_manager.h"
#include "node/process_data_cyclic_task.h"
#include "options.h"
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
// Nothing here throws deliberately — the codebase returns std::expected rather than throwing —
// but the standard library still can: a std::string or nlohmann::json allocation raises bad_alloc.
// Letting that terminate the process at startup is the honest outcome; a catch-all here could only
// print and exit non-zero anyway.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char** argv) {
  // Replacing the default logger drops its built-in console sink, so re-add it
  // explicitly alongside the ring sink that backs GET /api/log.
  auto ringSink = std::make_shared<mm::RingLogSinkMt>();
  auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  spdlog::set_default_logger(
      std::make_shared<spdlog::logger>("", spdlog::sinks_init_list{consoleSink, ringSink}));

  auto opts = parseOptions(argc, argv);

  // Where everything Motion Master keeps on this machine's disk lives. Resolved here, before the
  // log file that is the first thing to go under it, so the user-cache store, the recorder's dumps
  // and the log cannot drift apart: pointing `userCache.directory` somewhere else moves all three,
  // and each stays listable through /api/user-cache. An explicit per-feature path still wins.
  const std::filesystem::path userCacheRoot =
      opts.config.userCache.directory.empty()
          ? mm::core::userCacheDir()
          : std::filesystem::path{opts.config.userCache.directory};

  // The console and the ring share the configured level; the file keeps its own, so the terminal
  // can stay readable while the file holds the detail a support request needs. The logger gates
  // before any sink does, so it has to run at whichever of the two is more verbose — otherwise the
  // console setting would silently starve the file.
  const auto consoleLevel = spdlog::level::from_str(opts.config.logging.level);
  consoleSink->set_level(consoleLevel);
  ringSink->set_level(consoleLevel);
  auto loggerLevel = consoleLevel;
  std::filesystem::path logFile;
  if (opts.config.logging.file.enabled) {
    const auto& fileConfig = opts.config.logging.file;
    const std::filesystem::path logDir = fileConfig.directory.empty()
                                             ? userCacheRoot / "logs"
                                             : std::filesystem::path{fileConfig.directory};
    logFile = logDir / "motion-master.log";
    // spdlog signals a sink it cannot open by throwing, which is the one place this codebase has to
    // catch rather than return — and it must not be fatal: a read-only or unwritable directory is a
    // reason to run without a log file, not a reason not to run. The console and GET /api/log are
    // unaffected either way.
    try {
      auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
          logFile.string(), static_cast<std::size_t>(fileConfig.maxSizeMb) * 1024 * 1024,
          fileConfig.maxFiles);
      const auto fileLevel = spdlog::level::from_str(fileConfig.level);
      fileSink->set_level(fileLevel);
      spdlog::default_logger()->sinks().push_back(std::move(fileSink));
      loggerLevel = std::min(loggerLevel, fileLevel);
    } catch (const spdlog::spdlog_ex& e) {
      logFile.clear();
      spdlog::warn("Log file disabled — could not open {}: {}", (logDir).string(), e.what());
    }
  }
  spdlog::set_level(loggerLevel);
  // Make a warning durable the moment it is written. spdlog hands every line to fwrite immediately
  // but never calls fflush of its own accord, so lines sit in the C stdio buffer (~4 KB, roughly 44
  // of them) until it fills. A clean shutdown flushes that; a segfault or a SIGKILL does not —
  // which would lose exactly the tail a crash investigation wants.
  //
  // Flushing does not reorder anything, and that is what makes this cheap rather than a compromise:
  // every line goes into the same stream in the order it was logged, so a flush pushes out
  // *everything buffered so far*, not just the line that triggered it. A warning therefore
  // checkpoints the debug lines that led up to it as well. What can still be lost is only what was
  // logged after the last warning.
  //
  // warn rather than info: it is the level that means something is wrong, and it is rare, so the
  // extra fflush (~200 ns, measured) is paid on the lines that justify it instead of on the
  // high-volume debug traffic — flushing every line costs closer to 330 ns against 200.
  spdlog::default_logger()->flush_on(spdlog::level::warn);

  spdlog::info("Motion Master v{}", mm::core::kVersion);
  // Named once the file sink is up, so a support log says which config was in effect and where the
  // log itself is — neither of which is recoverable from the log's contents afterwards.
  spdlog::info("Config: {}", opts.configPath.empty() ? "built-in defaults" : opts.configPath);
  if (!logFile.empty()) {
    spdlog::info("Logging to {} at level {}", logFile.string(), opts.config.logging.file.level);
  }

  // Refuse to start a second instance: exactly one Motion Master per machine may own the EtherCAT
  // NIC, the RT loop, and the HTTP/WebSocket ports. Held for the whole of main() (the OS releases
  // it on any exit, so there is no stale lock to clear). This is the primary guard — the exclusive
  // port bind below is a backstop for the case where some *other* process holds a port.
  auto instanceLock = mm::core::acquireSingleInstanceLock();
  if (!instanceLock) {
    spdlog::error("{}", instanceLock.error());
    return 1;
  }

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
  // Its directory is derived from the shared root for the same reason the recorder's dump
  // directory is: moving `userCache.directory` has to move everything Motion Master writes, or the
  // /api/user-cache listing stops being the whole picture. An explicit `parameterCache.directory`
  // still wins.
  deviceManager.configureParameterCache(
      {.cacheAllVendors = opts.config.parameterCache.cacheAllVendors,
       .directory = opts.config.parameterCache.directory.empty()
                        ? (userCacheRoot / "parameters").string()
                        : opts.config.parameterCache.directory,
       .enabled = opts.config.parameterCache.enabled});

  // Runtime tuning for DeviceManager::init, derived once from the config and shared by both the
  // eager startup init below and the POST /api/init callback.
  const mm::node::DeviceManagerConfig deviceManagerConfig{
      .readObjectDictionaryOnPreop = opts.config.parameters.readObjectDictionaryOnPreop,
      .useCompleteAccess = opts.config.parameters.useCompleteAccess,
      .recorderDumpDir = opts.config.recorder.dumpDir.empty() ? (userCacheRoot / "dumps").string()
                                                              : opts.config.recorder.dumpDir,
      .recorderCapacity = opts.config.recorder.capacity};

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
      ifname = resolved->name;
      // Recorded on every init, because a support log that never names the adapter cannot answer
      // the first question any fieldbus fault raises — and on Windows the interface name is an NPF
      // GUID path, so the description and the MAC (whose OUI names the vendor) are what identify
      // the hardware. The description is empty where the interface name already does that.
      if (resolved->description.empty()) {
        spdlog::info("Fieldbus adapter: {} [{}]", resolved->name, resolved->macLinux);
      } else {
        spdlog::info("Fieldbus adapter: {} — {} [{}]", resolved->name, resolved->description,
                     resolved->macLinux);
      }
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

  // Runs off-RT procedures and retains their results for polling.
  mm::node::ProcedureManager procedureManager{deviceManager};

  // The user-writable file store behind /api/user-cache, rooted at the shared cache directory
  // resolved above. Like the parameter cache, its location is a process-level setting fixed at
  // startup, not something init() can change. Everything Motion Master writes lands under this
  // root — the parameter cache's `parameters/` and the recorder's `dumps/` included — so the API
  // lists those too.
  mm::core::UserCache userCache{userCacheRoot};
  // The log file is the one thing under this root that the server itself holds open, so deleting it
  // through the API cannot do what it appears to. Say so here — the store is told a path, never
  // asked to recognise one — and both the listing and DELETE then refuse it. Rotated siblings are
  // closed and stay deletable, which is how a user reclaims the space. Skipped when the log was
  // configured outside the root, where the API cannot reach it anyway.
  if (!logFile.empty()) {
    const auto relative = logFile.lexically_relative(userCacheRoot);
    if (!relative.empty() && *relative.begin() != "..") {
      userCache.retain(relative.generic_string());
    }
  }

  // Exchange process data every cycle. No-op until devices are mapped and brought into SAFE-OP/OP
  // via the API, at which point DeviceManager publishes the image and the loop begins driving PDO
  // automatically. Declared before gameLoop so it is destroyed after it — a registered task must
  // outlive every call to GameLoop::run().
  ProcessDataCyclicTask processDataCyclicTask{deviceManager};

  // Tier 3 — your own code inside the RT loop. Uncomment the three marked lines (here, and the
  // addTask + keepFresh below) to run libs/example/example_cyclic_task.cc, then copy that file to
  // start your own. It is a naive thermal interlock: it puts a drive into CSV, enables it, runs it
  // at a fixed velocity, and quick-stops it if the temperature goes over the limit.
  //
  // WARNING: this spins a motor. Enabling the drive is the task's job, not something it waits for.
  // Check Config::slavePosition and the temperature object against your bus first — the defaults
  // are placeholders, and the object index in particular has to come from your device's dictionary.
  // mm::example::ExampleCyclicTask exampleCyclicTask{deviceManager, {}};

  // The RT game loop is constructed here — before the servers — so HttpServer's GET /api/game-loop
  // callback can borrow it via a lambda. Its constructor is side-effect-free; RT setup and the
  // cycle loop happen in run() at the very end. Declared before httpServer so it outlives the HTTP
  // thread that may invoke the callback.
  GameLoop gameLoop{std::chrono::microseconds{opts.config.gameLoop.periodUs},
                    opts.config.gameLoop.cpuAffinity};
  gameLoop.addTask(&processDataCyclicTask);
  // Tier 3, line 2 of 3 — register the example task. Membership is fixed: every task is added
  // before run(), and the loop never gains or loses one afterwards.
  // gameLoop.addTask(&exampleCyclicTask);

  HttpServer httpServer{
      HttpServer::Config{
          .bindAddress = opts.config.server.bindAddress,
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
          .setGameLoopPeriod = [&gameLoop](uint32_t periodUs) -> std::expected<void, std::string> {
            // Same rule as config validation (config.cc): a zero period is meaningless. No upper
            // clamp — a too-aggressive period fails visibly via skippedCycles, not silently.
            if (periodUs == 0) {
              return std::unexpected("periodUs must be greater than 0");
            }
            // Retime the RT loop. The change is transient — it does not rewrite the config file.
            // The recorder ring is period-independent (sized in cycles), so nothing else to touch.
            gameLoop.setPeriod(std::chrono::microseconds{periodUs});
            return {};
          },
          .corsOrigin = opts.config.server.corsOrigin,
      },
      deviceManager, monitoringManager, procedureManager, userCache};
  // Wire the example C++ route plug-in (/api/example/...) before start(): the composition root is
  // the only place that knows the concrete plug-in. Copy libs/example to add your own.
  httpServer.addRoutes(mm::example::registerRoutes);
  if (!httpServer.start()) {
    spdlog::error("HTTP server could not bind {}:{} — is another process using it?",
                  opts.config.server.bindAddress, opts.config.server.httpPort);
    return 1;
  }

  WebSocketServer wsServer{WebSocketServer::Config{
      .bindAddress = opts.config.server.bindAddress,
      .port = opts.config.server.wsPort,
      .certFile = opts.config.tls.certPath,
      .keyFile = opts.config.tls.keyPath,
  }};
  if (!wsServer.start()) {
    spdlog::error("WebSocket server could not bind {}:{} — is another process using it?",
                  opts.config.server.bindAddress, opts.config.server.wsPort);
    return 1;
  }

  // Bound off loopback, this server is reachable from the network — so say the two things the
  // listen lines above do not. The machine's own addresses are deliberately NOT enumerated and
  // printed: a host with several interfaces (wired, wireless, a container bridge) would yield
  // several plausible-looking URLs of which only one works, and inside a bridge-networked container
  // it would confidently print an address nobody can reach. The client already derives the exact
  // hostname and hosts-file line from the address the user enters, which is where that belongs.
  if (opts.config.server.bindAddress != "127.0.0.1" &&
      opts.config.server.bindAddress != "localhost") {
    spdlog::warn(
        "Bound off loopback — reachable from the network, and the API has NO authentication. Use "
        "only on a trusted network.");
    spdlog::info(
        "Browsers reach this server as https://<dashed-ip>.ip.motion-master.synapticon.com:{} "
        "(192-168-1-50.ip.… for 192.168.1.50) and need a matching hosts-file entry — see "
        "docs/LAN_DEPLOYMENT.md",
        opts.config.server.httpPort);
  }

  // Publish each sampled batch to the WebSocket topic named after its monitoring, then start the
  // sampler + refresher threads. (Not added to the RT GameLoop — sampling runs off the RT thread.)
  monitoringManager.setPublish([&wsServer](std::string topic, std::string json) {
    wsServer.publish(std::move(topic), std::move(json));
  });
  monitoringManager.start();
  // Tier 3, line 3 of 3 — keep the example task's temperature object polled. 0x2031:01 (SOMANET
  // "Drive temperature") is not in the process image, so nothing refills it cyclically; without
  // this the task never gets a reading and its interlock holds the drive stopped. Off the RT
  // thread, as it must be — an ordinary call on an ordinary thread. Must name the same object as
  // ExampleCyclicTask::Config.
  // monitoringManager.keepFresh(1, 0x2031, 0x01, std::chrono::milliseconds{200});

  // Says what the RT loop saw and cannot say itself — cycles the bus did not fully answer.
  // Declared after deviceManager so it is destroyed first; see bus_health_reporter.h for why this
  // is an app-layer policy rather than something DeviceManager does for itself.
  BusHealthReporter busHealthReporter{deviceManager};

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
