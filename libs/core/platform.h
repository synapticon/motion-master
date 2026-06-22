#ifndef LIBS_CORE_PLATFORM_H_
#define LIBS_CORE_PLATFORM_H_

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

}  // namespace mm::core

#endif  // LIBS_CORE_PLATFORM_H_
