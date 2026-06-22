#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include "cert_info.h"
#include "cert_updater.h"
#include "comm/base.h"
#include "comm/fieldbus_driver.h"
#include "comm/soem_fieldbus_driver.h"
#include "core/platform.h"
#include "core/version.h"
#include "game_loop.h"
#include "http_server.h"
#include "node/device_manager.h"
#include "node/monitoring_manager.h"
#include "options.h"
#include "process_data_task.h"
#include "ring_log_sink.h"
#include "ws_server.h"

static GameLoop* gGameLoop = nullptr;  ///< Signal handler target; set before run(), cleared after.

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
  auto ringLogSink = std::make_shared<mm::RingLogSinkMt>();
  auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  spdlog::set_default_logger(
      std::make_shared<spdlog::logger>("", spdlog::sinks_init_list{consoleSink, ringLogSink}));

  auto opts = parseOptions(argc, argv);
  spdlog::set_level(spdlog::level::from_str(opts.config.logLevel));

  spdlog::info("Motion Master v{}", mm::core::kVersion);

  mm::node::DeviceManager deviceManager;
  // The parameter cache is a process-level setting (its directory comes from the config file, like
  // the ports), independent of whether/when a driver is initialised — so apply it at startup, not
  // from init(). This keeps the cache directory correct for the management API and every device's
  // parameter load from the get-go.
  deviceManager.configureParameterCache(
      {.enabled = opts.config.parameterCache.enabled,
       .cacheAllVendors = opts.config.parameterCache.cacheAllVendors,
       .directory = opts.config.parameterCache.directory});

  // Runtime tuning for DeviceManager::init, derived once from the config and shared by both the
  // eager startup init below and the POST /api/init callback.
  const mm::node::DeviceManagerConfig deviceManagerConfig{
      .recorderHistorySeconds = opts.config.recorder.historySeconds,
      .cyclePeriodUs = opts.config.gameLoop.periodUs,
      .dumpDir = opts.config.recorder.dumpDir,
      .readObjectDictionaryOnPreop = opts.config.parameters.readObjectDictionaryOnPreop};

  // Resolve the adapter, construct the concrete driver, and hand it to DeviceManager::init. Used
  // both for the optional eager init below and as the POST /api/init callback, so the two paths
  // share one set of driver-creation and adapter-resolution rules. main.cc is the only place that
  // names concrete driver types (the composition root).
  auto initDeviceManager = [&deviceManager, deviceManagerConfig](
                               const std::string& type,
                               const std::string& adapter) -> std::expected<void, std::string> {
    std::string ifname;
    if (!adapter.empty()) {
      auto resolved = mm::comm::resolveNetworkAdapter(adapter);
      if (!resolved) {
        return std::unexpected(resolved.error());
      }
      ifname = resolved->adapterName;
    }
    if (type != "soem") {
      return std::unexpected("unsupported driver: " + type);
    }
    return deviceManager.init(std::make_unique<mm::comm::soem::SoemFieldbusDriver>(ifname),
                              deviceManagerConfig);
  };

  // Optional eager init from the config file; otherwise the bus is initialised later via
  // POST /api/init. Failure here is fatal — a configured driver that cannot start is a hard error.
  if (!opts.config.fieldbus.driver.empty()) {
    if (auto result = initDeviceManager(opts.config.fieldbus.driver, opts.config.fieldbus.adapter);
        !result) {
      spdlog::error("DeviceManager init failed: {}", result.error());
      return 1;
    }
    if (auto result = deviceManager.scan(); !result) {
      return 1;
    }
  }

  // The install-dir cert/key — both the default served location and the target the self-heal and
  // --update-cert paths fetch into (it is writable by the same privileges that installed the
  // binary).
  const auto defaultCert = mm::core::exeDir() / "cert.pem";
  const auto defaultKey = mm::core::exeDir() / "key.pem";

  // Resolve the TLS cert/key paths: explicit config wins, else discover (bundled → acme.sh), else
  // fall through to the install-dir default so the self-heal below has a target to fetch into. A
  // miss is not fatal here — unlike before, the self-heal can populate it.
  const auto resolvedCert = mm::resolveCertPaths(opts.config.tls.certPath, opts.config.tls.keyPath,
                                                 defaultCert, defaultKey);
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

  // Decide whether the served cert needs refreshing. A missing cert means we cannot serve TLS at
  // all; an expired cert still binds (browsers can bypass) but should be refreshed. Both are healed
  // by fetching from the rolling release unless --no-cert-update is set.
  bool needFetch = false;
  bool certMissing = !std::filesystem::exists(opts.config.tls.certPath) ||
                     !std::filesystem::exists(opts.config.tls.keyPath);
  if (certMissing) {
    needFetch = true;
    spdlog::warn("No TLS certificate at {}", opts.config.tls.certPath);
  } else if (auto info = mm::readCertInfo(opts.config.tls.certPath)) {
    const auto now = std::chrono::system_clock::now();
    const auto daysRemaining =
        std::chrono::duration_cast<std::chrono::hours>(info->notAfter - now).count() / 24;
    if (now >= info->notAfter) {
      needFetch = true;
      spdlog::error("TLS certificate EXPIRED ({} days ago)", -daysRemaining);
    } else if (daysRemaining < mm::kCertExpiryWarningDays) {
      spdlog::warn("TLS certificate expires in {} days", daysRemaining);
    } else {
      spdlog::info("TLS certificate valid for {} more days", daysRemaining);
    }
  } else {
    spdlog::warn("Could not read TLS certificate expiry: {}", info.error());
  }

  if (needFetch) {
    if (!opts.config.tls.autoUpdate) {
      if (certMissing) {
        spdlog::error("No certificate and --no-cert-update set — cannot serve TLS");
        return 1;
      }
      spdlog::error(
          "Certificate expired and --no-cert-update set — serving the expired certificate");
    } else {
      spdlog::warn("Fetching fresh TLS certificate from {}", opts.certUrl);
      if (auto r = mm::fetchAndSwapCert(opts.config.tls.certPath, opts.config.tls.keyPath,
                                        opts.certUrl, opts.keyUrl);
          r) {
        spdlog::info("Installed fresh TLS certificate at {}", opts.config.tls.certPath);
      } else if (certMissing) {
        spdlog::error("Certificate fetch failed and no local certificate exists: {}", r.error());
        return 1;
      } else {
        spdlog::error("Certificate fetch failed: {} — serving the expired certificate", r.error());
      }
    }
  }

  // Owns the monitoring registry plus its background SDO-refresher and sampler threads. The HTTP
  // server reaches it for the /api/monitorings routes; sampled batches publish over the WebSocket
  // server, which runs on its own port/loop so a slow HTTP handler can never stall the stream.
  mm::node::MonitoringManager monitoringManager{deviceManager};

  HttpServer httpServer{
      HttpServer::Config{
          .port = opts.config.server.httpPort,
          .certFile = opts.config.tls.certPath,
          .keyFile = opts.config.tls.keyPath,
          .version = std::string{mm::core::kVersion},
          .startedConfig = nlohmann::json(opts.config).dump(),
          .initDriver = initDeviceManager,
          .getLog = [ringLogSink]() { return ringLogSink->entries(); },
          .refreshCert = [certFile = opts.config.tls.certPath, keyFile = opts.config.tls.keyPath,
                          certUrl = opts.certUrl,
                          keyUrl = opts.keyUrl]() -> std::expected<void, std::string> {
            return mm::fetchAndSwapCert(certFile, keyFile, certUrl, keyUrl);
          },
          .corsOrigin = opts.config.server.corsOrigin,
      },
      deviceManager, monitoringManager};
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
    mm::core::openInBrowser("https://motion-master.synapticon.com/app/");
  }

  GameLoop game_loop{std::chrono::microseconds{opts.config.gameLoop.periodUs}};

  // Exchange process data every cycle. No-op until devices are mapped and brought into
  // SAFE-OP/OP via the API, at which point DeviceManager publishes the image and the loop
  // begins driving PDO automatically.
  ProcessDataTask processDataTask{deviceManager};
  game_loop.addTask(&processDataTask);

  gGameLoop = &game_loop;

  std::signal(SIGINT, [](int) {
    if (gGameLoop) {
      gGameLoop->stop();
    }
  });
  std::signal(SIGTERM, [](int) {
    if (gGameLoop) {
      gGameLoop->stop();
    }
  });

  game_loop.run();  // main thread IS the RT loop — blocks until stop()

  gGameLoop = nullptr;
  monitoringManager.stop();  // stop sampling/publishing before the server loops go away
  wsServer.stop();
  httpServer.stop();

  spdlog::info("Shutting down");
  return 0;
}
