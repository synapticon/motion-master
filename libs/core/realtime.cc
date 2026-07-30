#include "core/realtime.h"

#ifndef _WIN32
#include <pthread.h>
#include <sched.h>
#endif
#ifdef __linux__
#include <sys/mman.h>
#endif

namespace mm::core {

RtSetupResult setRealtimePriority() {
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
#endif
  return result;
}

}  // namespace mm::core
