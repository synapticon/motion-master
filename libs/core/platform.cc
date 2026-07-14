#include "core/platform.h"

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <shellapi.h>  // ShellExecuteA — not pulled in by <windows.h> under WIN32_LEAN_AND_MEAN
// clang-format on
#else
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>  // _NSGetExecutablePath — macOS has no /proc/self/exe
#include <cstdint>        // uint32_t — only the macOS exeDir() branch uses it
#endif
#endif

#include <string>

namespace mm::core {

std::filesystem::path exeDir() {
#ifdef _WIN32
  wchar_t buf[MAX_PATH];
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return std::filesystem::path{buf}.parent_path();
#elif defined(__APPLE__)
  // First call reports the required buffer size (including the null terminator).
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buf(size, '\0');
  _NSGetExecutablePath(buf.data(), &size);
  // The returned path may contain symlinks or `..`; canonical() resolves them.
  return std::filesystem::canonical(buf.c_str()).parent_path();
#else
  return std::filesystem::canonical("/proc/self/exe").parent_path();
#endif
}

void openInBrowser(const std::string& url) {
#ifdef _WIN32
  ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
  pid_t pid = fork();
  if (pid == 0) {
#ifdef __APPLE__
    execlp("open", "open", url.c_str(), nullptr);
#else
    execlp("xdg-open", "xdg-open", url.c_str(), nullptr);
#endif
    _exit(1);
  }
#endif
}

}  // namespace mm::core
