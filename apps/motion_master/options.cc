#include "options.h"

#include <spdlog/spdlog.h>

#include <CLI/CLI.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

#include "cert_updater.h"
#include "comm/base.h"
#include "config.h"
#include "core/platform.h"
#include "core/version.h"
#include "object_address_generator.h"

// Every std::exit below carries a NOLINT for concurrency-mt-unsafe. The call is unsafe only
// against other threads running during teardown, and this function runs before main starts any:
// it is the argument parser, and each exit is the CLI refusing to start (a parse error,
// --list-adapters, an unreadable config).
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
                 "Path to a JSONC config file — ports, fieldbus, log level, and TLS live only in "
                 "such a file (see motion-master.example.jsonc). Overrides the motion-master.jsonc "
                 "auto-discovered next to the executable")
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

  // A developer action rather than a setting: it reads a vendor's ESI, writes the generated
  // object-address headers, and exits. Here rather than in a separate tool because it needs the
  // very ESI parser the server already links, and a second binary to carry one function is a
  // second binary to build, package and keep in step.
  std::string esiPath;
  std::string outDir;
  auto* generate =
      app.add_subcommand("generate-object-addresses",
                         "Generate the ObjectAddress headers from a vendor's ESI file and exit");
  generate->add_option("--esi", esiPath, "Path to the ESI XML to read")
      ->required()
      ->check(CLI::ExistingFile);
  generate->add_option("--out", outDir, "Directory to write the headers into")
      ->required()
      ->check(CLI::ExistingDirectory);

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    std::exit(app.exit(e));
  }

  if (*generate) {
    const auto result = mm::generateObjectAddresses(esiPath, outDir);
    if (!result) {
      std::cerr << result.error() << "\n";
      // NOLINTNEXTLINE(concurrency-mt-unsafe)
      std::exit(1);
    }
    for (const auto& warning : result->warnings) {
      std::cerr << "warning: " << warning << "\n";
    }
    for (const auto& path : result->files) {
      std::cout << path << "\n";
    }
    std::cout << result->rows << " object addresses from " << esiPath << "\n";
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    std::exit(0);
  }

  if (listAdapters) {
    for (const auto& adapter : mm::comm::enumerateNetworkAdapters()) {
      std::cout << adapter.macLinux << "  " << adapter.name << "\n";
    }
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    std::exit(0);
  }

  // Resolve which config file to load (JSONC: comments allowed). An explicit --config always wins;
  // CLI::ExistingFile already guaranteed its path exists during app.parse(). Absent --config, fall
  // back to a "motion-master.jsonc" sitting next to the executable — the auto-discovered bundled
  // config the Windows release ships to raise the RT period (this is the only implicit path, and it
  // is deliberately next-to-the-binary, never a system-wide /etc lookup). With neither, opts.config
  // keeps its in-code defaults.
  std::string configPath = opts.configPath;
  bool autoDiscovered = false;
  if (configPath.empty()) {
    const auto bundled = mm::core::exeDir() / "motion-master.jsonc";
    std::error_code ec;
    if (std::filesystem::exists(bundled, ec)) {
      configPath = bundled.string();
      autoDiscovered = true;
    }
  }

  if (!configPath.empty()) {
    std::ifstream f{configPath};
    auto doc =
        nlohmann::json::parse(f, nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
    if (doc.is_discarded()) {
      spdlog::error("Failed to parse config file: {}", configPath);
      // NOLINTNEXTLINE(concurrency-mt-unsafe)
      std::exit(1);
    }
    auto parsed = parseConfig(doc);
    if (!parsed) {
      spdlog::error("Config error in {}: {}", configPath, parsed.error());
      // NOLINTNEXTLINE(concurrency-mt-unsafe)
      std::exit(1);
    }
    opts.config = std::move(*parsed);
    opts.configPath = configPath;
    spdlog::debug("Loaded {}config from {}", autoDiscovered ? "auto-discovered " : "", configPath);
  }

  return opts;
}
