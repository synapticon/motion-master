// The test the design has never had: the control plane running against the cycle, on threads.
//
// `docs/LOCKING.md` says that its rules are checked by review and by nothing else. This file is the
// beginning of the answer. It drives one DeviceManager from several threads at once — one standing
// in for the RT loop, the others doing what HTTP workers, the samplers and a procedure do — and
// asserts that the manager survives it and still works afterwards.
//
// **What it can and cannot prove.** Run normally it catches crashes, use-after-free that trips the
// allocator, and any invariant an assertion below states. Run under ThreadSanitizer (the
// `x64-linux-tsan` preset) it also catches data races that happen to be benign today, which is the
// class of bug review is worst at. It cannot prove the absence of a race that these schedules never
// produce — so widen it when you touch the locking, rather than trusting a green run.
//
// The RT stand-in runs on an ordinary thread at ordinary priority, which is *harsher* than the real
// loop: it is preempted at arbitrary points, so the drain in pauseCycle really has to wait for it.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "comm/spoe_fieldbus_driver.h"
#include "fake_bus.h"
#include "fake_spoe_server.h"
#include "node/device.h"
#include "node/device_manager.h"
#include "node/process_data_ring.h"

namespace {

using mm::node::Device;
using mm::node::DeviceManager;
using mm::node::testing::makeCia402Bus;

/// How long each scenario runs. Long enough for thousands of cycles against hundreds of
/// control-plane operations, short enough to keep the suite quick.
constexpr auto kDuration = std::chrono::milliseconds(400);

/// Brings a manager up to "exchanging": driver, devices, dictionary, published image.
std::unique_ptr<DeviceManager> activatedManager() {
  auto bus = makeCia402Bus();
  bus->wkc = 3;
  auto dm = std::make_unique<DeviceManager>();
  if (!dm->init(std::move(bus)) || !dm->scan() || !dm->initializeDeviceParameters(1, false) ||
      !dm->configureProcessData()) {
    return nullptr;
  }
  return dm;
}

/// The RT loop, as a plain thread: enter the cycle, exchange, then read a device and a parameter
/// the way a cyclic task does. Counts the cycles it completed so a scenario can assert it ran at
/// all.
void runCycleLoop(DeviceManager& dm, const std::atomic<bool>& stop, std::atomic<uint64_t>& cycles) {
  while (!stop.load(std::memory_order_relaxed)) {
    const DeviceManager::CycleGuard cycle(dm);
    if (cycle) {
      dm.exchangeProcessData();
      // Exactly what a Tier-3 task does, and the reason the guard exists: a raw lookup, then a
      // cell.
      if (Device* device = dm.findDevice(1); device != nullptr) {
        static_cast<void>(device->value<uint16_t>(0x6041, 0x00));
        device->setValue<uint16_t>(0x6040, 0x00, 0x000F);
      }
      cycles.fetch_add(1, std::memory_order_relaxed);
    }
    std::this_thread::yield();
  }
}

// A rescan replaces the device set while the cycle runs, and a re-map re-allocates the recorder.
// Both refuse when the drain fails, which under this schedule happens often — the point is that
// every outcome is either a clean success or a clean error, and never a corrupted manager.
TEST(Concurrency, RescanAndRemapAgainstARunningCycle) {
  auto dm = activatedManager();
  ASSERT_NE(dm, nullptr);

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> cycles{0};
  std::atomic<uint64_t> scans{0};
  std::atomic<uint64_t> refusals{0};
  std::thread rt([&] { runCycleLoop(*dm, stop, cycles); });

  const auto deadline = std::chrono::steady_clock::now() + kDuration;
  while (std::chrono::steady_clock::now() < deadline) {
    if (dm->scan()) {
      scans.fetch_add(1, std::memory_order_relaxed);
      // A scan tears the image down, so bring it back for the next round.
      static_cast<void>(dm->configureProcessData());
    } else {
      refusals.fetch_add(1, std::memory_order_relaxed);
    }
  }
  stop.store(true, std::memory_order_relaxed);
  rt.join();

  EXPECT_GT(scans.load() + refusals.load(), 0U) << "the scanner must have run";
  // The manager still works: whatever the interleaving did, it left a usable object behind.
  ASSERT_TRUE(dm->scan().has_value());
  ASSERT_TRUE(dm->configureProcessData().has_value());
  EXPECT_TRUE(dm->processDataConfigured());
}

// A re-enumeration republishes a device's parameter index under the cycle. This is the path that
// used to dangle the published image's cell pointers; here the RT thread is decoding through those
// pointers the whole time.
TEST(Concurrency, ReEnumerationAgainstARunningCycle) {
  auto dm = activatedManager();
  ASSERT_NE(dm, nullptr);

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> cycles{0};
  std::thread rt([&] { runCycleLoop(*dm, stop, cycles); });

  uint64_t enumerations = 0;
  const auto deadline = std::chrono::steady_clock::now() + kDuration;
  while (std::chrono::steady_clock::now() < deadline) {
    ASSERT_TRUE(dm->initializeDeviceParameters(1, false).has_value());
    ++enumerations;
  }
  stop.store(true, std::memory_order_relaxed);
  rt.join();

  EXPECT_GT(enumerations, 0U);
  EXPECT_GT(cycles.load(), 0U) << "the cycle must have kept running throughout";
  // The cell the image points at is still the live one, and still decodes.
  Device* device = dm->findDevice(1);
  ASSERT_NE(device, nullptr);
  EXPECT_NE(device->findParameter(0x6041, 0x00), nullptr);
}

// Every read surface at once, against the cycle: the ones that take a snapshot, and the ones that
// walk the recorder under processDataMutex_ while the RT thread appends to it.
TEST(Concurrency, ReadSurfacesAgainstARunningCycle) {
  auto dm = activatedManager();
  ASSERT_NE(dm, nullptr);

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> cycles{0};
  std::thread rt([&] { runCycleLoop(*dm, stop, cycles); });

  std::vector<std::thread> readers;
  readers.reserve(3);
  for (int i = 0; i < 3; ++i) {
    readers.emplace_back([&] {
      const auto deadline = std::chrono::steady_clock::now() + kDuration;
      while (std::chrono::steady_clock::now() < deadline) {
        static_cast<void>(dm->deviceStates({}));
        static_cast<void>(dm->processImageInfo());
        static_cast<void>(dm->busConfig());
        if (const auto device = dm->deviceAt(1)) {
          static_cast<void>(device->parameterValue(0x6041, 0x00));
        }
        const uint64_t head = dm->recorderHead();
        if (head > 0) {
          mm::node::ProcessDataRing::Record record;
          static_cast<void>(dm->readRecord(head - 1, record));
        }
        const auto handle = dm->deviceAt(1);
        if (handle) {
          static_cast<void>(handle->parametersOrdered());
        }
      }
    });
  }
  for (auto& reader : readers) {
    reader.join();
  }
  stop.store(true, std::memory_order_relaxed);
  rt.join();

  EXPECT_GT(cycles.load(), 0U);
  EXPECT_TRUE(dm->processDataConfigured());
}

// reset() is the one operation that frees devices and never refuses, so it is the one that must
// hold memory back when the drain fails. Here it races the cycle, and a handle taken before the
// reset is read after it — which is a use-after-free if the retirement is wrong.
TEST(Concurrency, ResetAgainstARunningCycle) {
  for (int round = 0; round < 5; ++round) {
    auto dm = activatedManager();
    ASSERT_NE(dm, nullptr);

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> cycles{0};
    std::thread rt([&] { runCycleLoop(*dm, stop, cycles); });

    const auto handle = dm->deviceAt(1);
    ASSERT_TRUE(static_cast<bool>(handle));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    dm->reset();

    EXPECT_EQ(handle->slavePosition(), 1) << "a retired device must stay readable";
    EXPECT_FALSE(dm->initialised());

    stop.store(true, std::memory_order_relaxed);
    rt.join();
  }
}

// ── SPoE ─────────────────────────────────────────────────────────────────────────────────────────
//
// The same manager over the SPoE driver and the fake drive. This driver adds threads of its own:
// one exchange thread per drive, which shares the drive's connection with the control plane. The
// RT stand-in hands outputs to it and takes inputs from it without waiting.

using mm::comm::spoe::SpoeFieldbusDriver;
using mm::comm::spoe::SpoeFieldbusDriverConfig;
using mm::comm::spoe::SpoeMode;
using mm::comm::testing::FakeSpoeEntry;
using mm::comm::testing::FakeSpoeServer;

std::vector<uint8_t> le32(uint32_t v) {
  return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
          static_cast<uint8_t>(v >> 24)};
}

/// A CiA 402 drive on the fake: the same four mapped objects as makeCia402Bus, in OP.
void programSpoeDrive(FakeSpoeServer& server) {
  server.setObject(0x1C12, 0, {1});
  server.setObject(0x1C12, 1, {0x00, 0x16});
  server.setObject(0x1600, 0, {2});
  server.setObject(0x1600, 1, le32(0x60400010));
  server.setObject(0x1600, 2, le32(0x607A0020));
  server.setObject(0x1C13, 0, {1});
  server.setObject(0x1C13, 1, {0x00, 0x1A});
  server.setObject(0x1A00, 0, {2});
  server.setObject(0x1A00, 1, le32(0x60410010));
  server.setObject(0x1A00, 2, le32(0x60640020));
  server.setObject(0x6040, 0, {0, 0});
  server.setObject(0x607A, 0, {0, 0, 0, 0});
  server.setObject(0x6041, 0, {0, 0});
  server.setObject(0x6064, 0, {0, 0, 0, 0});
  const auto entry = [](uint16_t dataType, uint16_t bits, const char* name) {
    return FakeSpoeEntry{
        .dataType = dataType, .objectCode = 0x07, .bitLength = bits, .access = 0x3F, .name = name};
  };
  server.describeEntry(0x6040, 0, entry(0x0006, 16, "Controlword"));
  server.describeEntry(0x6041, 0, entry(0x0006, 16, "Statusword"));
  server.describeEntry(0x6064, 0, entry(0x0004, 32, "Position actual value"));
  server.describeEntry(0x607A, 0, entry(0x0004, 32, "Target position"));
  server.setState(mm::comm::testing::kSpoeStateOp);
}

std::unique_ptr<DeviceManager> spoeManager(const FakeSpoeServer& server, SpoeMode mode) {
  auto driver = std::make_unique<SpoeFieldbusDriver>(
      SpoeFieldbusDriverConfig{.hosts = {"127.0.0.1"},
                               .port = server.port(),
                               .mode = mode,
                               .watchdogMs = 75,
                               .requestTimeout = std::chrono::milliseconds(500),
                               .connectTimeout = std::chrono::milliseconds(500),
                               .exchangePeriod = std::chrono::microseconds(500)});
  auto dm = std::make_unique<DeviceManager>();
  if (!dm->init(std::move(driver)) || !dm->scan() || !dm->initializeDeviceParameters(1, false)) {
    return nullptr;
  }
  return dm;
}

/// The drive's side of the cycle: one input frame per millisecond, as the firmware buffers them.
void produceInputs(FakeSpoeServer& server, const std::atomic<bool>& stop) {
  uint32_t counter = 0;
  while (!stop.load(std::memory_order_relaxed)) {
    std::vector<uint8_t> frame{0x37, 0x02};
    const auto position = le32(++counter);
    frame.insert(frame.end(), position.begin(), position.end());
    server.pushInputFrame(frame);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

// Control mode: the cycle exchanges through the drive's exchange thread while SDO traffic, state
// reads and re-maps share the same connection.
TEST(Concurrency, SpoeExchangeAgainstTheControlPlane) {
  FakeSpoeServer server;
  programSpoeDrive(server);
  auto dm = spoeManager(server, SpoeMode::kControl);
  ASSERT_NE(dm, nullptr);
  ASSERT_TRUE(dm->configureProcessData().has_value());

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> cycles{0};
  std::thread drive([&] { produceInputs(server, stop); });
  std::thread rt([&] { runCycleLoop(*dm, stop, cycles); });
  std::thread worker([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      static_cast<void>(dm->deviceStates({}));
      if (auto device = dm->deviceAt(1); device) {
        static_cast<void>(device->readSdo(0x6041, 0x00));
      }
    }
  });

  const auto deadline = std::chrono::steady_clock::now() + kDuration;
  while (std::chrono::steady_clock::now() < deadline) {
    static_cast<void>(dm->configureProcessData());
  }
  stop.store(true, std::memory_order_relaxed);
  worker.join();
  rt.join();
  drive.join();

  EXPECT_GT(cycles.load(), 0U);
  ASSERT_TRUE(dm->configureProcessData().has_value());
  EXPECT_TRUE(dm->processDataConfigured());
  dm->reset();
}

// Monitor mode: another master moves the drive in and out of OP, and the follower re-maps and
// tears down under a running cycle.
TEST(Concurrency, SpoeFollowerAgainstARunningCycle) {
  FakeSpoeServer server;
  programSpoeDrive(server);
  auto dm = spoeManager(server, SpoeMode::kMonitor);
  ASSERT_NE(dm, nullptr);

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> cycles{0};
  std::atomic<uint64_t> passes{0};
  std::thread drive([&] { produceInputs(server, stop); });
  std::thread rt([&] { runCycleLoop(*dm, stop, cycles); });
  std::thread follower([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      static_cast<void>(dm->followObservedStates());
      passes.fetch_add(1, std::memory_order_relaxed);
    }
  });

  const auto deadline = std::chrono::steady_clock::now() + kDuration;
  bool op = true;
  while (std::chrono::steady_clock::now() < deadline) {
    op = !op;
    server.setState(op ? mm::comm::testing::kSpoeStateOp : mm::comm::testing::kSpoeStatePreOp);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  stop.store(true, std::memory_order_relaxed);
  follower.join();
  rt.join();
  drive.join();

  EXPECT_GT(passes.load(), 0U);
  server.setState(mm::comm::testing::kSpoeStateOp);
  ASSERT_TRUE(dm->followObservedStates().has_value());
  EXPECT_TRUE(dm->processDataConfigured());
  dm->reset();
}

}  // namespace
