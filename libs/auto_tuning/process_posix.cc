#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "auto_tuning/process_platform.h"
#include "core/platform.h"

// The child needs this process's environment: the auto-tuning executable unpacks itself into a
// temporary directory on every start, and it finds that directory through TMPDIR and HOME. Declared
// here rather than taken from a header, because unistd.h exposes it only under _GNU_SOURCE and
// macOS does not expose it to a library at all. The symbol is in libc on both.
extern "C" char** environ;  // NOLINT(readability-redundant-declaration)

namespace mm::auto_tuning::detail {

namespace {

/// How often to check whether a signalled child has gone.
constexpr std::chrono::milliseconds kReapPollInterval{20};

/// @brief Whether waitpid has already collected the child, so its pid means nothing more.
bool reaped(std::int64_t pid) {
  int status = 0;
  const pid_t result = waitpid(static_cast<pid_t>(pid), &status, WNOHANG);
  // 0 means the child exists and has not exited. A pid means it exited and this call reaped it.
  // -1 means there is no such child, which happens when a previous call already reaped it.
  return result != 0;
}

}  // namespace

std::expected<Child, std::string> spawnChild(const std::filesystem::path& binary,
                                             const std::vector<std::string>& args,
                                             const std::filesystem::path& logFile) {
  // posix_spawn rather than fork plus exec. Motion Master is multi-threaded by the time anything
  // spawns, and a forked child may only call async-signal-safe functions before it execs — a rule
  // that is easy to break by adding one line. posix_spawn does the same thing in one call, with the
  // redirections declared up front.
  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) {
    return std::unexpected("cannot prepare the child's file descriptors");
  }

  // stdin comes from /dev/null. The auto-tuning program reads none, and a child that shares a
  // terminal's stdin can suspend the whole process group by reading from it.
  bool ok = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) == 0;
  if (!logFile.empty()) {
    ok = ok && posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, logFile.c_str(),
                                                O_WRONLY | O_CREAT | O_APPEND, 0644) == 0;
    // stderr goes to the same file rather than being opened twice, so the two streams interleave in
    // write order instead of overwriting each other through separate file offsets.
    ok = ok && posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO) == 0;
  }
  if (!ok) {
    posix_spawn_file_actions_destroy(&actions);
    return std::unexpected("cannot redirect the child's output to " + logFile.string());
  }

  // execv's argv: the program name, the arguments, and a null terminator. The strings must outlive
  // the call, so they are borrowed from the caller's vector rather than built here.
  const std::string binaryString = binary.string();
  std::vector<char*> argv;
  argv.reserve(args.size() + 2);
  argv.push_back(const_cast<char*>(binaryString.c_str()));
  std::transform(args.begin(), args.end(), std::back_inserter(argv),
                 [](const std::string& arg) { return const_cast<char*>(arg.c_str()); });
  argv.push_back(nullptr);

  // Its own process group, for two reasons. A kill can then take the launcher and the program it
  // unpacked together, which a signal to one process cannot do. And a Ctrl-C in the terminal, which
  // the kernel sends to the foreground group, no longer reaches the child directly — Motion Master
  // stops it deliberately on the way out instead, in the order it chooses.
  posix_spawnattr_t attr;
  if (posix_spawnattr_init(&attr) != 0) {
    posix_spawn_file_actions_destroy(&actions);
    return std::unexpected("cannot prepare the child's attributes");
  }
  const bool grouped = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP) == 0 &&
                       posix_spawnattr_setpgroup(&attr, 0) == 0;
  if (!grouped) {
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    return std::unexpected("cannot put the child in its own process group");
  }

  pid_t pid = 0;
  const int rc = posix_spawn(&pid, binaryString.c_str(), &actions, &attr, argv.data(), environ);
  posix_spawnattr_destroy(&attr);
  posix_spawn_file_actions_destroy(&actions);
  if (rc != 0) {
    // posix_spawn returns the error rather than setting errno, and a wrong path arrives here as
    // ENOENT — the common case by far, because the executable is downloaded by a separate script.
    return std::unexpected(mm::core::errnoMessage(rc));
  }
  return Child{pid, 0};
}

bool childAlive(const Child& child) {
  if (child.pid == 0) {
    return false;
  }
  return !reaped(child.pid);
}

bool terminateChild(const Child& child, std::chrono::milliseconds grace) {
  if (child.pid == 0) {
    return false;
  }
  const auto pid = static_cast<pid_t>(child.pid);
  // The child leads its own process group, so its group id is its process id, and a negative
  // argument signals the group. SIGTERM first: the auto-tuning launcher handles it and shuts its
  // HTTP server down.
  kill(-pid, SIGTERM);

  const auto deadline = std::chrono::steady_clock::now() + grace;
  while (std::chrono::steady_clock::now() < deadline) {
    if (reaped(child.pid)) {
      return false;
    }
    std::this_thread::sleep_for(kReapPollInterval);
  }

  // Still there, so it is wedged inside a numerical routine and will not act on a signal it has to
  // return to its main loop to see. SIGKILL the group, then reap. The group is what matters: the
  // program the launcher unpacked is the process holding the port, and it would otherwise survive
  // its parent and refuse the next Motion Master a bind.
  kill(-pid, SIGKILL);
  int status = 0;
  waitpid(pid, &status, 0);
  return true;
}

}  // namespace mm::auto_tuning::detail
