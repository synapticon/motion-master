#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <latch>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "comm/fieldbus_driver.h"
#include "node/device_manager.h"

/// @file
/// @brief Hammers the read surfaces of @c DeviceManager against a thread rebuilding the device set.
///
/// **Run these under ThreadSanitizer** — `cmake --preset x64-linux-tsan` — which is where they earn
/// their keep. Without it they mostly pass even against genuinely racy code: an unsynchronised read
/// of a vector being cleared usually reads memory that is still mapped, so the failure is a wrong
/// answer once in a while rather than a crash on demand. TSan turns the same run into a reported
/// data race with both stacks. That is the whole point of the file: the locking rules in
/// docs/LOCKING.md are otherwise checked by review alone, and review is what let a raw @c Device*
/// escape to 32 concurrent HTTP workers in the first place.
///
/// What each test pins is a *rule*, not a call: every public read surface must be safe against a
/// concurrent @c scan / @c reset without the caller doing anything. A new accessor that forgets its
/// lock fails here.
namespace {

using mm::comm::EtherCatState;
using mm::comm::FieldbusDriver;
using mm::comm::OdEntry;
using mm::comm::SlaveInfo;
using mm::node::Device;
using mm::node::DeviceManager;

/// A driver that answers everything instantly, so the tests measure the node layer's locking rather
/// than a fake bus. @c scan reports a count that changes between calls, which is what makes a
/// rebuild actually move the device vector (and reallocate it) rather than rewrite it in place.
class ChurningDriver : public FieldbusDriver {
 public:
  std::atomic<int> slaves{3};

  std::expected<void, std::string> init() override { return {}; }
  std::expected<int, std::string> scan() override { return slaves.load(); }
  SlaveInfo slaveInfo(uint16_t position) const override {
    return {.name = "device-" + std::to_string(position),
            .vendorId = 0x22D2,
            .productCode = 0x201,
            .revisionNumber = 1,
            .serialNumber = position};
  }
  std::expected<void, std::string> configureProcessData() override { return {}; }
  mm::comm::PdoLayout processDataLayout() override { return {}; }
  std::vector<mm::comm::SlaveConfig> busConfig() const override {
    std::vector<mm::comm::SlaveConfig> out;
    for (uint16_t i = 1; i <= 3; ++i) {
      mm::comm::SlaveConfig c{};
      c.slavePosition = i;
      out.push_back(c);
    }
    return out;
  }
  int exchangeProcessData(std::span<const uint8_t>, std::span<uint8_t>) override { return 0; }
  void stop() override {}

  std::expected<std::vector<SlaveStateRaw>, std::string> readStates(
      const std::vector<uint16_t>& positions) override {
    return std::vector<SlaveStateRaw>(
        positions.size(),
        SlaveStateRaw{.alStatus = static_cast<uint16_t>(EtherCatState::PreOp), .alStatusCode = 0});
  }
  uint16_t slaveState(uint16_t) const override {
    return static_cast<uint16_t>(EtherCatState::PreOp);
  }
  uint16_t mailboxProtocols(uint16_t) const override {
    return mm::comm::MailboxConfig::kProtocolCoe;
  }
  std::expected<std::vector<uint8_t>, std::string> readSdo(uint16_t, uint16_t, uint8_t) override {
    return std::vector<uint8_t>{0, 0, 0, 0};
  }
  std::expected<void, std::string> writeSdo(uint16_t, uint16_t, uint8_t,
                                            std::span<const uint8_t>) override {
    return {};
  }
  std::expected<std::vector<OdEntry>, std::string> readObjectDictionary(uint16_t) override {
    // Two entries is enough: the point is that initializeParameters *replaces* the map, which is
    // what a concurrent unlocked reader of it would trip over.
    return std::vector<OdEntry>{OdEntry{.index = 0x6040,
                                        .subindex = 0,
                                        .objectCode = 7,
                                        .dataType = 0x06,
                                        .bitLength = 16,
                                        .access = 0x3F,
                                        .name = "Controlword",
                                        .unit = std::nullopt,
                                        .defaultValue = std::nullopt,
                                        .minValue = std::nullopt,
                                        .maxValue = std::nullopt},
                                OdEntry{.index = 0x6041,
                                        .subindex = 0,
                                        .objectCode = 7,
                                        .dataType = 0x06,
                                        .bitLength = 16,
                                        .access = 0x07,
                                        .name = "Statusword",
                                        .unit = std::nullopt,
                                        .defaultValue = std::nullopt,
                                        .minValue = std::nullopt,
                                        .maxValue = std::nullopt}};
  }
  std::expected<std::vector<uint8_t>, mm::comm::FoeError> readFile(uint16_t,
                                                                   const std::string&) override {
    return std::vector<uint8_t>{};
  }
  std::expected<void, mm::comm::FoeError> writeFile(uint16_t, const std::string&,
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
};

/// Runs @p readers concurrently with a thread that rebuilds the device set, for @p rounds rebuilds.
///
/// Every reader loops until the churn thread is done, so each one overlaps many rebuilds. Nothing
/// is asserted about the *values* read — a position may or may not resolve depending on when the
/// read lands, and both answers are correct. What is asserted is that the process survives, and
/// under TSan, that no read raced the rebuild.
void churnAgainst(DeviceManager& dm, ChurningDriver& driver, int rounds,
                  const std::vector<std::function<void(DeviceManager&)>>& readers) {
  std::atomic<bool> done{false};
  std::latch started(static_cast<ptrdiff_t>(readers.size()) + 1);

  std::vector<std::jthread> threads;
  threads.reserve(readers.size());
  for (const auto& reader : readers) {
    threads.emplace_back([&dm, &done, &started, reader] {
      started.arrive_and_wait();
      while (!done.load(std::memory_order_relaxed)) {
        reader(dm);
      }
    });
  }

  started.arrive_and_wait();
  for (int i = 0; i < rounds; ++i) {
    // Alternate the slave count so the vector is genuinely reallocated, not just rewritten.
    driver.slaves.store(2 + (i % 3));
    ASSERT_TRUE(dm.scan().has_value());
  }
  done.store(true, std::memory_order_relaxed);
}

// Every position-based read surface, run against a device set being rebuilt underneath it. These
// are the calls an HTTP worker makes while another worker serves POST /api/scan.
TEST(DeviceManagerConcurrency, ReadSurfacesSurviveARescan) {
  DeviceManager dm;
  auto driver = std::make_unique<ChurningDriver>();
  ChurningDriver* raw = driver.get();
  ASSERT_TRUE(dm.init(std::move(driver)).has_value());
  ASSERT_TRUE(dm.scan().has_value());

  churnAgainst(
      dm, *raw, 200,
      {
          // GET /api/devices — serialises the whole vector.
          [](DeviceManager& d) { [[maybe_unused]] nlohmann::json j = d; },
          // GET /api/bus-config — dereferences driver_ and resolves names against devices_.
          [](DeviceManager& d) { [[maybe_unused]] auto c = d.busConfig(); },
          // GET /api/process-image — walks the retained generations and each device's
          // parameter map.
          [](DeviceManager& d) { [[maybe_unused]] auto i = d.processImageInfo(); },
          // GET /api/devices/:pos/state and friends.
          [](DeviceManager& d) { [[maybe_unused]] auto s = d.deviceStates({}); },
          // The 404 guard every device route runs first.
          [](DeviceManager& d) { [[maybe_unused]] bool h = d.hasDevice(2); },
          [](DeviceManager& d) { [[maybe_unused]] bool i = d.initialised(); },
          // The monitoring sampler's per-flush gate and cached-value read.
          [](DeviceManager& d) { [[maybe_unused]] bool e = d.deviceExchangesProcessData(1); },
          [](DeviceManager& d) { [[maybe_unused]] auto v = d.value(1, 0x6041, 0); },
          // The recorder accessors — allocate()/clear() free the ring from the churn side.
          [](DeviceManager& d) { [[maybe_unused]] uint64_t h = d.recorderHead(); },
      });
}

// A borrow must keep its Device& valid for the callable's whole duration, however long that is.
// This is the property procedures depend on: a body runs for seconds holding one reference.
TEST(DeviceManagerConcurrency, BorrowedDeviceOutlivesARescan) {
  DeviceManager dm;
  auto driver = std::make_unique<ChurningDriver>();
  ChurningDriver* raw = driver.get();
  ASSERT_TRUE(dm.init(std::move(driver)).has_value());
  ASSERT_TRUE(dm.scan().has_value());

  churnAgainst(dm, *raw, 100,
               {
                   [](DeviceManager& d) {
                     // Touch the borrowed device repeatedly. A rescan landing mid-callable must
                     // wait, not free it underneath us — and the identity read must stay
                     // self-consistent throughout.
                     [[maybe_unused]] auto r =
                         d.withDevice(1, [](Device& device) -> std::expected<void, std::string> {
                           for (int i = 0; i < 32; ++i) {
                             EXPECT_EQ(device.slavePosition(), 1);
                             EXPECT_EQ(device.serialNumber(), 1u);
                           }
                           return {};
                         });
                   },
                   [](DeviceManager& d) {
                     [[maybe_unused]] auto n = d.withDevices(
                         [](const std::vector<Device>& devices) { return devices.size(); });
                   },
               });
}

// The parameter map is replaced wholesale by initializeParameters. Every reader of it must go
// through the device's own lock — which is what makes Device::parameter return a copy.
TEST(DeviceManagerConcurrency, ParameterReadersSurviveAReEnumeration) {
  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::make_unique<ChurningDriver>()).has_value());
  ASSERT_TRUE(dm.scan().has_value());
  ASSERT_TRUE(dm.initializeDeviceParameters(1, /*readValues=*/false).has_value());

  std::atomic<bool> done{false};
  std::latch started(4);

  // Readers that reach into a Device's parameter map from their own threads.
  auto reader = [&](std::function<void()> body) {
    return std::jthread([&done, &started, body = std::move(body)] {
      started.arrive_and_wait();
      while (!done.load(std::memory_order_relaxed)) {
        body();
      }
    });
  };

  auto a = reader([&dm] { [[maybe_unused]] auto i = dm.processImageInfo(); });
  auto b = reader([&dm] { [[maybe_unused]] nlohmann::json j = dm; });  // to_json → isCia402
  auto c = reader([&dm] {
    [[maybe_unused]] auto r = dm.withDevice(1, [](Device& d) -> std::expected<void, std::string> {
      [[maybe_unused]] auto p = d.parameter(0x6041, 0);
      [[maybe_unused]] auto t = d.dataType(0x6041, 0);
      [[maybe_unused]] bool h = d.hasParameters();
      [[maybe_unused]] auto all = d.parametersOrdered();
      return {};
    });
  });

  started.arrive_and_wait();
  for (int i = 0; i < 200; ++i) {
    ASSERT_TRUE(dm.initializeDeviceParameters(1, /*readValues=*/false).has_value());
  }
  done.store(true, std::memory_order_relaxed);
}

}  // namespace
