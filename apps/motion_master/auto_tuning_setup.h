#pragma once

#include <filesystem>
#include <optional>

#include "auto_tuning/client.h"
#include "auto_tuning/process.h"
#include "auto_tuning/status.h"
#include "config.h"

/// @file
/// @brief Startup and shutdown of the auto-tuning child process.
///
/// The process itself stays a local of @c main — it is neither copyable nor movable, and its
/// lifetime is the process lifetime. These functions carry the policy around it: where the
/// executable is, what a failed start means, and what a stop is worth logging.
///
/// Not having auto-tuning is a supported state, so nothing here is fatal.

namespace mm {

/// @brief The startup outcome, as the HTTP layer needs it.
struct AutoTuningStartup {
  /// The snapshot @c GET @c /api/auto-tuning serves. A page needs to tell the four states apart —
  /// switched off, not installed, would not start, running — because each asks something different
  /// of the person reading it.
  auto_tuning::Status status;
  /// Bound to the running process, and left empty when there is none: an unset callback is how the
  /// routes answer 503 without naming any of this.
  std::optional<auto_tuning::Client> client;
};

/// @brief Resolves what to start, and where its output goes.
///
/// @param config   The @c "autoTuning" block of the configuration file.
/// @param logFile  Motion Master's own log file, or empty when it has none. The child's log is
///                 placed beside it, so the child's output is listed and downloadable through
///                 @c /api/user-cache like everything else this process writes. Without a log file
///                 of our own there is nowhere better than our streams, which is what an empty
///                 path selects.
auto_tuning::ProcessOptions buildAutoTuningOptions(const AutoTuningConfig& config,
                                                   const std::filesystem::path& logFile);

/// @brief Starts the child unless the configuration or the filesystem says otherwise, and logs it.
///
/// **Call before the calling thread becomes real-time.** A child inherits the scheduling policy of
/// the thread that spawned it, and this one runs a spin-waiting numerical worker per core.
///
/// @param process  The process to start. Its options name the binary that is looked for.
/// @param enabled  @c AutoTuningConfig::enabled.
/// @return The status snapshot, and a client when something is running.
AutoTuningStartup startAutoTuning(auto_tuning::Process& process, bool enabled);

/// @brief Stops the child and logs how it ended.
///
/// Called explicitly rather than left to the destructor, so what happened to it is logged while the
/// log is still being written. A Motion Master that exits leaves nothing of its own running.
void stopAutoTuning(auto_tuning::Process& process);

}  // namespace mm
