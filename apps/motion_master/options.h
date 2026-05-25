#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "comm/base.h"

/// All command-line options and loaded configuration for Motion Master.
struct Options {
  std::string config;    ///< Path to the JSONC config file; empty if not given.
  uint16_t port{8443};   ///< HTTP/WebSocket listen port.
  std::string certFile;  ///< Path to the TLS certificate file.
  std::string keyFile;   ///< Path to the TLS private key file.
  /// Fieldbus driver: "soem", "spoe", or "igh". Absent means defer init to the HTTP API.
  std::optional<std::string> driver;
  std::string logLevel{"info"};                     ///< spdlog level: trace/debug/info/warn/error.
  std::optional<mm::comm::NetworkAdapter> adapter;  ///< Absent when --adapter is not given.
  std::optional<nlohmann::json> configData;  ///< Parsed config; absent when --config not given.
  /// Allowed CORS origin. Defaults to the production PWA origin.
  std::string corsOrigin{"https://motion-master.synapticon.com"};
  bool openBrowser{false};  ///< Open https://motion-master.synapticon.com/app/ in browser on start.
};

/// @brief Parse argv and load the config file into an Options value.
/// @details CLI flags are parsed first via CLI11. If --config is given the named file is
///          read and parsed as JSON; on failure the process exits with code 1.
///          If --list-adapters is given, network adapters are printed to stdout and the
///          process exits with code 0 before an Options value is constructed.
/// @param argc Argument count forwarded from main().
/// @param argv Argument vector forwarded from main().
/// @return Fully populated Options.
/// @note Never returns on --help, --version, --list-adapters, a CLI parse error, a config
///       parse failure, or an unresolvable --adapter; exit codes for CLI11-handled flags are
///       determined by CLI11, --list-adapters uses 0, and config/adapter failures use 1.
Options parseOptions(int argc, char** argv);
