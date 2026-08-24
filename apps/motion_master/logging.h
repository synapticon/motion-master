#pragma once

#include <spdlog/sinks/stdout_color_sinks.h>

#include <filesystem>
#include <memory>

#include "config.h"
#include "ring_log_sink.h"

/// @file
/// @brief Startup of the process-wide logger: the sinks, their levels, and the flush policy.
///
/// Split in two because the order matters. @c installLogSinks runs before the configuration is
/// read, so that a config-file error is itself logged and reaches @c GET @c /api/log;
/// @c applyLoggingConfig then applies what that file asked for.

namespace mm {

/// @brief The two sinks that exist before any configuration is read.
struct LogSinks {
  /// Backs @c GET @c /api/log. The HTTP server borrows it for the lifetime of the process.
  std::shared_ptr<RingLogSinkMt> ring;
  std::shared_ptr<spdlog::sinks::stdout_color_sink_mt> console;
};

/// @brief Replaces the default logger with a console sink plus the in-memory ring.
///
/// Call this first, before anything that logs. Levels stay at spdlog's defaults until
/// @c applyLoggingConfig runs, so the lines emitted while the configuration is being read are kept.
///
/// @return The two sinks, for @c applyLoggingConfig and for the @c GET @c /api/log route.
LogSinks installLogSinks();

/// @brief Applies the configured levels and adds the rotating file sink.
///
/// @param sinks          What @c installLogSinks returned.
/// @param config         The @c "logging" block of the configuration file.
/// @param userCacheRoot  Root the default @c logs directory hangs off.
/// @return The active log file, or an empty path when logging to a file is off or the file could
///         not be opened. An unwritable directory is a reason to run without a log file, not a
///         reason not to run, so this never fails.
std::filesystem::path applyLoggingConfig(const LogSinks& sinks, const LoggingConfig& config,
                                         const std::filesystem::path& userCacheRoot);

}  // namespace mm
