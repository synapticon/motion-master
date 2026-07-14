#include <mach/mach_time.h>
#include <time.h>

#include <cstdint>

#include "core/cyclic_timer.h"

namespace mm::core {

namespace {

// Cache the mach timebase (numer/denom converts mach ticks <-> nanoseconds).
// On Apple Silicon this is typically 125/3 (a 24 MHz counter), NOT 1/1, so the
// conversion is mandatory — never assume ticks equal nanoseconds.
mach_timebase_info_data_t machTimebase() {
  mach_timebase_info_data_t tb;
  mach_timebase_info(&tb);
  return tb;
}

const mach_timebase_info_data_t kTimebase = machTimebase();

// "Now" on the exact timeline mach_wait_until() sleeps against.
// CLOCK_UPTIME_RAW is mach_absolute_time() pre-converted to nanoseconds, so it
// shares mach_wait_until()'s epoch and timebase. We must NOT read the deadline
// baseline from clock_gettime(CLOCK_MONOTONIC): on Apple Silicon that clock does
// not share a timebase with mach_absolute_time(), so a deadline derived from it
// would be compared against a different clock and land in the past, making every
// wait return immediately.
uint64_t nowNs() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

}  // namespace

// macOS has no clock_nanosleep, so the absolute-deadline sleep is built on
// mach_wait_until(), which takes a deadline in mach_absolute_time() units. We
// keep the deadline in nanoseconds (matching the Linux impl's member layout, on
// the CLOCK_UPTIME_RAW / mach_absolute_time timeline) and convert to mach ticks
// per cycle.
CyclicTimer::CyclicTimer(std::chrono::microseconds period) : period_ns_(period.count() * 1000) {
  const uint64_t now = nowNs();
  next_sec_ = static_cast<int64_t>(now / 1'000'000'000ULL);
  next_nsec_ = static_cast<int64_t>(now % 1'000'000'000ULL);
}

CyclicTimer::~CyclicTimer() = default;

void CyclicTimer::setPeriod(std::chrono::microseconds period) {
  period_ns_ = period.count() * 1000;
  const uint64_t now = nowNs();
  next_sec_ = static_cast<int64_t>(now / 1'000'000'000ULL);
  next_nsec_ = static_cast<int64_t>(now % 1'000'000'000ULL);
}

uint64_t CyclicTimer::waitForNextCycle() {
  advanceOnePeriod();

  const uint64_t now = nowNs();
  const int64_t now_sec = static_cast<int64_t>(now / 1'000'000'000ULL);
  const int64_t now_nsec = static_cast<int64_t>(now % 1'000'000'000ULL);

  // Skip-to-grid: if the deadline is already behind us — an overrun or a
  // multi-cycle stall — fast-forward to the next future grid point instead of
  // firing the missed cycles back-to-back. Strict `<` so an exactly-due
  // deadline is met, not skipped. The original grid phase is preserved.
  uint64_t skipped = 0;
  while (next_sec_ < now_sec || (next_sec_ == now_sec && next_nsec_ < now_nsec)) {
    advanceOnePeriod();
    ++skipped;
  }

  const uint64_t deadline_ns =
      static_cast<uint64_t>(next_sec_) * 1'000'000'000ULL + static_cast<uint64_t>(next_nsec_);
  // ticks = ns * denom / numer
  const uint64_t deadline_ticks = deadline_ns * kTimebase.denom / kTimebase.numer;

  // Absolute deadline: late wake-ups in one cycle don't shift the next, so
  // drift never accumulates. Retry on KERN_ABORTED so a signal doesn't cut a
  // cycle short — the sleep resumes toward the same absolute deadline,
  // mirroring the Linux EINTR-retry contract.
  while (mach_wait_until(deadline_ticks) == KERN_ABORTED) {
  }
  return skipped;
}

}  // namespace mm::core
