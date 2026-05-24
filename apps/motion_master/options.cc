#include "options.h"

#include <spdlog/spdlog.h>

#include <CLI/CLI.hpp>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>

#include "comm/base.h"
#include "core/version.h"

Options parseOptions(int argc, char** argv) {
  Options opts;

  CLI::App app{"Motion Master", "motion-master"};
  app.set_version_flag("--version", std::string{mm::core::kVersion});

  bool listAdapters = false;
  app.add_flag("--list-adapters", listAdapters,
               "Print network adapters (MAC -> interface) and exit");

  app.add_option("-c,--config", opts.config, "Path to JSON config file")->check(CLI::ExistingFile);
  app.add_option("-p,--port", opts.port, "HTTP/WebSocket port")->capture_default_str();
  app.add_option("--cert", opts.certFile, "TLS certificate file")->check(CLI::ExistingFile);
  app.add_option("--key", opts.keyFile, "TLS private key file")->check(CLI::ExistingFile);
  std::string driverInput;
  auto* driverOpt = app.add_option("-d,--driver", driverInput, "Fieldbus driver (soem|spoe|igh)")
                        ->check(CLI::IsMember({"soem", "spoe", "igh"}));
  app.add_option("-l,--log-level", opts.logLevel, "Log level")
      ->capture_default_str()
      ->check(CLI::IsMember({"trace", "debug", "info", "warn", "error"}));

  std::string adapterInput;
  app.add_option("--adapter", adapterInput,
                 "Network adapter: MAC (AA:BB:CC:DD:EE:FF or AA-BB-CC-DD-EE-FF)"
                 " or interface name (eth0)");
  app.add_option("--cors-origin", opts.corsOrigin, "Allowed CORS origin")->capture_default_str();

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    std::exit(app.exit(e));
  }

  if (listAdapters) {
    for (const auto& [mac, iface] : mm::comm::mapMacAddressesToInterfaces()) {
      std::cout << mac << "  " << iface << "\n";
    }
    std::exit(0);
  }

  if (driverOpt->count() > 0) {
    opts.driver = driverInput;
  }

  if (!adapterInput.empty()) {
    auto adapter = mm::comm::resolveNetworkAdapter(adapterInput);
    if (!adapter) {
      spdlog::error("{}", adapter.error());
      std::exit(1);
    }
    opts.adapter = *adapter;
  }

  if (!opts.config.empty()) {
    std::ifstream f{opts.config};
    auto cfg = nlohmann::json::parse(f, nullptr, false);
    if (cfg.is_discarded()) {
      spdlog::error("Failed to parse config file: {}", opts.config);
      std::exit(1);
    }
    spdlog::debug("Loaded config from {}", opts.config);
    opts.configData = std::move(cfg);
  }

  return opts;
}
