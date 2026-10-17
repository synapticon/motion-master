#include "comm/control_plane_locks.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace {

using mm::comm::ControlPlaneLocks;
using std::chrono::milliseconds;

// Long enough that a thread which could take a lock has taken it. A wait that shows a thread did
// not get a lock can only pass falsely on a slow host, never fail falsely.
constexpr milliseconds kSettle{50};

TEST(ControlPlaneLocks, TwoDrivesHoldTheirLocksAtTheSameTime) {
  ControlPlaneLocks locks(4);
  const auto first = locks.lockDrive(1);
  std::atomic<bool> acquired{false};
  std::jthread other([&] {
    const auto second = locks.lockDrive(2);
    acquired = true;
  });
  other.join();
  EXPECT_TRUE(acquired.load());
}

TEST(ControlPlaneLocks, OneDriveTakesOneHolderAtATime) {
  ControlPlaneLocks locks(4);
  auto first = std::make_unique<mm::comm::DriveLock>(locks.lockDrive(1));
  std::atomic<bool> acquired{false};
  std::jthread other([&] {
    const auto second = locks.lockDrive(1);
    acquired = true;
  });
  std::this_thread::sleep_for(kSettle);
  EXPECT_FALSE(acquired.load());
  first.reset();
  other.join();
  EXPECT_TRUE(acquired.load());
}

TEST(ControlPlaneLocks, TheBusLockExcludesEveryDrive) {
  ControlPlaneLocks locks(4);
  auto bus = locks.lockBus();
  std::atomic<bool> acquired{false};
  std::jthread other([&] {
    const auto drive = locks.lockDrive(2);
    acquired = true;
  });
  std::this_thread::sleep_for(kSettle);
  EXPECT_FALSE(acquired.load());
  bus.unlock();
  other.join();
  EXPECT_TRUE(acquired.load());
}

// A scan must wait only for the work that already started. A reader that arrives while the scan
// waits goes after the scan.
TEST(ControlPlaneLocks, AWaitingWriterStopsNewReaders) {
  ControlPlaneLocks locks(4);
  auto early = std::make_unique<mm::comm::DriveLock>(locks.lockDrive(1));
  std::atomic<int> order{0};
  std::atomic<int> writerTurn{0};
  std::atomic<int> lateReaderTurn{0};
  std::jthread writer([&] {
    const auto bus = locks.lockBus();
    writerTurn = ++order;
  });
  std::this_thread::sleep_for(kSettle);
  std::jthread lateReader([&] {
    const auto drive = locks.lockDrive(2);
    lateReaderTurn = ++order;
  });
  std::this_thread::sleep_for(kSettle);
  EXPECT_EQ(order.load(), 0);
  early.reset();
  writer.join();
  lateReader.join();
  EXPECT_EQ(writerTurn.load(), 1);
  EXPECT_EQ(lateReaderTurn.load(), 2);
}

// SOEM has a fixed number of frame indexes, so the control plane may have only so many frames on
// the wire. Each drive lock takes a frame slot.
TEST(ControlPlaneLocks, NoMoreDriveLocksThanFrameSlots) {
  constexpr int kThreads = ControlPlaneLocks::kFrameSlots + 4;
  ControlPlaneLocks locks(kThreads + 1);
  std::atomic<int> holding{0};
  std::atomic<int> most{0};
  {
    std::vector<std::jthread> threads;
    for (int i = 1; i <= kThreads; ++i) {
      threads.emplace_back([&, position = static_cast<uint16_t>(i)] {
        const auto drive = locks.lockDrive(position);
        const int now = ++holding;
        int seen = most.load();
        while (now > seen && !most.compare_exchange_weak(seen, now)) {
        }
        std::this_thread::sleep_for(kSettle);
        --holding;
      });
    }
  }
  EXPECT_EQ(most.load(), ControlPlaneLocks::kFrameSlots);
}

// A bus I/O lock takes a frame slot too, so it waits while every slot is held.
TEST(ControlPlaneLocks, ABusIoLockTakesAFrameSlot) {
  ControlPlaneLocks locks(ControlPlaneLocks::kFrameSlots + 1);
  std::vector<mm::comm::DriveLock> held;
  held.reserve(ControlPlaneLocks::kFrameSlots);
  for (int i = 1; i <= ControlPlaneLocks::kFrameSlots; ++i) {
    held.push_back(locks.lockDrive(static_cast<uint16_t>(i)));
  }
  std::atomic<bool> acquired{false};
  std::jthread reader([&] {
    const auto io = locks.lockBusIo();
    acquired = true;
  });
  std::this_thread::sleep_for(kSettle);
  EXPECT_FALSE(acquired.load());
  held.pop_back();
  reader.join();
  EXPECT_TRUE(acquired.load());
}

}  // namespace
