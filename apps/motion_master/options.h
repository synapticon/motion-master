#pragma once

#include <string>

#include "config.h"

/// @brief Everything resolved from the command line and config file for one run.
///
/// The tunable settings live in @c config (built-in defaults, overlaid by the @c --config file,
/// overlaid by explicit CLI flags). The remaining members are CLI-only: actions and runtime values
/// that have no place in the config file.
struct Options {
  std::string configPath;   ///< --config path; empty if not given.
  Config config;            ///< Settings: built-in defaults ⊕ config file ⊕ CLI overrides.
  bool openBrowser{false};  ///< Open the PWA in the default browser on start (--open).
  bool updateCert{false};   ///< Fetch a fresh cert/key, install them, and exit (--update-cert).
  std::string certUrl;      ///< --cert-url (CLI-only; defaults to the rolling release).
  std::string keyUrl;       ///< --key-url (CLI-only; defaults to the rolling release).
};

/// @brief Parse argv and load the config file into an Options value.
/// @details Loads the @c --config file first (JSONC, comments allowed) into @c Options::config,
///          then lets explicit CLI flags override individual fields — precedence is
///          CLI flag > config file > built-in default. On --list-adapters the adapters are printed
///          and the process exits 0.
/// @param argc Argument count forwarded from main().
/// @param argv Argument vector forwarded from main().
/// @return Fully populated Options.
/// @note Never returns on --help, --version, --list-adapters, a CLI parse error, or a config parse
///       or validation failure (config failures exit 1). The configured adapter is resolved later,
///       at driver init, not here.
Options parseOptions(int argc, char** argv);
