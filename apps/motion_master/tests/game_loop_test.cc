#include "game_loop.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "core/cyclic_task.h"
#include "node/device_manager.h"

namespace {

/// Counts how many times the loop called it.
class CountingTask : public CyclicTask {
 public:
  void execute(const CycleContext&) noexcept override {
    calls.fetch_add(1, std::memory_order_relaxed);
  }

  std::atomic<uint64_t> calls{0};
};

// The loop enters the cycle before it calls a task, so a task never has to do it and never has to
// check. With no driver there is no published process image, the cycle is falsy, and no task may
// run: a task's findDevice would otherwise resolve against a device set the control plane is free
// to replace. The truthy half of the gate is covered by the CycleGuard tests in
// libs/node/tests/process_image_test.cc, which publish a real image through a fake driver.
TEST(GameLoopCycleGate, RunsNoTaskWhileTheBusIsNotActivated) {
  mm::node::DeviceManager deviceManager;
  GameLoop loop(deviceManager, std::chrono::microseconds(1000));
  CountingTask task;
  loop.addTask(&task);

  std::thread runner([&loop] { loop.run(); });
  // Long enough for a 1 ms loop to have executed many cycles had the gate let it.
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  loop.stop();
  runner.join();

  EXPECT_GT(loop.executedCycles(), 0U) << "the loop itself must keep cycling";
  EXPECT_EQ(task.calls.load(), 0U) << "no task may run while no process image is published";
}

}  // namespace
