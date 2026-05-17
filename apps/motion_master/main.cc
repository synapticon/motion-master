#include <spdlog/spdlog.h>

#include <CLI/CLI.hpp>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

#include "core/version.h"

int main(int argc, char** argv) {
  CLI::App app{"Motion Master", "motion-master"};
  app.set_version_flag("--version", std::string{mm::core::kVersion});

  std::string config;
  app.add_option("-c,--config", config, "Path to JSON config file")->check(CLI::ExistingFile);

  uint16_t port = 8443;
  app.add_option("-p,--port", port, "HTTP/WebSocket port")->capture_default_str();

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

  return 0;
}
