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

uint64_t CyclicTimer::waitForNextCycle() {
  advanceOnePeriod();

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);

  // Skip-to-grid: if the deadline is already behind us — an overrun or a
  // multi-cycle stall — fast-forward to the next future grid point instead of
  // firing the missed cycles back-to-back (which would spam stale process data
  // onto the bus). Strict `<` so an exactly-due deadline is met, not skipped.
  // The original grid phase is preserved; only the backlog is dropped.
  uint64_t skipped = 0;
  while (next_sec_ < now.tv_sec || (next_sec_ == now.tv_sec && next_nsec_ < now.tv_nsec)) {
    advanceOnePeriod();
    ++skipped;
  }

  struct timespec target = {next_sec_, next_nsec_};
  // TIMER_ABSTIME: sleep to a fixed absolute deadline so drift never accumulates.
  // Retry on EINTR so a signal (e.g. stop) doesn't cut a cycle short — the caller
  // checks its stop flag after waitForNextCycle() returns normally.
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &target, nullptr) == EINTR) {
  }
  return skipped;
}

}  // namespace mm::core
