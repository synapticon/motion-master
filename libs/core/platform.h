#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

namespace mm::core {

/// @brief Return the directory that contains the running executable.
/// @details argv[0] is not used — it can be a relative path, a bare command name resolved
///          via PATH, or a symlink.
///
///          **Platform behaviour**
///          - Linux: resolves `/proc/self/exe` via `std::filesystem::canonical`.
///          - macOS: queries the path via `_NSGetExecutablePath` (no `/proc`).
///          - Windows: queries the path via `GetModuleFileNameW`.
/// @return Absolute path to the executable's parent directory.
std::filesystem::path exeDir();

/// @brief Open the given URL in the system default browser.
/// @details Non-blocking — returns immediately after spawning the browser process.
///          Uses xdg-open on Linux, `open` on macOS, and ShellExecute on Windows.
/// @param url URL to open.
void openInBrowser(const std::string& url);

/// @brief RAII holder of the process-wide single-instance lock.
/// @details Owns an OS handle whose lifetime *is* the lock: an @c flock'd file descriptor on
///          Linux/macOS, a named mutex on Windows. The OS releases the lock automatically when the
///          process exits for any reason — normal return, crash, or SIGKILL — so there is never a
///          stale lock to clean up (unlike a PID file). Move-only; destroy it (or move-assign over
///          it) to release the lock early.
class SingleInstanceLock {
 public:
  SingleInstanceLock() = default;
  SingleInstanceLock(SingleInstanceLock&& other) noexcept;
  SingleInstanceLock& operator=(SingleInstanceLock&& other) noexcept;
  SingleInstanceLock(const SingleInstanceLock&) = delete;
  SingleInstanceLock& operator=(const SingleInstanceLock&) = delete;
  ~SingleInstanceLock();

 private:
  explicit SingleInstanceLock(std::intptr_t handle) : handle_(handle) {}
  friend std::expected<SingleInstanceLock, std::string> acquireSingleInstanceLock();

  static constexpr std::intptr_t kInvalid = -1;  ///< Sentinel for "holds no lock" (also post-move).
  std::intptr_t handle_ = kInvalid;              ///< POSIX fd or Windows HANDLE, opaquely.
};

/// @brief Acquire the process-wide lock that permits only one running Motion Master.
/// @details Non-blocking. Returns the held lock when no other instance holds it; fails when one
///          already does. The lock guards the *whole process* — acquired before the EtherCAT NIC is
///          claimed and before the servers bind — so a second master can never drive the same bus
///          alongside the first. The exclusive port bind is only a backstop: it fires too late (the
///          NIC is already claimed by then) and misses instances configured on different ports, so
///          the lock, not the port, is the real guard.
///          - Linux/macOS: @c flock(LOCK_EX|LOCK_NB) on `<temp_dir>/motion-master.lock`
///            (machine-wide).
///          - Windows: a named mutex in the session-local namespace (per interactive session — a
///            standard user cannot create objects in the machine-global namespace).
/// @return The held lock on success; an error string describing the conflict on failure.
std::expected<SingleInstanceLock, std::string> acquireSingleInstanceLock();

}  // namespace mm::core
