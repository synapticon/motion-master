#include <spdlog/spdlog.h>

#include <CLI/CLI.hpp>
#include <atomic>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

#include "core/version.h"
#include "server.h"

namespace {
std::atomic<bool> g_quit{false};

std::filesystem::path exe_dir() {
  // argv[0] is unreliable — it can be a relative path, a bare command name
  // resolved via PATH, or a symlink — so we ask the OS for the real path instead.
#ifdef _WIN32
  wchar_t buf[MAX_PATH];
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return std::filesystem::path{buf}.parent_path();
#else
  return std::filesystem::canonical("/proc/self/exe").parent_path();
#endif
}
}

int main(int argc, char** argv) {
  CLI::App app{"Motion Master", "motion-master"};
  app.set_version_flag("--version", std::string{mm::core::kVersion});

  std::string config;
  app.add_option("-c,--config", config, "Path to JSON config file")->check(CLI::ExistingFile);

  uint16_t port = 8443;
  app.add_option("-p,--port", port, "HTTP/WebSocket port")->capture_default_str();

  std::string cert_file;
  app.add_option("--cert", cert_file, "TLS certificate file")->check(CLI::ExistingFile);

  std::string key_file;
  app.add_option("--key", key_file, "TLS private key file")->check(CLI::ExistingFile);

  std::string driver = "soem";
  app.add_option("-d,--driver", driver, "Fieldbus driver")
      ->capture_default_str()
      ->check(CLI::IsMember({"soem", "spoe", "igh"}));

  std::string log_level = "info";
  app.add_option("-l,--log-level", log_level, "Log level")
      ->capture_default_str()
      ->check(CLI::IsMember({"trace", "debug", "info", "warn", "error"}));

  CLI11_PARSE(app, argc, argv);

  spdlog::info("Motion Master v{}", mm::core::kVersion);

  if (!config.empty()) {
    std::ifstream f{config};
    auto cfg = nlohmann::json::parse(f, nullptr, false);
    if (cfg.is_discarded()) {
      spdlog::error("Failed to parse config file: {}", config);
      return 1;
    }

    spdlog::debug("Loaded config from {}", config);
  }

  std::signal(SIGINT, [](int) { g_quit = true; });
  std::signal(SIGTERM, [](int) { g_quit = true; });

  // swagger.yml ships alongside the binary, so no user configuration is needed.
  auto swagger_file = (exe_dir() / "swagger.yml").string();

  Server server{Server::Config{
      .port = port,
      .cert_file = cert_file,
      .key_file = key_file,
      .version = std::string{mm::core::kVersion},
      .swagger_file = std::move(swagger_file),
  }};
  server.start();

  while (!g_quit) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  spdlog::info("Shutting down");
  server.stop();

  return 0;
}
