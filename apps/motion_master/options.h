#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

/// All command-line options and loaded configuration for Motion Master.
struct Options {
  std::string config;                          ///< Path to the JSONC config file; empty if not given.
  uint16_t port{8443};                         ///< HTTP/WebSocket listen port.
  std::string cert_file;                       ///< Path to the TLS certificate file.
  std::string key_file;                        ///< Path to the TLS private key file.
  std::string driver{"soem"};                  ///< Fieldbus driver: "soem", "spoe", or "igh".
  std::string log_level{"info"};               ///< spdlog level: trace/debug/info/warn/error.
  std::optional<nlohmann::json> config_data;   ///< Parsed config; absent when --config is not given.
};

/// @brief Parse argv and load the config file into an Options value.
/// @details CLI flags are parsed first via CLI11. If --config is given the named file is
///          read and parsed as JSON; on failure the process exits with code 1.
///          If --list-adapters is given, network adapters are printed to stdout and the
///          process exits with code 0 before an Options value is constructed.
/// @param argc Argument count forwarded from main().
/// @param argv Argument vector forwarded from main().
/// @return Fully populated Options.
/// @note Never returns on --help, --version, --list-adapters, a CLI parse error, or a
///       config parse failure; exit codes for CLI11-handled flags are determined by CLI11,
///       --list-adapters uses 0, and config failure uses 1.
Options parseOptions(int argc, char** argv);
