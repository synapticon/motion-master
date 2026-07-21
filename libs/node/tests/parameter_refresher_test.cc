#include "node/parameter_refresher.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
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
using mm::node::DeviceManager;
using mm::node::ParameterRefresher;

constexpr uint16_t kU32 = 0x0007;
constexpr uint16_t kPreOp = static_cast<uint16_t>(EtherCatState::PreOp);

std::vector<uint8_t> u32le(uint32_t v) {
  return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
          static_cast<uint8_t>(v >> 24)};
}

/// One-slave driver double: scan() reports a single device whose object dictionary holds the
/// programmed entries; readSdo answers from a programmed map and counts calls per object (and in
/// total, atomically, for the threaded test) so a test can assert the refresher actually polled.
/// slaveState() is settable to bring the device online.
class FakeDriver : public FieldbusDriver {
 public:
  uint16_t state = 0;
  std::vector<OdEntry> ods;
  std::map<uint32_t, std::vector<uint8_t>> reads;
  std::map<uint32_t, int> readCounts;
  std::atomic<int> totalReads{0};

  static uint32_t key(uint16_t index, uint8_t subindex) {
    return (static_cast<uint32_t>(index) << 8) | subindex;
  }
  void programOd(uint16_t index, uint8_t subindex, uint16_t dataType) {
    OdEntry e{};
    e.index = index;
    e.subindex = subindex;
    e.dataType = dataType;
    ods.push_back(e);
  }
  void programRead(uint16_t index, uint8_t subindex, std::vector<uint8_t> bytes) {
    reads[key(index, subindex)] = std::move(bytes);
  }
  int readCount(uint16_t index, uint8_t subindex) const {
    auto it = readCounts.find(key(index, subindex));
    return it == readCounts.end() ? 0 : it->second;
  }

  std::expected<std::vector<uint8_t>, std::string> readSdo(uint16_t, uint16_t index,
                                                           uint8_t subindex) override {
    ++readCounts[key(index, subindex)];
    totalReads.fetch_add(1, std::memory_order_relaxed);
    auto it = reads.find(key(index, subindex));
    if (it == reads.end()) {
      return std::unexpected("no such object");
    }
    return it->second;
  }
  std::expected<std::vector<OdEntry>, std::string> readObjectDictionary(uint16_t) override {
    return ods;
  }
  uint16_t slaveState(uint16_t) const override { return state; }
  // CoE-capable stand-in: parameters are enumerated over the object dictionary, not SII.
  uint16_t mailboxProtocols(uint16_t) const override {
    return mm::comm::MailboxConfig::kProtocolCoe;
  }
  std::expected<std::vector<SlaveStateRaw>, std::string> readStates(
      const std::vector<uint16_t>& positions) override {
    return std::vector<SlaveStateRaw>(positions.size(),
                                      SlaveStateRaw{.alStatus = state, .alStatusCode = 0});
  }

  // --- unused stubs ---------------------------------------------------------
  std::expected<void, std::string> init() override { return {}; }
  std::expected<int, std::string> scan() override { return 1; }
  SlaveInfo slaveInfo(uint16_t) const override { return {}; }
  std::expected<void, std::string> configureProcessData() override { return {}; }
  mm::comm::PdoLayout processDataLayout() override { return {}; }
  std::vector<mm::comm::SlaveConfig> busConfig() const override { return {}; }
  int exchangeProcessData(std::span<const uint8_t>, std::span<uint8_t>) override { return 0; }
  void stop() override {}
  std::expected<std::vector<mm::comm::SlaveDiagnostics>, std::string> readDiagnostics(
      const std::vector<uint16_t>&) override {
    return {};
  }
  std::expected<std::vector<mm::comm::DcSyncDiagnostics>, std::string> readDcSync(
      const std::vector<uint16_t>&) override {
    return {};
  }
  std::expected<void, std::string> writeSdo(uint16_t, uint16_t, uint8_t,
                                            std::span<const uint8_t>) override {
    return {};
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
};

/// Brings @p dm up with one online device that has a single U32 parameter (0x2030:01) reading
/// back @p value. Returns the live driver pointer for per-test tweaks. (DeviceManager is
/// non-movable — it owns a shared_mutex — so it is built in place and taken by reference.)
FakeDriver* setUp(DeviceManager& dm, uint32_t value) {
  auto driver = std::make_unique<FakeDriver>();
  FakeDriver* raw = driver.get();
  raw->state = kPreOp;
  raw->programOd(0x2030, 0x01, kU32);
  raw->programRead(0x2030, 0x01, u32le(value));
  EXPECT_TRUE(dm.init(std::move(driver)).has_value());
  EXPECT_TRUE(dm.scan().has_value());
  EXPECT_TRUE(dm.initializeDeviceParameters(1, /*readValues=*/false).has_value());
  return raw;
}

TEST(ParameterRefresherTest, PollsDueParameterViaSdo) {
  DeviceManager dm;
  FakeDriver* driver = setUp(dm, 42);
  ParameterRefresher refresher(dm);

  refresher.acquire(1, 0x2030, 0x01, std::chrono::milliseconds{50});
  EXPECT_EQ(refresher.trackedCount(), 1u);

  refresher.pollDue();  // a freshly-acquired entry is due immediately
  EXPECT_EQ(driver->readCount(0x2030, 0x01), 1);
}

TEST(ParameterRefresherTest, RefcountTracksAcquireAndRelease) {
  DeviceManager dm;
  setUp(dm, 7);
  ParameterRefresher refresher(dm);

  refresher.acquire(1, 0x2030, 0x01, std::chrono::milliseconds{50});
  refresher.acquire(1, 0x2030, 0x01,
                    std::chrono::milliseconds{50});  // a second monitoring wants the same object
  EXPECT_EQ(refresher.trackedCount(), 1u);

  refresher.release(1, 0x2030, 0x01);  // one still needs it
  EXPECT_EQ(refresher.trackedCount(), 1u);

  refresher.release(1, 0x2030, 0x01);  // last reference gone
  EXPECT_EQ(refresher.trackedCount(), 0u);

  refresher.release(1, 0x2030, 0x01);  // extra release is ignored
  EXPECT_EQ(refresher.trackedCount(), 0u);
}

TEST(ParameterRefresherTest, ReleaseStopsPolling) {
  DeviceManager dm;
  FakeDriver* driver = setUp(dm, 1);
  ParameterRefresher refresher(dm);

  refresher.acquire(1, 0x2030, 0x01, std::chrono::milliseconds{50});
  refresher.pollDue();
  EXPECT_EQ(driver->readCount(0x2030, 0x01), 1);

  refresher.release(1, 0x2030, 0x01);
  EXPECT_EQ(refresher.trackedCount(), 0u);

  refresher.pollDue();  // nothing tracked → no further bus access
  EXPECT_EQ(driver->readCount(0x2030, 0x01), 1);
}

TEST(ParameterRefresherTest, BackgroundThreadPolls) {
  DeviceManager dm;
  FakeDriver* driver = setUp(dm, 99);
  ParameterRefresher refresher(dm);

  refresher.acquire(1, 0x2030, 0x01, std::chrono::milliseconds{10});
  refresher.start();

  // Wait (bounded) for the background thread to poll at least once.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (driver->totalReads.load(std::memory_order_relaxed) == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  refresher.stop();

  EXPECT_GE(driver->totalReads.load(std::memory_order_relaxed), 1);
}

}  // namespace
