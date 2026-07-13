#pragma once

#include <chrono>
#include <cstdint>

namespace mm::core {

/// @brief Precision cyclic timer for fixed-period loops.
///
/// Sleeps to an absolute deadline each cycle so that scheduling jitter from
/// one cycle does not accumulate into drift over time.  The sequence of
/// deadlines is a fixed grid anchored at construction — each call to
/// waitForNextCycle() advances the target by one period regardless of actual
/// wake-up time.
///
/// Ordinary jitter (a late wake-up whose cycle work still fits the budget)
/// leaves the next deadline in the future and is absorbed drift-free in that
/// cycle's remaining slack.  A genuine overrun or multi-cycle stall leaves the
/// next deadline already in the past; rather than fire the missed cycles
/// back-to-back (a burst that, on an EtherCAT bus, would spam stale process
/// data), the timer skips the backlog and re-syncs to the next FUTURE grid
/// point, preserving the original phase.  waitForNextCycle() returns how many
/// cycles were skipped so the caller can count overruns.
///
/// Platform implementations:
/// - Linux: `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)`.  Works on
///   standard kernels; becomes hard real-time when the calling thread runs at
///   `SCHED_FIFO` priority on a `CONFIG_PREEMPT_RT` kernel.
/// - Windows: `CreateWaitableTimerEx` with
///   `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` (requires Windows 10 1803+).
/// - macOS: `mach_wait_until()` against an absolute `mach_absolute_time()`
///   deadline (Darwin has no `clock_nanosleep`).
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
  /// If the newly-advanced deadline is already in the past — an overrun or a
  /// multi-cycle scheduling stall — the timer does not run the missed cycles
  /// back-to-back.  It fast-forwards over the backlog to the next grid point
  /// still in the future and sleeps to that, so at most one cycle runs per
  /// period and the deadline phase is preserved.
  ///
  /// Signals that interrupt the sleep (EINTR) are retried transparently: the
  /// sleep resumes toward the same absolute deadline, so signal delivery
  /// neither shortens a cycle nor causes drift.  Callers should check their
  /// own stop condition after this function returns.
  ///
  /// @return Number of cycles skipped to catch up to the grid.  0 on the
  ///         normal path (deadline was met or the wake was merely late);
  ///         positive only after an overrun/stall.  Not `[[nodiscard]]` —
  ///         callers that don't track overruns may ignore it.
  uint64_t waitForNextCycle();

 private:
#ifdef _WIN32
  void* handle_ = nullptr;   // HANDLE — avoids pulling <windows.h> into every consumer
  int64_t qpcFreq_ = 0;      // QueryPerformanceCounter ticks per second
  int64_t periodTicks_ = 0;  // period expressed in QPC ticks
  int64_t next_ = 0;         // absolute deadline on the QPC (monotonic) timeline, in ticks
#else
  /// @brief Advances the deadline (next_sec_/next_nsec_) by exactly one period,
  ///        normalizing the nanosecond carry.  Shared by the Linux and macOS
  ///        implementations.  A single `if` suffices because period_ns_ < 1s
  ///        (this timer targets sub-second cycles), so at most one whole second
  ///        is ever carried per call.  Defined inline here so both platform
  ///        translation units share one definition.
  void advanceOnePeriod() {
    next_nsec_ += period_ns_;
    if (next_nsec_ >= 1'000'000'000L) {
      next_nsec_ -= 1'000'000'000L;
      next_sec_++;
    }
  }

  int64_t period_ns_ = 0;
  int64_t next_sec_ = 0;
  int64_t next_nsec_ = 0;
#endif
};

}  // namespace mm::core
