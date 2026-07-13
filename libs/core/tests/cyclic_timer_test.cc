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
  // Slept past ~5 grid points; skip count is a robust lower bound (sleep_for
  // may overshoot, and alignment adds slack, so don't pin the exact value).
  EXPECT_GE(skipped, 3u);
  EXPECT_LE(skipped, 20u);
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
