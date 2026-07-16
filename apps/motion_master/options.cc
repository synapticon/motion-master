#include "options.h"

#include <spdlog/spdlog.h>

#include <CLI/CLI.hpp>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

#include "cert_updater.h"
#include "comm/base.h"
#include "config.h"
#include "core/version.h"

Options parseOptions(int argc, char** argv) {
  Options opts;
  opts.certUrl = mm::defaultCertUrl();
  opts.keyUrl = mm::defaultKeyUrl();

  // The CLI carries only actions and cert-fetch sources. Every tunable setting (ports, fieldbus,
  // log level, TLS paths) lives in the JSONC config file and has no command-line flag.
  CLI::App app{"Motion Master", "motion-master"};
  app.set_version_flag("--version", std::string{mm::core::kVersion});

  bool listAdapters = false;
  app.add_flag("--list-adapters", listAdapters,
               "Print network adapters (MAC -> interface) and exit");

  app.add_option("-c,--config", opts.configPath,
                 "Path to a JSONC config file — the only way to set ports, fieldbus, log level, "
                 "and TLS (see motion-master.example.jsonc)")
      ->check(CLI::ExistingFile);
  app.add_flag("--open", opts.openBrowser,
               "Open https://motion-master.synapticon.com/apps/console/ in the default browser");
  app.add_flag("--update-cert", opts.updateCert,
               "Download a fresh TLS cert/key, install them at the configured (or default) path, "
               "and exit");
  app.add_option("--cert-url", opts.certUrl, "Source URL for the TLS certificate")
      ->capture_default_str();
  app.add_option("--key-url", opts.keyUrl, "Source URL for the TLS private key")
      ->capture_default_str();

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    std::exit(app.exit(e));
  }

  if (listAdapters) {
    for (const auto& adapter : mm::comm::enumerateNetworkAdapters()) {
      std::cout << adapter.macLinux << "  " << adapter.name << "\n";
    }
    std::exit(0);
  }

  // Load the config file (JSONC: comments allowed) into the typed settings tree. --config is
  // optional: when omitted, configPath stays empty and opts.config keeps its in-code defaults.
  // When given, CLI::ExistingFile already guaranteed the path exists during app.parse().
  if (!opts.configPath.empty()) {
    std::ifstream f{opts.configPath};
    auto doc =
        nlohmann::json::parse(f, nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
    if (doc.is_discarded()) {
      spdlog::error("Failed to parse config file: {}", opts.configPath);
      std::exit(1);
    }
    auto parsed = parseConfig(doc);
    if (!parsed) {
      spdlog::error("Config error in {}: {}", opts.configPath, parsed.error());
      std::exit(1);
    }
    opts.config = std::move(*parsed);
    spdlog::debug("Loaded config from {}", opts.configPath);
  }

  return opts;
}
