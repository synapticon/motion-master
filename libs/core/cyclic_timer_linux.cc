#include <time.h>

#include "core/cyclic_timer.h"

namespace mm::core {

CyclicTimer::CyclicTimer(std::chrono::microseconds period) : period_ns_(period.count() * 1000) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  next_sec_ = now.tv_sec;
  next_nsec_ = now.tv_nsec;
}

CyclicTimer::~CyclicTimer() = default;

void CyclicTimer::waitForNextCycle() {
  next_nsec_ += period_ns_;
  if (next_nsec_ >= 1'000'000'000L) {
    next_nsec_ -= 1'000'000'000L;
    next_sec_++;
  }
  struct timespec target{next_sec_, next_nsec_};
  // TIMER_ABSTIME: sleep to a fixed absolute deadline so drift never accumulates.
  // Retry on EINTR so a signal (e.g. stop) doesn't cut a cycle short — the caller
  // checks its stop flag after waitForNextCycle() returns normally.
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &target, nullptr) == EINTR) {
  }
}

}  // namespace mm::core
