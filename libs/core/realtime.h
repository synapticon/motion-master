#pragma once

namespace mm::core {

/// @brief Outcome of setRealtimePriority() — which of the two best-effort steps took.
struct RtSetupResult {
  bool schedFifo = false;  ///< SCHED_FIFO acquired (false on Windows / on failure).
  bool memLocked = false;  ///< mlockall succeeded (Linux only; false elsewhere).
};

/// @brief Priority requested for the real-time cyclic thread.
///
/// Well clear of the 90+ band the kernel's own migration/watchdog threads
/// occupy, and above the ~50 that `CONFIG_PREEMPT_RT` gives threaded IRQs.
/// That last relation is the one to verify per target before relying on hard
/// synchronisation: on a stock kernel softirqs run in interrupt context and
/// preempt SCHED_FIFO at any priority, so the number cannot starve the NIC,
/// but under PREEMPT_RT this thread does outrank the interface's IRQ thread.
/// Confirm against the target with
/// `ps -eo pid,cls,rtprio,comm | grep irq` rather than assuming.
inline constexpr int kRtThreadPriority = 80;

/// @brief Prepares the **calling** thread for real-time execution.
///
/// Raises it to `SCHED_FIFO` at kRtThreadPriority so the cycle is never
/// preempted by ordinary `SCHED_OTHER` work, then locks all current and future
/// pages (`mlockall`) so a mid-cycle page fault cannot inject an unbounded
/// latency spike.
///
/// Both steps are **best effort and independent**: neither failure is fatal and
/// a failed `SCHED_FIFO` does not skip the `mlockall`, so a process lacking
/// `CAP_SYS_NICE` / `CAP_IPC_LOCK` still runs — just non-deterministically.
/// Nothing is logged here (this layer has no logger); the caller reports
/// whichever step the returned result says was missed.
///
/// Scheduling policy applies to the calling thread alone, so only the thread
/// that calls this becomes real-time.  The policy is requested with
/// `SCHED_RESET_ON_FORK` where the platform defines it, so any child this
/// thread forks starts at `SCHED_OTHER`/nice 0 with the flag cleared — the
/// guarantee then holds by construction rather than by no one happening to
/// fork from here.  (`mlockall(MCL_FUTURE)` is process-wide and survives fork
/// regardless; an `exec` clears it.)
///
/// Windows has no equivalent to either step: the call is a no-op returning an
/// all-false result.
RtSetupResult setRealtimePriority();

}  // namespace mm::core
