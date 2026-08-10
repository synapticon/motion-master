#include "core/platform.h"

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <shellapi.h>  // ShellExecuteA — not pulled in by <windows.h> under WIN32_LEAN_AND_MEAN
// clang-format on
#else
#include <fcntl.h>     // open, O_* flags
#include <sys/file.h>  // flock
#include <unistd.h>    // close

#include <cerrno>  // errno
#ifdef __APPLE__
#include <mach-o/dyld.h>  // _NSGetExecutablePath — macOS has no /proc/self/exe

#include <cstdint>  // uint32_t — only the macOS exeDir() branch uses it
#endif
#endif

#include <cstdlib>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

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

namespace {

/// The value of an environment variable, or nullopt when it is unset *or* empty — an empty value is
/// as unusable as an absent one for a path root, and both must fall through to the next candidate.
std::optional<std::string> envVar(const char* name) {
  // getenv races only a concurrent setenv/putenv, and the shipped binary calls neither. (The one
  // caller that does is a test fixture, which sets the variable on the gtest main thread before
  // starting anything.) std::getenv is the only portable reader; secure_getenv is glibc-only.
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  const char* v = std::getenv(name);
  if (v == nullptr || *v == '\0') {
    return std::nullopt;
  }
  return std::string{v};
}

}  // namespace

std::filesystem::path userCacheDir() {
#if defined(_WIN32)
  if (auto p = envVar("LOCALAPPDATA")) {
    return std::filesystem::path(*p) / "motion-master";
  }
#elif defined(__APPLE__)
  if (auto p = envVar("HOME")) {
    return std::filesystem::path(*p) / "Library" / "Caches" / "motion-master";
  }
#else
  if (auto p = envVar("XDG_CACHE_HOME")) {
    return std::filesystem::path(*p) / "motion-master";
  }
  if (auto p = envVar("HOME")) {
    return std::filesystem::path(*p) / ".cache" / "motion-master";
  }
#endif
  std::error_code ec;
  return std::filesystem::temp_directory_path(ec) / "motion-master";
}

std::string errnoMessage(int err) { return std::system_category().message(err); }

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

SingleInstanceLock::SingleInstanceLock(SingleInstanceLock&& other) noexcept
    : handle_(other.handle_) {
  other.handle_ = kInvalid;
}

SingleInstanceLock& SingleInstanceLock::operator=(SingleInstanceLock&& other) noexcept {
  // Swap so `other`'s destructor releases whatever this instance was holding; self-move is a no-op.
  std::swap(handle_, other.handle_);
  return *this;
}

SingleInstanceLock::~SingleInstanceLock() {
  if (handle_ == kInvalid) {
    return;
  }
#ifdef _WIN32
  CloseHandle(reinterpret_cast<HANDLE>(handle_));  // releases the named mutex
#else
  ::close(static_cast<int>(handle_));  // closing the fd drops the flock
#endif
}

std::expected<SingleInstanceLock, std::string> acquireSingleInstanceLock() {
#ifdef _WIN32
  // Session-local namespace (no "Global\\" prefix): a standard interactive user lacks the
  // privilege to create machine-global objects, and Motion Master runs interactively on Windows.
  HANDLE mutex = CreateMutexW(nullptr, TRUE, L"motion-master-single-instance");
  if (mutex == nullptr) {
    return std::unexpected("cannot create single-instance mutex (error " +
                           std::to_string(GetLastError()) + ")");
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(mutex);  // we got a handle to the existing mutex; release it and refuse to start
    return std::unexpected("another Motion Master instance is already running");
  }
  return SingleInstanceLock{reinterpret_cast<std::intptr_t>(mutex)};
#else
  const auto path = std::filesystem::temp_directory_path() / "motion-master.lock";
  // The lock is the flock on the fd, not the file's existence — the file is left behind on exit and
  // simply re-locked next run, so there is no unlink race. O_CLOEXEC keeps it out of any child.
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) {
    return std::unexpected("cannot open lock file " + path.string() + ": " + errnoMessage(errno));
  }
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    const int err = errno;
    ::close(fd);
    if (err == EWOULDBLOCK) {
      return std::unexpected("another Motion Master instance is already running (lock held on " +
                             path.string() + ")");
    }
    return std::unexpected("cannot lock " + path.string() + ": " + errnoMessage(err));
  }
  return SingleInstanceLock{static_cast<std::intptr_t>(fd)};
#endif
}

}  // namespace mm::core
