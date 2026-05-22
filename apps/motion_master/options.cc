#include "options.h"

#include <spdlog/spdlog.h>

#include <CLI/CLI.hpp>
#include <cstdlib>
#include <fstream>

#include "core/version.h"

Options parseOptions(int argc, char** argv) {
  Options opts;

  CLI::App app{"Motion Master", "motion-master"};
  app.set_version_flag("--version", std::string{mm::core::kVersion});

  app.add_option("-c,--config", opts.config, "Path to JSON config file")->check(CLI::ExistingFile);
  app.add_option("-p,--port", opts.port, "HTTP/WebSocket port")->capture_default_str();
  app.add_option("--cert", opts.cert_file, "TLS certificate file")->check(CLI::ExistingFile);
  app.add_option("--key", opts.key_file, "TLS private key file")->check(CLI::ExistingFile);
  app.add_option("-d,--driver", opts.driver, "Fieldbus driver")
      ->capture_default_str()
      ->check(CLI::IsMember({"soem", "spoe", "igh"}));
  app.add_option("-l,--log-level", opts.log_level, "Log level")
      ->capture_default_str()
      ->check(CLI::IsMember({"trace", "debug", "info", "warn", "error"}));

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    std::exit(app.exit(e));
  }

  if (!opts.config.empty()) {
    std::ifstream f{opts.config};
    auto cfg = nlohmann::json::parse(f, nullptr, false);
    if (cfg.is_discarded()) {
      spdlog::error("Failed to parse config file: {}", opts.config);
      std::exit(1);
    }
    spdlog::debug("Loaded config from {}", opts.config);
    opts.config_data = std::move(cfg);
  }

  return opts;
}
