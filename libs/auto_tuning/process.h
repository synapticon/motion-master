#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

/// @file
/// @brief The auto-tuning executable, started and supervised as a child process.
///
/// Auto-tuning is a separate program. It holds the tuning calculations, and the fit that turns
/// recorded measurements into a plant model. It is compiled from another repository and serves an
/// HTTP API on loopback. Motion Master starts one instance at startup and talks to it for the rest
/// of the run.
///
/// It drives nothing. Every function computes on the numbers it is handed. The measurement a plant
/// model is fitted to is recorded on the drive, by the system identification procedure, and this
/// program only ever sees the samples that procedure produced.
///
/// It is a child process rather than a library because the algorithms are Python compiled to a
/// self-contained executable of about 65 MB. Linking that into the server is not an option, and
/// the process boundary also means a numerical routine that hangs cannot take the server with it.
///
/// This class owns the process. Nothing here makes a tuning call; @c Client does that.

namespace mm::auto_tuning {

/// @brief Port the child serves on when the configuration names none.
///
/// The auto-tuning program's own default, so a hand-started child and one started here answer on
/// the same port.
constexpr std::uint16_t kDefaultPort = 63528;

/// @brief Name of the executable Motion Master installs beside itself.
///
/// The install scripts download a file named for its platform and rename it to this, so one name
/// works everywhere. See @c install-auto-tuning.sh.
std::filesystem::path defaultBinaryName();

/// @brief What to start, and where.
struct ProcessOptions {
  /// The executable. An empty path is a configuration error, not a default: the caller resolves
  /// the default, because only the composition root knows where Motion Master itself lives.
  std::filesystem::path binary;
  std::uint16_t port = kDefaultPort;
  /// The child's own stdout and stderr are appended here. It prints a startup line and one line
  /// per request, which would otherwise land on Motion Master's console without passing through
  /// its logger — visible, but absent from both @c GET @c /api/log and the log file. An empty path
  /// lets the child inherit our streams instead.
  std::filesystem::path logFile;
  /// How long @c start waits for the child to answer @c GET @c /api/health.
  ///
  /// Measured on a Linux development machine, a healthy child answers 1.0 s after the spawn: the
  /// executable unpacks itself on every start, and that unpack is the whole cost. The default is
  /// far above that because a machine that never unpacked this executable is slower, and
  /// because the wait ends the moment the child answers or dies. So a generous limit costs nothing
  /// in either normal case, and only a child that is alive and silent waits it out.
  std::chrono::milliseconds startTimeout{30000};
};

/// @brief Starts the auto-tuning executable and keeps it running for the process lifetime.
///
/// @c start and @c stop belong to the composition root and run on one thread. They are the only
/// members that touch the child. Everything else is read-only after a successful @c start, and safe
/// to read from any thread, which is what the HTTP routes do.
///
/// There is deliberately no "is it running" query. Answering it means reaping the child, and a
/// reaped process id can be reissued to something else — so a query that reaped would let a later
/// @c stop signal an unrelated process. The child's health is discovered by calling it: a request
/// to a dead child fails, and that is the honest answer rather than a flag that was true a moment
/// ago.
class Process {
 public:
  explicit Process(ProcessOptions options);

  /// @brief Stops the child. A Motion Master that exits leaves nothing running behind it.
  ~Process();

  Process(const Process&) = delete;
  Process& operator=(const Process&) = delete;
  Process(Process&&) = delete;
  Process& operator=(Process&&) = delete;

  /// @brief Spawns the child and waits for it to serve.
  ///
  /// Runs the executable as `<binary> --http --host 127.0.0.1 --port <port>`, then polls
  /// @c GET @c /api/health until it answers or @c startTimeout expires. The reply carries the
  /// program's version, which @c version then reports.
  ///
  /// **Call this before the calling thread becomes real-time.** A child inherits the scheduling
  /// policy of the thread that spawned it, and this child spreads its work over one numerical
  /// worker thread per core, which synchronise by spin-waiting. At real-time priority each of those
  /// burns a whole timeslice waiting for the others: measured on the previous integration, a
  /// seven-second identification took minutes and starved the 1 ms cycle while it ran.
  ///
  /// @return Nothing on success, or an error naming what failed: a missing executable, a spawn
  ///         failure, a child that exited during the wait, or a timeout.
  std::expected<void, std::string> start();

  /// @brief How a @c stop ended, so that the caller can log it.
  ///
  /// On Windows only @c NotRunning, @c Requested and @c Killed occur: there is no signal that
  /// program handles there, so the middle step does not exist.
  enum class StopOutcome {
    NotRunning,  ///< Nothing was running.
    Requested,   ///< It exited on the exit request, which is the ordinary case.
    Signalled,   ///< It ignored the request and exited on the termination signal.
    Killed,      ///< It ignored both and was killed.
  };

  /// @brief Asks the child to exit, then kills it if it does not.
  ///
  /// Three steps, politest first: a @c {"run":"exit"} request, which the auto-tuning program
  /// answers by shutting its server down; then a termination signal; then a kill of the whole
  /// process group. The request matters most on Windows, which has no signal the program handles,
  /// and where its own output is block-buffered — so a hard kill there would lose the tail of its
  /// log.
  ///
  /// Safe to call when nothing was started, and safe to call twice.
  ///
  /// @return Which step ended it. @c Killed is the one worth a warning: a child wedged inside a
  ///         numerical routine had to be taken down, and that is the case where something could be
  ///         left holding the port.
  StopOutcome stop();

  /// @brief The version the child reported at startup, or empty when it never started.
  const std::string& version() const { return version_; }

  /// @brief The root URL of the child's API, for a client to build request URLs from.
  std::string baseUrl() const;

  /// @brief The options this instance was constructed with.
  const ProcessOptions& options() const { return options_; }

  /// @brief The child's process id, or 0 when nothing is running. For the startup log line.
  std::int64_t pid() const { return pid_; }

 private:
  /// Drops every trace of a child that is gone. A process id may be reissued once it is reaped, so
  /// nothing must be kept that a later @c stop could signal.
  void forget();

  ProcessOptions options_;
  /// The process id on POSIX. On Windows the id is also kept as a handle, in @c handle_.
  std::int64_t pid_ = 0;
  /// Windows only: the process handle, stored as an integer so this header names no Windows type.
  std::intptr_t handle_ = 0;
  /// Windows only: the job object that holds the child and whatever it starts. On POSIX the process
  /// group is the process id, so nothing extra is kept.
  std::intptr_t group_ = 0;
  std::string version_;
};

}  // namespace mm::auto_tuning
