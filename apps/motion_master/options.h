#pragma once

#include <string>

#include "config.h"

/// @brief Everything resolved from the command line and config file for one run.
///
/// The tunable settings live in @c config (built-in defaults, overlaid only by a JSONC config file
/// — none of them have a command-line flag). The remaining members are CLI-only: actions and
/// cert-fetch sources that have no place in the config file.
struct Options {
  std::string configPath;  ///< Effective config path: --config if given, else an auto-discovered
                           ///< motion-master.jsonc next to the executable; empty if neither exists.
  Config config;           ///< Settings: built-in defaults overlaid by the config file.
  bool openBrowser{false};  ///< Open the PWA in the default browser on start (--open).
  bool updateCert{false};   ///< Fetch a fresh cert/key, install them, and exit (--update-cert).
  std::string certUrl;      ///< --cert-url (CLI-only; defaults to the rolling release).
  std::string keyUrl;       ///< --key-url (CLI-only; defaults to the rolling release).
};

/// @brief Parse argv and load the config file into an Options value.
/// @details Loads a JSONC config file (comments allowed) into @c Options::config, layering it over
///          the built-in defaults; the settings themselves have no command-line flags — the CLI
///          carries only actions and cert-fetch sources. The config file is either the explicit
///          @c --config path or, absent that, a @c motion-master.jsonc auto-discovered next to the
///          executable (--config wins over it). On --list-adapters the adapters are printed and the
///          process exits 0.
/// @param argc Argument count forwarded from main().
/// @param argv Argument vector forwarded from main().
/// @return Fully populated Options.
/// @note Never returns on --help, --version, --list-adapters, a CLI parse error, or a config parse
///       or validation failure (config failures exit 1). The configured adapter is resolved later,
///       at driver init, not here.
Options parseOptions(int argc, char** argv);
