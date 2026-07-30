#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <vector>

#include "core/cyclic_task.h"

/// @brief Snapshot of game-loop real-time health for the GET /api/game-loop
///        diagnostic endpoint. All fields are read with relaxed ordering — for
///        monitoring, not synchronisation.
struct GameLoopHealth {
  uint64_t periodUs = 0;        ///< Configured target period (µs).
  double targetHz = 0.0;        ///< Target rate = 1e6 / periodUs.
  double achievedHz = 0.0;      ///< Cumulative avg = executedCycles / uptime; 0 pre-run.
  uint64_t executedCycles = 0;  ///< Loop iterations run since run().
  uint64_t skippedCycles = 0;   ///< Cycles skipped to catch up since run().
  uint64_t lastExecNs = 0;      ///< Task-exec time of the most recent cycle (ns).
  uint64_t maxExecNs = 0;       ///< Worst task-exec time since run() (ns).
  uint64_t avgExecNs = 0;       ///< Mean task-exec time since run() (ns).
  bool schedFifo = false;       ///< SCHED_FIFO acquired (false on Windows / failure).
  bool memLocked = false;       ///< mlockall ok (Linux only; false elsewhere).
  uint64_t timestampUs = 0;     ///< Server stamp (epoch µs) for exact client Δt.
};

/// @brief Serializes a GameLoopHealth to JSON (found via ADL by nlohmann::json).
void to_json(nlohmann::json& j, const GameLoopHealth& h);

/// @brief Fixed-period real-time loop.
///
/// The caller drives the loop by blocking the calling thread in run().  The
/// main thread becomes the RT thread — no separate thread is created.  All
/// other subsystems should start their own threads before calling run(), and
/// stop them after run() returns.
///
/// Each cycle waits for the next absolute deadline via CyclicTimer, then
/// executes the loop body.  The period is set at construction and may be
/// retimed while running via setPeriod() (see below).
///
/// Shutdown is cooperative: stop() sets an atomic flag that run() checks after
/// each cycle completes.  The loop exits cleanly within one period.
class GameLoop {
 public:
  /// @brief Constructs the loop with the given cycle period.
  /// @param period  Time between cycle starts.  Typical value: 1000 µs (1 ms).
  explicit GameLoop(std::chrono::microseconds period);

  /// @brief Destructor.  Does not call stop() — the caller is responsible for
  ///        stopping the loop before destroying it.
  ~GameLoop() = default;

  /// @brief Copying is deleted — the loop owns real-time scheduling state that
  ///        cannot be shared.
  GameLoop(const GameLoop&) = delete;
  /// @brief Copy assignment is deleted — see copy constructor.
  GameLoop& operator=(const GameLoop&) = delete;

  /// @brief Registers a task to be executed every cycle.
  ///
  /// Tasks are called in registration order after each timer tick.  Must be
  /// called before run() — not safe to call concurrently with a running loop.
  ///
  /// @param task  Non-owning pointer.  The task must outlive every call to
  ///              run() — its pointer is only dereferenced while the loop is
  ///              executing, never after run() returns.
  void addTask(CyclicTask* task);

  /// @brief Blocks the calling thread, running one cycle per period until
  ///        stop() is called.
  ///
  /// Calls mm::core::setRealtimePriority() before entering the loop, which
  /// raises the calling thread to SCHED_FIFO (POSIX) and locks all memory pages
  /// (Linux).  Both steps fail gracefully with a warning when the process lacks
  /// the required privileges (e.g. not run as root and no CAP_SYS_NICE /
  /// CAP_IPC_LOCK); health() reports which of them took.
  ///
  /// @note Call this on the main thread and start all other subsystem threads
  ///       beforehand — it does not return until the loop stops.
  void run();

  /// @brief Signals the loop to stop after the current cycle completes.
  ///
  /// Safe to call from a signal handler.  run() returns within one period.
  void stop();

  /// @brief Changes the cycle period while the loop is running.
  ///
  /// Stores the new period; the running loop picks it up on its next iteration
  /// and re-anchors the CyclicTimer's deadline grid to that instant (see
  /// CyclicTimer::setPeriod).  Takes effect within one cycle.  Safe to call from
  /// any thread — the store is a relaxed atomic and the RT loop is the only
  /// reader.  Also updates the period reported by health().
  ///
  /// Applying a change also starts a fresh health epoch: the loop resets its
  /// cumulative counters (executed/skipped cycles, task-time max/avg) and
  /// re-anchors the achievedHz baseline to that instant, so health() reflects
  /// only the new period.  Without this the old, worse figures would linger and
  /// mask the improvement the change was meant to produce.
  ///
  /// Changing the period only re-times the master cadence; it does not touch the
  /// process-data recorder ring or any drive-side watchdog.  Raising the period
  /// toward a drive's PDO/SM watchdog window can fault that drive — the caller is
  /// responsible for choosing a sane value.
  ///
  /// @param period  New cycle period.  Must be > 0 (validated by the caller).
  void setPeriod(std::chrono::microseconds period);

  /// @brief Returns the number of cycles actually executed since run() was
  ///        called.
  ///
  /// This counts loop iterations (calls to a task's execute()), which is
  /// distinct from grid periods elapsed: a stall that skips N cycles advances
  /// this by 1 (one iteration ran) while wall-clock time advanced by 1 + N
  /// periods.  `CycleContext::elapsed` = `executedCycles() + skippedCycles()`.
  ///
  /// @return Executed-cycle count.  Reads with relaxed ordering — suitable for
  ///         diagnostics and logging, not for synchronisation.
  uint64_t executedCycles() const;

  /// @brief Returns the total number of cycles skipped since run() was called.
  ///
  /// When a deadline is already past (an overrun or a scheduling stall — the
  /// latter routine on a non-RT OS), the timer skips the backlog and re-syncs
  /// to the grid rather than running missed cycles back-to-back (which would
  /// flood the bus with stale process data).  This is the running total of
  /// cycles skipped that way — a nonzero, growing value means the loop is not
  /// meeting its period.  Read via health() (GET /api/game-loop, the console
  /// Game Loop page).  `CycleContext::elapsed` = `executedCycles() + skippedCycles()`.
  ///
  /// @return Skipped-cycle count.  Reads with relaxed ordering — suitable for
  ///         diagnostics, not for synchronisation.
  uint64_t skippedCycles() const;

  /// @brief Returns a snapshot of real-time loop health for diagnostics.
  ///
  /// Reads the counters, task-execution aggregates, and RT-scheduling flags with
  /// relaxed ordering, and computes the cumulative achievedHz from uptime. Safe
  /// to call from any thread; the dynamic fields are zero before run() starts.
  GameLoopHealth health() const;

 private:
  // Cycle period; read by the RT loop each iteration, written by setPeriod from another thread.
  // std::chrono::microseconds is an 8-byte trivially-copyable type, so this atomic is lock-free.
  std::atomic<std::chrono::microseconds> period_;
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> executedCycles_{0};
  std::atomic<uint64_t> skippedCycles_{0};
  std::atomic<uint64_t> lastExecNs_{0};   // most recent cycle's task-execution time
  std::atomic<uint64_t> maxExecNs_{0};    // worst task-execution time since run()
  std::atomic<uint64_t> sumExecNs_{0};    // running sum → avg = sum / executedCycles
  std::atomic<uint64_t> startMonoNs_{0};  // steady_clock ns at run() start; 0 until then
  std::atomic<bool> schedFifo_{false};    // SCHED_FIFO acquired in run()
  std::atomic<bool> memLocked_{false};    // mlockall succeeded in run()
  std::vector<CyclicTask*> tasks_;
};
