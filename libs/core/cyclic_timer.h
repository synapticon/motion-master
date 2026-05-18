#pragma once

#include <chrono>

namespace mm::core {

/// @brief Precision cyclic timer for fixed-period loops.
///
/// Sleeps to an absolute deadline each cycle so that scheduling jitter from
/// one cycle does not accumulate into drift over time.  The sequence of
/// deadlines is fixed at construction — each call to waitForNextCycle()
/// advances the target by exactly one period regardless of actual wake-up
/// time.
///
/// Platform implementations:
/// - Linux: `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)`.  Works on
///   standard kernels; becomes hard real-time when the calling thread runs at
///   `SCHED_FIFO` priority on a `CONFIG_PREEMPT_RT` kernel.
/// - Windows: `CreateWaitableTimerEx` with
///   `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` (requires Windows 10 1803+).
///
/// Not copyable — each instance owns its own deadline state.
class CyclicTimer {
 public:
  /// @brief Constructs the timer and records the current time as the first
  ///        deadline baseline.
  /// @param period  Cycle period.  Typical value: 1000 µs (1 ms).
  explicit CyclicTimer(std::chrono::microseconds period);

  /// @brief Destroys the timer.  On Windows, cancels and closes the OS timer
  ///        handle.  On Linux, no OS resource is held.
  ~CyclicTimer();

  /// @brief Copying is deleted — each instance owns its own deadline state and,
  ///        on Windows, an OS timer handle that cannot be shared.
  CyclicTimer(const CyclicTimer&) = delete;
  /// @brief Copy assignment is deleted — see copy constructor.
  CyclicTimer& operator=(const CyclicTimer&) = delete;

  /// @brief Blocks until the next cycle deadline, then returns.
  ///
  /// Advances the internal deadline by one period and sleeps until that
  /// absolute point in time.  Because the target is absolute rather than
  /// relative, late wake-ups in one cycle do not shift the deadline of the
  /// next.
  ///
  /// Signals that interrupt the sleep (EINTR) are retried transparently: the
  /// sleep resumes toward the same absolute deadline, so signal delivery
  /// neither shortens a cycle nor causes drift.  Callers should check their
  /// own stop condition after this function returns.
  void waitForNextCycle();

 private:
#ifdef _WIN32
  void* handle_ = nullptr;  // HANDLE — avoids pulling <windows.h> into every consumer
#else
  int64_t period_ns_ = 0;
  int64_t next_sec_ = 0;
  int64_t next_nsec_ = 0;
#endif
};

}  // namespace mm::core
