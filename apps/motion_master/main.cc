#include <spdlog/spdlog.h>

#include <csignal>
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
#include "device_manager.h"
#include "game_loop.h"
#include "options.h"
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
  auto opts = parseOptions(argc, argv);

  spdlog::info("Motion Master v{}", mm::core::kVersion);

  std::unique_ptr<mm::comm::FieldbusDriver> fieldbusDriver;
  if (opts.driver == "soem") {
    std::string ifname = opts.adapter ? opts.adapter->adapterName : "";
    fieldbusDriver = std::make_unique<mm::comm::soem::SoemFieldbusDriver>(ifname);
  } else {
    spdlog::error("Unsupported driver: {}", opts.driver);
    return 1;
  }

  DeviceManager deviceManager{*fieldbusDriver};

  if (auto result = deviceManager.init(); !result) {
    spdlog::error("DeviceManager init failed: {}", result.error());
    return 1;
  }

  if (auto result = deviceManager.configure(); !result) {
    spdlog::error("DeviceManager configure failed: {}", result.error());
    return 1;
  } else {
    spdlog::info("Found {} slave(s)", *result);
    for (const auto& device : deviceManager.devices()) {
      spdlog::info("  [{:2}] {} — vendor: {:#010x}  product: {:#010x}  rev: {:#010x}  serial: {}",
                   device.slavePosition(), device.name(), device.vendorId(),
                   device.productCode(), device.revisionNumber(), device.serialNumber());
    }
  }

  auto swaggerFile = (exeDir() / "swagger.yml").string();

  Server server{Server::Config{
      .port = opts.port,
      .certFile = opts.certFile,
      .keyFile = opts.keyFile,
      .version = std::string{mm::core::kVersion},
      .swaggerFile = std::move(swaggerFile),
  }};
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
