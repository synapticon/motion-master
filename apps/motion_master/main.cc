#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <shellapi.h>  // ShellExecuteA — not pulled in by <windows.h> under WIN32_LEAN_AND_MEAN
// clang-format on
#else
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>  // _NSGetExecutablePath — macOS has no /proc/self/exe
#endif
#endif

#include <memory>

#include "cert_info.h"
#include "cert_updater.h"
#include "comm/fieldbus_driver.h"
#include "comm/soem_fieldbus_driver.h"
#include "core/version.h"
#include "game_loop.h"
#include "node/device_manager.h"
#include "node/monitoring_manager.h"
#include "options.h"
#include "process_data_task.h"
#include "ring_log_sink.h"
#include "server.h"

namespace {

/// @brief Return the directory that contains the running executable.
/// @details argv[0] is not used — it can be a relative path, a bare command name resolved
///          via PATH, or a symlink.
///
///          **Platform behaviour**
///          - Linux: resolves `/proc/self/exe` via `std::filesystem::canonical`.
///          - macOS: queries the path via `_NSGetExecutablePath` (no `/proc`).
///          - Windows: queries the path via `GetModuleFileNameW`.
/// @return Absolute path to the executable's parent directory.
std::filesystem::path exeDir() {
#ifdef _WIN32
  wchar_t buf[MAX_PATH];
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return std::filesystem::path{buf}.parent_path();
#elif defined(__APPLE__)
  // First call reports the required buffer size (including the null terminator).
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buf(size, '\0');
  _NSGetExecutablePath(buf.data(), &size);
  // The returned path may contain symlinks or `..`; canonical() resolves them.
  return std::filesystem::canonical(buf.c_str()).parent_path();
#else
  return std::filesystem::canonical("/proc/self/exe").parent_path();
#endif
}

GameLoop* gGameLoop = nullptr;  ///< Signal handler target; set before run(), cleared after.

/// @brief Open the given URL in the system default browser.
/// @details Non-blocking — returns immediately after spawning the browser process.
///          Uses xdg-open on Linux, `open` on macOS, and ShellExecute on Windows.
void openInBrowser(const char* url) {
#ifdef _WIN32
  ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
#else
  pid_t pid = fork();
  if (pid == 0) {
#ifdef __APPLE__
    execlp("open", "open", url, nullptr);
#else
    execlp("xdg-open", "xdg-open", url, nullptr);
#endif
    _exit(1);
  }
#endif
}

}  // namespace

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
  spdlog::set_level(spdlog::level::from_str(opts.logLevel));

  spdlog::info("Motion Master v{}", mm::core::kVersion);

  mm::node::DeviceManager deviceManager;

  auto makeDriver = [](const std::string& type, const std::string& adapter)
      -> std::expected<std::unique_ptr<mm::comm::FieldbusDriver>, std::string> {
    if (type == "soem") {
      return std::make_unique<mm::comm::soem::SoemFieldbusDriver>(adapter);
    }
    return std::unexpected("unsupported driver: " + type);
  };

  if (opts.driver.has_value()) {
    std::string ifname = opts.adapter ? opts.adapter->adapterName : "";
    auto driver = makeDriver(*opts.driver, ifname);
    if (!driver) {
      spdlog::error("{}", driver.error());
      return 1;
    }
    if (auto result = deviceManager.init(std::move(*driver)); !result) {
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
  const auto defaultCert = exeDir() / "cert.pem";
  const auto defaultKey = exeDir() / "key.pem";

  // Auto-discover TLS cert/key when not supplied via --cert/--key:
  //   1. cert.pem / key.pem next to the binary  (release install)
  //   2. ~/.acme.sh/local.motion-master.synapticon.com_ecc/  (local acme.sh)
  // Unlike before, a miss is not fatal here — the self-heal below fetches a fresh cert.
  if (opts.certFile.empty() || opts.keyFile.empty()) {
    if (std::filesystem::exists(defaultCert) && std::filesystem::exists(defaultKey)) {
      opts.certFile = defaultCert.string();
      opts.keyFile = defaultKey.string();
      spdlog::info("TLS: bundled cert ({})", opts.certFile);
    } else if (const char* home = std::getenv("HOME")) {
      const auto acmeDir =
          std::filesystem::path(home) / ".acme.sh/local.motion-master.synapticon.com_ecc";
      const auto acmeCert = acmeDir / "fullchain.cer";
      const auto acmeKey = acmeDir / "local.motion-master.synapticon.com.key";
      if (std::filesystem::exists(acmeCert) && std::filesystem::exists(acmeKey)) {
        opts.certFile = acmeCert.string();
        opts.keyFile = acmeKey.string();
        spdlog::info("TLS: Let's Encrypt cert from acme.sh ({})", opts.certFile);
      }
    }
  }

  // --update-cert: fetch a fresh cert/key into the resolved path (or the install-dir default when
  // nothing is configured), then exit without serving. The explicit path for terminal/headless use.
  if (opts.updateCert) {
    const std::string certTarget = opts.certFile.empty() ? defaultCert.string() : opts.certFile;
    const std::string keyTarget = opts.keyFile.empty() ? defaultKey.string() : opts.keyFile;
    spdlog::info("Fetching TLS certificate from {}", opts.certUrl);
    if (auto r = mm::fetchAndSwapCert(certTarget, keyTarget, opts.certUrl, opts.keyUrl); !r) {
      spdlog::error("Certificate update failed: {}", r.error());
      return 1;
    }
    spdlog::info("Installed fresh TLS certificate at {}", certTarget);
    return 0;
  }

  // Nothing resolved — target the install-dir default so the self-heal below can populate it.
  if (opts.certFile.empty() || opts.keyFile.empty()) {
    opts.certFile = defaultCert.string();
    opts.keyFile = defaultKey.string();
  }

  // Decide whether the served cert needs refreshing. A missing cert means we cannot serve TLS at
  // all; an expired cert still binds (browsers can bypass) but should be refreshed. Both are healed
  // by fetching from the rolling release unless --no-cert-update is set.
  bool needFetch = false;
  bool certMissing =
      !std::filesystem::exists(opts.certFile) || !std::filesystem::exists(opts.keyFile);
  if (certMissing) {
    needFetch = true;
    spdlog::warn("No TLS certificate at {}", opts.certFile);
  } else if (auto info = mm::readCertInfo(opts.certFile)) {
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
    if (opts.noCertUpdate) {
      if (certMissing) {
        spdlog::error("No certificate and --no-cert-update set — cannot serve TLS");
        return 1;
      }
      spdlog::error(
          "Certificate expired and --no-cert-update set — serving the expired certificate");
    } else {
      spdlog::warn("Fetching fresh TLS certificate from {}", opts.certUrl);
      if (auto r = mm::fetchAndSwapCert(opts.certFile, opts.keyFile, opts.certUrl, opts.keyUrl);
          r) {
        spdlog::info("Installed fresh TLS certificate at {}", opts.certFile);
      } else if (certMissing) {
        spdlog::error("Certificate fetch failed and no local certificate exists: {}", r.error());
        return 1;
      } else {
        spdlog::error("Certificate fetch failed: {} — serving the expired certificate", r.error());
      }
    }
  }

  // Owns the monitoring registry plus its background SDO-refresher and sampler threads. Wired to
  // the server below so the HTTP routes reach it and sampled batches publish over the WebSocket.
  mm::node::MonitoringManager monitoringManager{deviceManager};

  Server server{
      Server::Config{
          .port = opts.port,
          .certFile = opts.certFile,
          .keyFile = opts.keyFile,
          .version = std::string{mm::core::kVersion},
          .initDriver = [&deviceManager, makeDriver](
                            const std::string& type,
                            const std::string& adapter) -> std::expected<void, std::string> {
            std::string ifname = adapter;
            if (!adapter.empty()) {
              auto resolved = mm::comm::resolveNetworkAdapter(adapter);
              if (!resolved) return std::unexpected(resolved.error());
              ifname = resolved->adapterName;
            }
            auto driver = makeDriver(type, ifname);
            if (!driver) return std::unexpected(driver.error());
            return deviceManager.init(std::move(*driver));
          },
          .getLog = [ringLogSink]() { return ringLogSink->entries(); },
          .corsOrigin = opts.corsOrigin,
      },
      deviceManager, monitoringManager};
  server.start();

  // Publish each sampled batch to the WebSocket topic named after its monitoring, then start the
  // sampler + refresher threads. (Not added to the RT GameLoop — sampling runs off the RT thread.)
  monitoringManager.setPublish([&server](std::string topic, std::string json) {
    server.publish(std::move(topic), std::move(json));
  });
  monitoringManager.start();

  if (opts.openBrowser) {
    openInBrowser("https://motion-master.synapticon.com/app/");
  }

  GameLoop game_loop{std::chrono::microseconds{1000}};

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
  monitoringManager.stop();  // stop sampling/publishing before the server loop goes away
  server.stop();

  spdlog::info("Shutting down");
  return 0;
}
