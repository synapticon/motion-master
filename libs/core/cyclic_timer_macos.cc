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

uint64_t CyclicTimer::waitForNextCycle() {
  advanceOnePeriod();

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);

  // Skip-to-grid: if the deadline is already behind us — an overrun or a
  // multi-cycle stall — fast-forward to the next future grid point instead of
  // firing the missed cycles back-to-back. Strict `<` so an exactly-due
  // deadline is met, not skipped. The original grid phase is preserved.
  uint64_t skipped = 0;
  while (next_sec_ < now.tv_sec || (next_sec_ == now.tv_sec && next_nsec_ < now.tv_nsec)) {
    advanceOnePeriod();
    ++skipped;
  }

  static const mach_timebase_info_data_t tb = machTimebase();
  const uint64_t deadline_ns =
      static_cast<uint64_t>(next_sec_) * 1'000'000'000ULL + static_cast<uint64_t>(next_nsec_);
  // ticks = ns * denom / numer
  const uint64_t deadline_ticks = deadline_ns * tb.denom / tb.numer;

  // Absolute deadline: late wake-ups in one cycle don't shift the next, so
  // drift never accumulates. Retry on KERN_ABORTED so a signal doesn't cut a
  // cycle short — the sleep resumes toward the same absolute deadline,
  // mirroring the Linux EINTR-retry contract.
  while (mach_wait_until(deadline_ticks) == KERN_ABORTED) {
  }
  return skipped;
}

}  // namespace mm::core
