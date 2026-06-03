#include <mach/mach_time.h>
#include <time.h>

#include "core/cyclic_timer.h"

namespace mm::core {

namespace {

// Cache the mach timebase (numer/denom converts mach ticks <-> nanoseconds).
// On Apple Silicon this is 1/1, but never assume it.
mach_timebase_info_data_t machTimebase() {
  mach_timebase_info_data_t tb;
  mach_timebase_info(&tb);
  return tb;
}

}  // namespace

// macOS has no clock_nanosleep, so the absolute-deadline sleep is built on
// mach_wait_until(), which takes a deadline in mach_absolute_time() units. We
// keep the deadline in CLOCK_MONOTONIC nanoseconds (matching the Linux impl's
// member layout) and convert to mach ticks per cycle. CLOCK_MONOTONIC and
// mach_absolute_time share the same epoch on macOS, so the conversion is exact.
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

  static const mach_timebase_info_data_t tb = machTimebase();
  const uint64_t deadline_ns =
      static_cast<uint64_t>(next_sec_) * 1'000'000'000ULL + static_cast<uint64_t>(next_nsec_);
  // ticks = ns * denom / numer
  const uint64_t deadline_ticks = deadline_ns * tb.denom / tb.numer;

  // Absolute deadline: late wake-ups in one cycle don't shift the next, so
  // drift never accumulates. mach_wait_until resumes toward the same deadline
  // if interrupted, mirroring the Linux EINTR-retry contract.
  mach_wait_until(deadline_ticks);
}

}  // namespace mm::core
