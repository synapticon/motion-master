#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

/// @file
/// @brief The three platform-specific steps of running a child process.
///
/// Internal to @c mm::auto_tuning. Implemented once per platform (@c process_posix.cc,
/// @c process_windows.cc) so @c process.cc holds the part that is the same everywhere: the health
/// wait, the version, and the error messages.
///
/// A started child is identified by integers, which is what lets @c process.h name no platform
/// type. On POSIX only the process id is used, and it doubles as the process group id. On Windows
/// the id is for the log line, @c handle is the process, and @c group is a job object.
///
/// **The child is a launcher that runs a second process.** The auto-tuning executable unpacks
/// itself on every start and runs the unpacked program as its own child, and that grandchild is the
/// one that holds the port. Measured: a termination signal to the launcher does reach it, because
/// the launcher forwards the signal — but a kill cannot be forwarded, and a kill is exactly what
/// the wedged case needs. So the child is started in its own process group (POSIX) or job object
/// (Windows), and both signals go to that rather than to the one process.

namespace mm::auto_tuning::detail {

/// @brief A started child, as the platform identifies it.
struct Child {
  std::int64_t pid = 0;
  std::intptr_t handle = 0;
  std::intptr_t group = 0;
};

/// @brief Runs @p binary with @p args, with its output appended to @p logFile.
///
/// The child inherits this process's stdout and stderr when @p logFile is empty. It never inherits
/// stdin: the auto-tuning program reads none, and a child sharing a terminal's stdin can stop the
/// whole process group.
///
/// @return The started child, or an error string. A missing or non-executable @p binary is
///         reported here, by the platform, rather than pre-checked — a check followed by a spawn
///         would still have to handle the spawn failing.
std::expected<Child, std::string> spawnChild(const std::filesystem::path& binary,
                                             const std::vector<std::string>& args,
                                             const std::filesystem::path& logFile);

/// @brief Whether @p child is still running.
///
/// **This reaps a child that has exited**, which is how the exit is detected at all, and it is why
/// only @c Process::start calls it: a reaped process id may be reissued to an unrelated process, so
/// the caller must forget the child as soon as this returns false.
bool childAlive(const Child& child);

/// @brief Asks @p child to exit, waits up to @p grace, then kills it and everything it started.
///
/// Returns once the child is gone. The auto-tuning program handles a termination signal and shuts
/// its server down; the kill is for a child wedged inside a numerical routine, and it takes the
/// whole group so that nothing is left holding the port.
void terminateChild(const Child& child, std::chrono::milliseconds grace);

}  // namespace mm::auto_tuning::detail
