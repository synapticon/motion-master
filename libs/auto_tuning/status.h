#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

/// @file
/// @brief What Motion Master knows about its auto-tuning process, as a client can read it.

namespace mm::auto_tuning {

/// @brief The state of the auto-tuning process, reported by @c GET @c /api/auto-tuning.
///
/// Four states matter to a user interface, and they need different words: auto-tuning was switched
/// off in the configuration, the executable is not on this machine, it is there but would not run,
/// or it is running. A page that can say which one is showing is far more use than a form that
/// fails.
///
/// This is a startup snapshot. @c started says the process answered when Motion Master waited for
/// it, not that it is alive now — nothing polls it, because a request is the honest test and the
/// endpoints make one.
struct Status {
  /// @c autoTuning.enabled from the configuration.
  bool enabled = false;
  /// The executable Motion Master looked for, resolved to an absolute path.
  std::string binaryPath;
  /// Whether a file exists at that path.
  bool installed = false;
  /// Whether the process started and answered its health endpoint.
  bool started = false;
  /// The version it reported, empty unless it started. A build older than the version endpoint
  /// starts and reports nothing, which is why this is not the test for @c started.
  std::string version;
  /// The loopback port it was told to serve on.
  std::uint16_t port = 0;
  /// Why it is not running, when it is not. Empty when it is.
  std::string error;
};

/// @brief Serialises a @c Status. Free function found by ADL, as nlohmann expects.
inline void to_json(nlohmann::json& j, const Status& status) {
  j = nlohmann::json{{"enabled", status.enabled},     {"binaryPath", status.binaryPath},
                     {"installed", status.installed}, {"started", status.started},
                     {"version", status.version},     {"port", status.port},
                     {"error", status.error}};
}

}  // namespace mm::auto_tuning
