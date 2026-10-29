#include "core/realtime.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif
#ifdef __linux__
#include <sys/mman.h>
#endif

namespace mm::core {

#ifdef _WIN32
namespace {

// Older Windows SDKs lack the flag that Windows 11 added.
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif

// Turns one kind of power throttling off for the whole process: the flag in ControlMask and not
// in StateMask.
bool turnOffPowerThrottling(ULONG flag) {
  PROCESS_POWER_THROTTLING_STATE state{};
  state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  state.ControlMask = flag;
  state.StateMask = 0;
  return SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state,
                               sizeof(state)) != 0;
}

}  // namespace
#endif

RtSetupResult setRealtimePriority(int cpuAffinity) {
  RtSetupResult result;
#ifndef _WIN32
  // SCHED_RESET_ON_FORK is a Linux extension; Darwin's <sched.h> does not
  // define it, so the flag is folded in only where it exists.
  int policy = SCHED_FIFO;
#ifdef SCHED_RESET_ON_FORK
  policy |= SCHED_RESET_ON_FORK;
#endif
  struct sched_param param = {};
  param.sched_priority = kRtThreadPriority;
  if (pthread_setschedparam(pthread_self(), policy, &param) == 0) {
    result.schedFifo = true;
  }
#endif
#ifdef __linux__
  // Prevents the kernel from faulting in pages mid-cycle, which would
  // introduce unbounded latency spikes.  macOS has no mlockall().
  if (mlockall(MCL_CURRENT | MCL_FUTURE) == 0) {
    result.memLocked = true;
  }

  // Only this thread moves.  A core removed from the scheduler by isolcpus runs
  // nothing unless a thread asks for it by name, so without this the isolated
  // core stays idle and the isolation buys nothing.  Fails harmlessly (EINVAL)
  // if the core does not exist.
  if (cpuAffinity >= 0) {
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(static_cast<unsigned int>(cpuAffinity), &cpus);
    if (sched_setaffinity(0, sizeof(cpus), &cpus) == 0) {
      result.cpuPinned = true;
    }
  }
#else
  (void)cpuAffinity;  // no per-thread affinity API on macOS, and no RT path on Windows
#endif
#ifdef _WIN32
  result.highQos = turnOffPowerThrottling(PROCESS_POWER_THROTTLING_EXECUTION_SPEED);
  result.timerResolutionHonoured =
      turnOffPowerThrottling(PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION);
#endif
  return result;
}

}  // namespace mm::core
