#include "core/realtime.h"

#include <gtest/gtest.h>

#include <thread>

#ifdef __linux__
#include <sched.h>
#endif

using mm::core::RtSetupResult;
using mm::core::setRealtimePriority;

// These run unprivileged in CI, so SCHED_FIFO and mlockall are expected to fail.
// Affinity needs no privilege, which is what makes it testable here at all.

TEST(RealtimeTest, NoCpuRequestedLeavesThreadUnpinned) {
  bool pinned = false;
  // A separate thread keeps the change (or absence of one) off the test runner.
  std::thread([&] { pinned = setRealtimePriority(-1).cpuPinned; }).join();
  EXPECT_FALSE(pinned);
}

TEST(RealtimeTest, StepsAreIndependent) {
  RtSetupResult rt;
  std::thread([&] { rt = setRealtimePriority(-1); }).join();
  // Unprivileged: the two privileged steps fail, and that must not be fatal or
  // stop the call from returning a usable result.
  SUCCEED() << "schedFifo=" << rt.schedFifo << " memLocked=" << rt.memLocked
            << " cpuPinned=" << rt.cpuPinned;
}

#ifdef __linux__
TEST(RealtimeTest, PinsToRequestedCpu) {
  RtSetupResult rt;
  cpu_set_t actual;
  CPU_ZERO(&actual);

  std::thread([&] {
    rt = setRealtimePriority(0);
    sched_getaffinity(0, sizeof(actual), &actual);
  }).join();

  ASSERT_TRUE(rt.cpuPinned);
  EXPECT_TRUE(CPU_ISSET(0, &actual));
  EXPECT_EQ(CPU_COUNT(&actual), 1);
}

TEST(RealtimeTest, PinningIsPerThreadNotPerProcess) {
  cpu_set_t before;
  cpu_set_t after;
  CPU_ZERO(&before);
  CPU_ZERO(&after);
  ASSERT_EQ(sched_getaffinity(0, sizeof(before), &before), 0);

  std::thread([] { setRealtimePriority(0); }).join();

  // The whole point of using sched_setaffinity on the calling thread: the RT
  // thread moves to the isolated core while every other thread stays put.
  ASSERT_EQ(sched_getaffinity(0, sizeof(after), &after), 0);
  EXPECT_TRUE(CPU_EQUAL(&before, &after));
}

TEST(RealtimeTest, NonexistentCpuFailsWithoutThrowing) {
  bool pinned = true;
  // Far past any plausible core count; sched_setaffinity returns EINVAL and the
  // step simply reports that it did not take.
  std::thread([&] { pinned = setRealtimePriority(1 << 20).cpuPinned; }).join();
  EXPECT_FALSE(pinned);
}
#endif
