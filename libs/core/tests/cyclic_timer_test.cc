#include "core/cyclic_timer.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

// CyclicTimer is a timing primitive, so these tests assert coarse, robust
// properties rather than exact wake-up instants — they must pass on a loaded,
// non-RT CI runner. The precise-jitter characterisation lives in
// hil/jitter_bench, not here.

namespace {

using std::chrono::steady_clock;

// 2 ms — comfortably above ordinary scheduling jitter.
constexpr auto kPeriod = std::chrono::microseconds(2000);

int64_t elapsedUs(steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::microseconds>(steady_clock::now() - start).count();
}

}  // namespace

// The timer must never let a deadline pass early: N cycles cannot complete in
// less than N periods. This is the anti-drift guarantee — the whole reason the
// sleep targets an absolute grid rather than a relative duration.
TEST(CyclicTimerTest, DoesNotFinishEarly) {
  constexpr int kCycles = 20;
  mm::core::CyclicTimer timer(kPeriod);
  const auto start = steady_clock::now();
  for (int i = 0; i < kCycles; ++i) {
    timer.waitForNextCycle();
  }
  // 0.9× tolerates timer granularity undershoot (e.g. Windows ~0.5 ms); the
  // point is that we get nowhere near instantaneous completion.
  EXPECT_GE(elapsedUs(start), kCycles * kPeriod.count() * 9 / 10);
}

// After a multi-period stall, a single waitForNextCycle() must absorb the whole
// backlog and report it — skip-to-grid — instead of returning 0 and forcing the
// caller to run the missed cycles back-to-back.
TEST(CyclicTimerTest, SkipsBacklogAfterStall) {
  mm::core::CyclicTimer timer(kPeriod);
  timer.waitForNextCycle();  // align to the grid

  // Stall ~5 periods inside the "cycle body". The next deadline (grid+1) and
  // several after it are now in the past.
  std::this_thread::sleep_for(5 * kPeriod);

  const uint64_t skipped = timer.waitForNextCycle();

  // The contract under test is skip-to-grid: one waitForNextCycle() absorbs the
  // whole backlog and reports it, rather than returning 0 and forcing the caller
  // to replay the missed cycles. So we only assert a lower bound — that it did
  // skip several grid points. We deliberately do NOT bound it from above: a
  // loaded CI runner can overshoot both the sleep and the alignment wait wildly
  // (a macOS runner has turned this 10 ms stall into ~90 ms → 47 skips), and the
  // skip count is measured from the timer's internal deadline, so no wall-clock
  // figure sampled here is a reliable ceiling. That the timer re-syncs afterward
  // instead of staying perpetually behind is covered by ResyncsAfterStall.
  EXPECT_GE(skipped, 3u);
}

// Once the backlog is cleared, the timer must re-sync to the grid and sleep
// normally again — it must not stay perpetually behind, returning instantly
// every cycle. Post-stall cycles with no injected delay should again consume
// roughly one period each.
TEST(CyclicTimerTest, ResyncsAfterStall) {
  mm::core::CyclicTimer timer(kPeriod);
  timer.waitForNextCycle();

  std::this_thread::sleep_for(5 * kPeriod);
  timer.waitForNextCycle();  // absorbs the backlog, re-syncs to a future grid point

  // No per-cycle skipped==0 assertion here: on a loaded non-RT runner an
  // undelayed 2 ms cycle can still legitimately miss its deadline. The elapsed
  // lower bound is the robust proof of re-sync — had the timer stayed behind
  // and returned instantly each cycle, elapsed would be near zero.
  constexpr int kPostCycles = 5;
  const auto start = steady_clock::now();
  for (int i = 0; i < kPostCycles; ++i) {
    timer.waitForNextCycle();
  }
  EXPECT_GE(elapsedUs(start), kPostCycles * kPeriod.count() * 9 / 10);
}

// setPeriod must retime subsequent cycles: after switching to a longer period,
// N cycles take at least N of the *new* period. Proves the new value actually
// governs the cadence, not the constructed one.
TEST(CyclicTimerTest, SetPeriodChangesCadence) {
  constexpr int kCycles = 10;
  constexpr auto kLongPeriod = std::chrono::microseconds(4000);  // 2× kPeriod
  mm::core::CyclicTimer timer(kPeriod);
  timer.waitForNextCycle();  // align to the initial grid

  timer.setPeriod(kLongPeriod);
  const auto start = steady_clock::now();
  for (int i = 0; i < kCycles; ++i) {
    timer.waitForNextCycle();
  }
  EXPECT_GE(elapsedUs(start), kCycles * kLongPeriod.count() * 9 / 10);
}

// setPeriod must re-anchor the grid to now, so the change does not manifest as a
// spurious skip burst — the immediate next cycle reports no skips even though
// the old baseline is long in the past relative to the new (larger) period.
TEST(CyclicTimerTest, SetPeriodDoesNotBurstSkips) {
  mm::core::CyclicTimer timer(kPeriod);
  timer.waitForNextCycle();

  // Sit well past the old grid, then change period. A naive impl that kept the
  // old baseline would fast-forward over the gap and report many skips.
  std::this_thread::sleep_for(5 * kPeriod);
  timer.setPeriod(std::chrono::microseconds(4000));

  EXPECT_EQ(timer.waitForNextCycle(), 0u);
}
