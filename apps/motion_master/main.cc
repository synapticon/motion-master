#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

#include <memory>

#include "comm/fieldbus_driver.h"
#include "comm/soem_fieldbus_driver.h"
#include "core/version.h"
#include "game_loop.h"
#include "node/device_manager.h"
#include "options.h"
#include "ring_log_sink.h"
#include "server.h"

namespace {

/// @brief Return the directory that contains the running executable.
/// @details argv[0] is not used — it can be a relative path, a bare command name resolved
///          via PATH, or a symlink.
///
///          **Platform behaviour**
///          - Linux: resolves `/proc/self/exe` via `std::filesystem::canonical`.
///          - Windows: queries the path via `GetModuleFileNameW`.
/// @return Absolute path to the executable's parent directory.
std::filesystem::path exeDir() {
#ifdef _WIN32
  wchar_t buf[MAX_PATH];
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return std::filesystem::path{buf}.parent_path();
#else
  return std::filesystem::canonical("/proc/self/exe").parent_path();
#endif
}

GameLoop* gGameLoop = nullptr;  ///< Signal handler target; set before run(), cleared after.

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

  // Auto-discover TLS cert/key when not supplied via --cert/--key:
  //   1. cert.pem / key.pem next to the binary  (release install)
  //   2. ~/.acme.sh/local.motion-master.synapticon.com_ecc/  (local acme.sh)
  if (opts.certFile.empty() || opts.keyFile.empty()) {
    const auto bundledCert = exeDir() / "cert.pem";
    const auto bundledKey = exeDir() / "key.pem";
    if (std::filesystem::exists(bundledCert) && std::filesystem::exists(bundledKey)) {
      opts.certFile = bundledCert.string();
      opts.keyFile = bundledKey.string();
      spdlog::info("TLS: bundled cert ({})", opts.certFile);
    } else if (const char* home = std::getenv("HOME")) {
      const auto acmeDir = std::filesystem::path(home) /
                           ".acme.sh/local.motion-master.synapticon.com_ecc";
      const auto acmeCert = acmeDir / "fullchain.cer";
      const auto acmeKey = acmeDir / "local.motion-master.synapticon.com.key";
      if (std::filesystem::exists(acmeCert) && std::filesystem::exists(acmeKey)) {
        opts.certFile = acmeCert.string();
        opts.keyFile = acmeKey.string();
        spdlog::info("TLS: Let's Encrypt cert from acme.sh ({})", opts.certFile);
      } else {
        spdlog::error(
            "No TLS certificate found — pass --cert/--key or place cert.pem/key.pem next to the "
            "binary");
        return 1;
      }
    } else {
      spdlog::error(
          "No TLS certificate found — pass --cert/--key or place cert.pem/key.pem next to the "
          "binary");
      return 1;
    }
  }

  auto swaggerFile = (exeDir() / "swagger.yml").string();

  Server server{
      Server::Config{
          .port = opts.port,
          .certFile = opts.certFile,
          .keyFile = opts.keyFile,
          .version = std::string{mm::core::kVersion},
          .swaggerFile = std::move(swaggerFile),
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
      deviceManager};
  server.start();

  GameLoop game_loop{std::chrono::microseconds{1000}};
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
  server.stop();

  spdlog::info("Shutting down");
  return 0;
}
