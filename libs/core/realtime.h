#pragma once

namespace mm::core {

/// @brief Outcome of setRealtimePriority() — which of the three best-effort steps took.
struct RtSetupResult {
  bool schedFifo = false;  ///< SCHED_FIFO acquired (false on Windows / on failure).
  bool memLocked = false;  ///< mlockall succeeded (Linux only; false elsewhere).
  bool cpuPinned = false;  ///< Affinity applied (Linux only; false if unrequested or failed).
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
/// Optionally also pins it to a single core (`cpu >= 0`).  This matters
/// wherever the kernel booted with `isolcpus`: that removes a core from the
/// scheduler's reach, so a thread runs there only if it asks to, and an
/// isolated core otherwise sits idle.  Affinity is per **thread**, which is the
/// whole point — pinning the entire process instead (`taskset`, systemd's
/// `CPUAffinity=`) drags every non-real-time thread onto the isolated core to
/// contend with this one, and leaves more than one runnable task there, which
/// is exactly what stops `nohz_full` from taking effect.
///
/// All steps are **best effort and independent**: no failure is fatal, and a
/// failed `SCHED_FIFO` skips neither the `mlockall` nor the pinning, so a
/// process lacking `CAP_SYS_NICE` / `CAP_IPC_LOCK` still runs — just
/// non-deterministically.  Nothing is logged here (this layer has no logger);
/// the caller reports whichever step the returned result says was missed.
///
/// Unlike the other two, the pinning needs **no privilege at all**: a thread may
/// always set its own affinity, and `CAP_SYS_NICE` is required only to change
/// another process's.  What does defeat it is a mask restricted from outside —
/// a cgroup cpuset, a container, systemd's `CPUAffinity=`, or an inherited
/// `taskset` — which makes a core outside that mask fail with `EINVAL` no matter
/// what capabilities the process holds.
///
/// Scheduling policy applies to the calling thread alone, so only the thread
/// that calls this becomes real-time.  The policy is requested with
/// `SCHED_RESET_ON_FORK` where the platform defines it, so any child this
/// thread forks starts at `SCHED_OTHER`/nice 0 with the flag cleared — the
/// guarantee then holds by construction rather than by no one happening to
/// fork from here.  (`mlockall(MCL_FUTURE)` is process-wide and survives fork
/// regardless; an `exec` clears it.)
///
/// Windows has no equivalent to any of the steps: the call is a no-op returning
/// an all-false result.
///
/// @param cpuAffinity  Core to pin the calling thread to, or a negative value (the
///             default) to leave affinity untouched.  Ignored off Linux —
///             macOS exposes no per-thread affinity API.  An out-of-range core
///             simply fails the step and reports `cpuPinned == false`.
RtSetupResult setRealtimePriority(int cpuAffinity = -1);

}  // namespace mm::core
