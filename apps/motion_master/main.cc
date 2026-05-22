#include <spdlog/spdlog.h>

#include <csignal>
#include <filesystem>
#include <string>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

#include "core/version.h"
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
std::filesystem::path exe_dir() {
#ifdef _WIN32
  wchar_t buf[MAX_PATH];
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return std::filesystem::path{buf}.parent_path();
#else
  return std::filesystem::canonical("/proc/self/exe").parent_path();
#endif
}

GameLoop* g_game_loop = nullptr;  ///< Signal handler target; set before run(), cleared after.

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

  auto swagger_file = (exe_dir() / "swagger.yml").string();

  Server server{Server::Config{
      .port = opts.port,
      .cert_file = opts.cert_file,
      .key_file = opts.key_file,
      .version = std::string{mm::core::kVersion},
      .swagger_file = std::move(swagger_file),
  }};
  server.start();

  GameLoop game_loop{std::chrono::microseconds{1000}};
  g_game_loop = &game_loop;

  std::signal(SIGINT, [](int) {
    if (g_game_loop) {
      g_game_loop->stop();
    }
  });
  std::signal(SIGTERM, [](int) {
    if (g_game_loop) {
      g_game_loop->stop();
    }
  });

  game_loop.run();  // main thread IS the RT loop — blocks until stop()

  g_game_loop = nullptr;
  server.stop();

  spdlog::info("Shutting down");
  return 0;
}
