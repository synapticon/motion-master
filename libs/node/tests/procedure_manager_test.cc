#include "node/procedure_manager.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "comm/fieldbus_driver.h"
#include "node/device_manager.h"

namespace {

using mm::comm::EtherCatState;
using mm::comm::FieldbusDriver;
using mm::comm::OdEntry;
using mm::comm::SlaveInfo;
using mm::node::Device;
using mm::node::DeviceManager;
using mm::node::ProcedureManager;
using mm::node::ProcedureStartError;
using mm::node::ProcedureStatus;
using mm::node::ProgressReporter;
using mm::node::ProgressStatus;
using mm::node::ProgressStep;

/// Driver double that reports a configurable slave count and nothing else — these tests exercise
/// the manager's threading, exclusion and retention, never the bus.
class CountingFakeDriver : public FieldbusDriver {
 public:
  explicit CountingFakeDriver(int slaves = 2) : slaves_(slaves) {}

  std::expected<void, std::string> init() override { return {}; }
  std::expected<int, std::string> scan() override { return slaves_; }
  SlaveInfo slaveInfo(uint16_t) const override { return SlaveInfo{}; }
  uint16_t slaveState(uint16_t) const override {
    return static_cast<uint16_t>(EtherCatState::PreOp);
  }
  std::expected<std::vector<OdEntry>, std::string> readObjectDictionary(uint16_t) override {
    return std::vector<OdEntry>{};
  }

  // --- unused stubs ---------------------------------------------------------
  std::expected<void, std::string> configureProcessData() override { return {}; }
  mm::comm::PdoLayout processDataLayout() override { return {}; }
  int exchangeProcessData(std::span<const uint8_t>, std::span<uint8_t>) override { return 0; }
  void stop() override {}
  std::expected<std::vector<uint8_t>, std::string> readSdo(uint16_t, uint16_t, uint8_t) override {
    return std::vector<uint8_t>{};
  }
  std::expected<void, std::string> writeSdo(uint16_t, uint16_t, uint8_t,
                                            std::span<const uint8_t>) override {
    return {};
  }
  std::expected<std::vector<SlaveStateRaw>, std::string> readStates(
      const std::vector<uint16_t>& positions) override {
    return std::vector<SlaveStateRaw>(positions.size(), SlaveStateRaw{});
  }
  std::expected<std::vector<uint8_t>, std::string> readFile(uint16_t, const std::string&) override {
    return std::vector<uint8_t>{};
  }
  std::expected<void, std::string> writeFile(uint16_t, const std::string&,
                                             std::span<const uint8_t>) override {
    return {};
  }
  std::expected<void, std::string> readRegister(uint16_t, uint16_t, std::span<uint8_t>) override {
    return {};
  }
  std::expected<void, std::string> writeRegister(uint16_t, uint16_t,
                                                 std::span<const uint8_t>) override {
    return {};
  }
  void transitionToState(const std::vector<uint16_t>&, std::optional<EtherCatState>, EtherCatState,
                         std::chrono::steady_clock::duration, std::chrono::steady_clock::duration,
                         std::function<void()>, std::function<bool()>) override {}

 private:
  int slaves_;
};

std::vector<ProgressStep> oneStep(std::string id = "work") {
  ProgressStep step;
  step.id = std::move(id);
  return {step};
}

// A body that blocks until released, so a test can observe a run mid-flight.
//
// The wait is deliberately the stop-aware std::condition_variable_any overload. request_stop() does
// not notify a plain std::condition_variable, so a predicate that merely checks stop_requested()
// would never be re-evaluated and the body would block for ever — taking the manager's destructor
// down with it, since it joins. That is the standing hazard of cooperative cancellation, and a
// blocking procedure body is exactly where it bites.
class Gate {
 public:
  void waitUntilOpen(std::stop_token stop) {
    std::unique_lock lock(mutex_);
    opened_.wait(lock, std::move(stop), [&] { return open_; });
  }
  void open() {
    {
      const std::lock_guard lock(mutex_);
      open_ = true;
    }
    opened_.notify_all();
  }
  void waitUntilEntered() {
    std::unique_lock lock(mutex_);
    entered_.wait(lock, [&] { return isEntered_; });
  }
  void markEntered() {
    {
      const std::lock_guard lock(mutex_);
      isEntered_ = true;
    }
    entered_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable_any opened_;
  std::condition_variable entered_;
  bool open_ = false;
  bool isEntered_ = false;
};

/// A scanned device set. DeviceManager is neither copyable nor movable (it owns the driver and the
/// process-data state), so tests hold one in place rather than taking it from a factory.
struct Bus {
  DeviceManager dm;

  explicit Bus(int slaves = 2) {
    EXPECT_TRUE(dm.init(std::make_unique<CountingFakeDriver>(slaves)).has_value());
    EXPECT_TRUE(dm.scan().has_value());
  }
};

// Waits for a procedure to reach a terminal status, so tests never sleep on a fixed duration.
ProcedureStatus awaitCompletion(const ProcedureManager& manager, uint16_t position,
                                std::string_view name) {
  for (int attempt = 0; attempt < 2000; ++attempt) {
    auto snapshot = manager.snapshot(position, name);
    if (snapshot && snapshot->status != ProcedureStatus::kRunning) {
      return snapshot->status;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ADD_FAILURE() << "procedure '" << name << "' never finished";
  return ProcedureStatus::kRunning;
}

TEST(ProcedureManagerStart, RunsTheBodyAndRecordsSuccess) {
  Bus bus;
  ProcedureManager manager(bus.dm);

  std::atomic<uint16_t> sawPosition{0};
  auto started = manager.start(2, "demo", oneStep(),
                               [&sawPosition](Device& device, ProgressReporter& reporter,
                                              std::stop_token) -> std::expected<void, std::string> {
                                 sawPosition.store(device.slavePosition());
                                 reporter.start("work");
                                 reporter.succeed("work", 42);
                                 return {};
                               });
  ASSERT_TRUE(started.has_value()) << started.error();
  EXPECT_EQ(awaitCompletion(manager, 2, "demo"), ProcedureStatus::kSucceeded);
  EXPECT_EQ(sawPosition.load(), 2) << "the body must receive the addressed device";

  auto snapshot = manager.snapshot(2, "demo");
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_EQ(snapshot->runCount, 1u);
  ASSERT_TRUE(snapshot->startedAt.has_value());
  ASSERT_TRUE(snapshot->finishedAt.has_value());
  EXPECT_GE(*snapshot->finishedAt, *snapshot->startedAt);
  ASSERT_EQ(snapshot->steps.size(), 1u);
  EXPECT_EQ(snapshot->steps[0].status, ProgressStatus::kSucceeded);
  EXPECT_EQ(snapshot->steps[0].value, 42);
  EXPECT_FALSE(snapshot->error.has_value());
}

TEST(ProcedureManagerStart, RecordsAFailureReturnedOutsideAnyStep) {
  Bus bus;
  ProcedureManager manager(bus.dm);

  ASSERT_TRUE(manager
                  .start(1, "demo", oneStep(),
                         [](Device&, ProgressReporter&,
                            std::stop_token) -> std::expected<void, std::string> {
                           return std::unexpected("not the right kind of device");
                         })
                  .has_value());
  EXPECT_EQ(awaitCompletion(manager, 1, "demo"), ProcedureStatus::kFailed);

  auto snapshot = manager.snapshot(1, "demo");
  ASSERT_TRUE(snapshot.has_value());
  ASSERT_TRUE(snapshot->error.has_value());
  EXPECT_EQ(*snapshot->error, "not the right kind of device");
}

TEST(ProcedureManagerStart, RejectsASecondRunOnABusyDevice) {
  Bus bus;
  ProcedureManager manager(bus.dm);
  Gate gate;

  ASSERT_TRUE(manager
                  .start(1, "slow", oneStep(),
                         [&gate](Device&, ProgressReporter&,
                                 std::stop_token stop) -> std::expected<void, std::string> {
                           gate.markEntered();
                           gate.waitUntilOpen(stop);
                           return {};
                         })
                  .has_value());
  gate.waitUntilEntered();

  // A different procedure on the same device is refused: exclusion is per device, not per
  // procedure name — the drive can only be doing one thing at a time.
  auto rejected =
      manager.start(1, "other", oneStep(),
                    [](Device&, ProgressReporter&,
                       std::stop_token) -> std::expected<void, std::string> { return {}; });
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(rejected.error().kind, ProcedureStartError::Kind::kBusy);
  EXPECT_NE(rejected.error().message.find("slow"), std::string::npos) << rejected.error();

  gate.open();
  EXPECT_EQ(awaitCompletion(manager, 1, "slow"), ProcedureStatus::kSucceeded);
}

TEST(ProcedureManagerStart, AllowsConcurrentRunsOnDifferentDevices) {
  Bus bus;
  ProcedureManager manager(bus.dm);
  Gate gate;

  ASSERT_TRUE(manager
                  .start(1, "slow", oneStep(),
                         [&gate](Device&, ProgressReporter&,
                                 std::stop_token stop) -> std::expected<void, std::string> {
                           gate.markEntered();
                           gate.waitUntilOpen(stop);
                           return {};
                         })
                  .has_value());
  gate.waitUntilEntered();

  // The busy claim is per device; device 2 is untouched. (Both hold the bus lock *shared*, so they
  // genuinely overlap.)
  auto second =
      manager.start(2, "quick", oneStep(),
                    [](Device&, ProgressReporter&,
                       std::stop_token) -> std::expected<void, std::string> { return {}; });
  ASSERT_TRUE(second.has_value()) << second.error();
  EXPECT_EQ(awaitCompletion(manager, 2, "quick"), ProcedureStatus::kSucceeded);

  gate.open();
  EXPECT_EQ(awaitCompletion(manager, 1, "slow"), ProcedureStatus::kSucceeded);
}

TEST(ProcedureManagerStart, RejectsAnUnknownDeviceBeforeSpawning) {
  Bus bus;
  ProcedureManager manager(bus.dm);

  bool ran = false;
  auto rejected = manager.start(
      99, "demo", oneStep(),
      [&ran](Device&, ProgressReporter&, std::stop_token) -> std::expected<void, std::string> {
        ran = true;
        return {};
      });
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(rejected.error().kind, ProcedureStartError::Kind::kUnknownDevice);
  EXPECT_FALSE(ran) << "an unknown position must fail before any thread is spawned";
  EXPECT_FALSE(manager.snapshot(99, "demo").has_value());
}

TEST(ProcedureManagerStart, CountsAcceptedRunsOnly) {
  Bus bus;
  ProcedureManager manager(bus.dm);

  auto body = [](Device&, ProgressReporter&, std::stop_token) -> std::expected<void, std::string> {
    return {};
  };
  for (int run = 0; run < 3; ++run) {
    ASSERT_TRUE(manager.start(1, "demo", oneStep(), body).has_value());
    ASSERT_EQ(awaitCompletion(manager, 1, "demo"), ProcedureStatus::kSucceeded);
  }
  EXPECT_EQ(manager.snapshot(1, "demo")->runCount, 3u);

  // A rejected start must not advance the counter — it is a generation marker for runs that
  // actually happened, so a poller can tell one run from the next.
  Gate gate;
  ASSERT_TRUE(manager
                  .start(1, "slow", oneStep(),
                         [&gate](Device&, ProgressReporter&,
                                 std::stop_token stop) -> std::expected<void, std::string> {
                           gate.markEntered();
                           gate.waitUntilOpen(stop);
                           return {};
                         })
                  .has_value());
  gate.waitUntilEntered();
  EXPECT_FALSE(manager.start(1, "demo", oneStep(), body).has_value());
  EXPECT_EQ(manager.snapshot(1, "demo")->runCount, 3u);
  gate.open();
  EXPECT_EQ(awaitCompletion(manager, 1, "slow"), ProcedureStatus::kSucceeded);
}

TEST(ProcedureManagerCancel, StopsARunningProcedure) {
  Bus bus;
  ProcedureManager manager(bus.dm);
  Gate gate;

  ASSERT_TRUE(manager
                  .start(1, "slow", oneStep(),
                         [&gate](Device&, ProgressReporter& reporter,
                                 std::stop_token stop) -> std::expected<void, std::string> {
                           reporter.start("work");
                           gate.markEntered();
                           gate.waitUntilOpen(stop);
                           if (stop.stop_requested()) {
                             return std::unexpected("stopped");
                           }
                           return {};
                         })
                  .has_value());
  gate.waitUntilEntered();

  EXPECT_TRUE(manager.cancel(1, "slow"));
  EXPECT_EQ(awaitCompletion(manager, 1, "slow"), ProcedureStatus::kCancelled);

  // Cancellation is distinct from failure, and the step it stopped on stays running — a truthful
  // record of how far it got.
  auto snapshot = manager.snapshot(1, "slow");
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_EQ(snapshot->steps[0].status, ProgressStatus::kRunning);
}

TEST(ProcedureManagerCancel, ReportsNothingToCancel) {
  Bus bus;
  ProcedureManager manager(bus.dm);

  EXPECT_FALSE(manager.cancel(1, "never-run"));

  ASSERT_TRUE(manager
                  .start(1, "demo", oneStep(),
                         [](Device&, ProgressReporter&,
                            std::stop_token) -> std::expected<void, std::string> { return {}; })
                  .has_value());
  ASSERT_EQ(awaitCompletion(manager, 1, "demo"), ProcedureStatus::kSucceeded);
  EXPECT_FALSE(manager.cancel(1, "demo")) << "a finished run has nothing to cancel";
}

TEST(ProcedureManagerSnapshot, IsRetainedAfterTheRunEnds) {
  Bus bus;
  ProcedureManager manager(bus.dm);

  ASSERT_TRUE(manager
                  .start(1, "demo", oneStep(),
                         [](Device&, ProgressReporter& reporter,
                            std::stop_token) -> std::expected<void, std::string> {
                           reporter.succeed("work", 7);
                           return {};
                         })
                  .has_value());
  ASSERT_EQ(awaitCompletion(manager, 1, "demo"), ProcedureStatus::kSucceeded);

  // Long after the thread is gone, the result is still there — the returning-user case.
  auto snapshot = manager.snapshot(1, "demo");
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_EQ(snapshot->status, ProcedureStatus::kSucceeded);
  EXPECT_EQ(snapshot->steps[0].value, 7);
}

TEST(ProcedureManagerSnapshot, IsDiscardedWhenTheDeviceSetIsRebuilt) {
  Bus bus;
  ProcedureManager manager(bus.dm);

  ASSERT_TRUE(manager
                  .start(1, "demo", oneStep(),
                         [](Device&, ProgressReporter&,
                            std::stop_token) -> std::expected<void, std::string> { return {}; })
                  .has_value());
  ASSERT_EQ(awaitCompletion(manager, 1, "demo"), ProcedureStatus::kSucceeded);
  ASSERT_TRUE(manager.snapshot(1, "demo").has_value());

  // Positions may name different hardware after a rescan, so a retained measurement would be
  // rendered against the wrong drive. Noticed by watching topologyGeneration — DeviceManager is
  // never told the manager exists.
  ASSERT_TRUE(bus.dm.scan().has_value());
  EXPECT_FALSE(manager.snapshot(1, "demo").has_value());
}

TEST(ProcedureManagerDestruction, CancelsAndJoinsRunningProcedures) {
  Bus bus;
  Gate gate;
  std::atomic<bool> observedStop{false};

  {
    ProcedureManager manager(bus.dm);
    ASSERT_TRUE(manager
                    .start(1, "slow", oneStep(),
                           [&](Device&, ProgressReporter&,
                               std::stop_token stop) -> std::expected<void, std::string> {
                             gate.markEntered();
                             gate.waitUntilOpen(stop);
                             observedStop.store(stop.stop_requested());
                             return {};
                           })
                    .has_value());
    gate.waitUntilEntered();
    // Leaving this scope must request the stop and wait, not detach or deadlock.
  }

  EXPECT_TRUE(observedStop.load()) << "the destructor must request cancellation before joining";
}

}  // namespace
