#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <vector>

#include "cyclic_task.h"

/// @brief Fixed-period real-time loop.
///
/// The caller drives the loop by blocking the calling thread in run().  The
/// main thread becomes the RT thread — no separate thread is created.  All
/// other subsystems should start their own threads before calling run(), and
/// stop them after run() returns.
///
/// Each cycle waits for the next absolute deadline via CyclicTimer, then
/// executes the loop body.  The period is fixed at construction.
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
  /// On Linux, raises the calling thread to SCHED_FIFO priority and locks all
  /// memory pages before entering the loop.  Both steps fail gracefully with a
  /// warning when the process lacks the required privileges (e.g. not run as
  /// root and no CAP_SYS_NICE / CAP_IPC_LOCK).
  ///
  /// @note Call this on the main thread and start all other subsystem threads
  ///       beforehand — it does not return until the loop stops.
  void run();

  /// @brief Signals the loop to stop after the current cycle completes.
  ///
  /// Safe to call from a signal handler.  run() returns within one period.
  void stop();

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
  /// meeting its period.  Intended to feed the planned master-side health
  /// timeline.  `CycleContext::elapsed` = `executedCycles() + skippedCycles()`.
  ///
  /// @return Skipped-cycle count.  Reads with relaxed ordering — suitable for
  ///         diagnostics, not for synchronisation.
  uint64_t skippedCycles() const;

 private:
  std::chrono::microseconds period_;
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> executedCycles_{0};
  std::atomic<uint64_t> skippedCycles_{0};
  std::vector<CyclicTask*> tasks_;
};
