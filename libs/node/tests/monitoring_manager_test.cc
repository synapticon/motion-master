#include "node/monitoring_manager.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
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
#include "node/monitoring.h"

namespace {

using mm::comm::EtherCatState;
using mm::comm::FieldbusDriver;
using mm::comm::OdEntry;
using mm::comm::PdoLayout;
using mm::comm::SlaveInfo;
using mm::comm::SlaveIo;
using mm::node::DeviceManager;
using mm::node::MonitoredParameter;
using mm::node::Monitoring;
using mm::node::MonitoringManager;

constexpr uint16_t kU16 = 0x0006;
constexpr uint16_t kI32 = 0x0004;
constexpr uint16_t kU32 = 0x0007;

std::vector<uint8_t> u8le(uint8_t v) { return {v}; }
std::vector<uint8_t> u16le(uint16_t v) {
  return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
}
std::vector<uint8_t> u32le(uint32_t v) {
  return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
          static_cast<uint8_t>(v >> 24)};
}
std::vector<uint8_t> pdoEntry(uint16_t index, uint8_t sub, uint8_t bits) {
  return u32le((static_cast<uint32_t>(index) << 16) | (static_cast<uint32_t>(sub) << 8) | bits);
}

/// One-slave CiA402 fake: canned SDO reads (so PDO mappings resolve), a programmed layout, and an
/// exchange that copies a canned input image back. State is settable to drive the live gate.
class FakeBus : public FieldbusDriver {
 public:
  std::map<uint32_t, std::vector<uint8_t>> reads;
  std::vector<OdEntry> ods;
  PdoLayout layout;
  std::vector<uint8_t> cannedInputs;
  uint16_t state = 0;
  int wkc = 0;

  static uint32_t key(uint16_t index, uint8_t sub) {
    return (static_cast<uint32_t>(index) << 8) | sub;
  }
  void program(uint16_t index, uint8_t sub, std::vector<uint8_t> bytes) {
    reads[key(index, sub)] = std::move(bytes);
  }
  void programOd(uint16_t index, uint8_t sub, uint16_t dataType) {
    OdEntry e{};
    e.index = index;
    e.subindex = sub;
    e.dataType = dataType;
    ods.push_back(e);
  }

  std::expected<int, std::string> scan() override { return 1; }
  uint16_t slaveState(uint16_t) const override { return state; }
  std::expected<void, std::string> configureProcessData() override { return {}; }
  PdoLayout processDataLayout() override { return layout; }
  int exchangeProcessData(std::span<const uint8_t>, std::span<uint8_t> inputs) override {
    for (size_t i = 0; i < inputs.size() && i < cannedInputs.size(); ++i) {
      inputs[i] = cannedInputs[i];
    }
    return wkc;
  }
  std::expected<std::vector<uint8_t>, std::string> readSdo(uint16_t, uint16_t index,
                                                           uint8_t sub) override {
    auto it = reads.find(key(index, sub));
    if (it == reads.end()) {
      return std::unexpected("no such object");
    }
    return it->second;
  }
  std::expected<std::vector<OdEntry>, std::string> readObjectDictionary(uint16_t) override {
    return ods;
  }

  // --- unused stubs ---------------------------------------------------------
  std::expected<void, std::string> init() override { return {}; }
  SlaveInfo slaveInfo(uint16_t) const override { return {}; }
  std::vector<mm::comm::SlaveConfig> busConfig() const override { return {}; }
  void stop() override {}
  std::expected<std::vector<SlaveStateRaw>, std::string> readStates(
      const std::vector<uint16_t>& p) override {
    return std::vector<SlaveStateRaw>(p.size(),
                                      SlaveStateRaw{.alStatus = state, .alStatusCode = 0});
  }
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

// CiA402 6-byte-per-direction mapping + an SDO-only object 0x2030:01 (temperature), state OP.
// Canned inputs: statusword 0x0237 @0, actual position 0x11223344 @16.
std::unique_ptr<FakeBus> makeBus() {
  auto bus = std::make_unique<FakeBus>();
  bus->program(0x1C12, 0x00, u8le(1));
  bus->program(0x1C12, 0x01, u16le(0x1600));
  bus->program(0x1600, 0x00, u8le(2));
  bus->program(0x1600, 0x01, pdoEntry(0x6040, 0x00, 16));  // controlword @0
  bus->program(0x1600, 0x02, pdoEntry(0x607A, 0x00, 32));  // target position @16
  bus->program(0x1C13, 0x00, u8le(1));
  bus->program(0x1C13, 0x01, u16le(0x1A00));
  bus->program(0x1A00, 0x00, u8le(2));
  bus->program(0x1A00, 0x01, pdoEntry(0x6041, 0x00, 16));  // statusword @0
  bus->program(0x1A00, 0x02, pdoEntry(0x6064, 0x00, 32));  // actual position @16
  bus->programOd(0x6040, 0x00, kU16);
  bus->programOd(0x607A, 0x00, kI32);
  bus->programOd(0x6041, 0x00, kU16);
  bus->programOd(0x6064, 0x00, kI32);
  bus->programOd(0x2030, 0x01, kU32);    // SDO-only (not PDO-mapped)
  bus->program(0x2030, 0x01, u32le(0));  // so a background refresher SDO read succeeds
  bus->layout.outputBytes = 6;
  bus->layout.inputBytes = 6;
  bus->layout.expectedWkc = 3;
  bus->layout.slaves = {SlaveIo{
      .slavePosition = 1, .outputOffset = 0, .outputBytes = 6, .inputOffset = 0, .inputBytes = 6}};
  bus->state = static_cast<uint16_t>(EtherCatState::Op);
  bus->wkc = 3;
  bus->cannedInputs = {0x37, 0x02, 0x44, 0x33, 0x22, 0x11};
  return bus;
}

// Brings dm up to OP with the image published and one input snapshot captured. Returns the driver.
FakeBus* setUp(DeviceManager& dm) {
  auto bus = makeBus();
  FakeBus* raw = bus.get();
  EXPECT_TRUE(dm.init(std::move(bus)).has_value());
  EXPECT_TRUE(dm.scan().has_value());
  EXPECT_TRUE(dm.initializeDeviceParameters(1, false).has_value());
  EXPECT_TRUE(dm.configureProcessData().has_value());
  dm.exchangeProcessData();  // publish the canned input snapshot
  return raw;
}

// A monitoring of actual position (PDO input) + temperature (SDO).
Monitoring axisConfig() {
  Monitoring m;
  m.topic = "axis";
  m.interval = std::chrono::milliseconds{10};
  m.parameters = {MonitoredParameter{1, 0x6064, 0x00}, MonitoredParameter{1, 0x2030, 0x01}};
  return m;
}

// Records @p n process-data cycles into the recorder ring (each advances recorderHead by one).
void drive(DeviceManager& dm, int n) {
  for (int i = 0; i < n; ++i) {
    dm.exchangeProcessData();
  }
}

TEST(MonitoringManagerTest, CreateClassifiesPdoAndSdoAndExposesSource) {
  DeviceManager dm;
  setUp(dm);
  MonitoringManager manager(dm);

  ASSERT_TRUE(manager.create(axisConfig()).has_value());
  EXPECT_EQ(manager.monitoringCount(), 1u);
  EXPECT_EQ(manager.polledSdoCount(), 1u);  // only the temperature object is polled

  auto resource = manager.get("axis");
  ASSERT_TRUE(resource.has_value());
  ASSERT_EQ((*resource)["parameters"].size(), 2u);
  EXPECT_EQ((*resource)["parameters"][0]["source"], "pdo");  // actual position
  EXPECT_EQ((*resource)["parameters"][1]["source"], "sdo");  // temperature
}

TEST(MonitoringManagerTest, CreateRejectsInvalidConfigs) {
  DeviceManager dm;
  setUp(dm);
  MonitoringManager manager(dm);

  auto bad = [&](auto mutate) {
    Monitoring m = axisConfig();
    mutate(m);
    return manager.create(m).has_value();
  };
  EXPECT_FALSE(bad([](Monitoring& m) { m.topic = "bad/topic"; }));  // not URL-safe
  EXPECT_FALSE(bad([](Monitoring& m) { m.topic = "pdos"; }));       // reserved
  EXPECT_FALSE(bad([](Monitoring& m) { m.interval = std::chrono::milliseconds{5}; }));  // < 10 ms
  EXPECT_FALSE(bad([](Monitoring& m) { m.interval = std::chrono::milliseconds{2000}; }));  // > 1 s
  EXPECT_FALSE(bad([](Monitoring& m) { m.parameters.clear(); }));                          // empty
  EXPECT_FALSE(bad([](Monitoring& m) {
    m.parameters = {MonitoredParameter{1, 0x9999, 0x00}};  // neither PDO-mapped nor in OD
  }));
  EXPECT_EQ(manager.monitoringCount(), 0u);  // nothing registered by the failures

  ASSERT_TRUE(manager.create(axisConfig()).has_value());
  EXPECT_FALSE(manager.create(axisConfig()).has_value());  // duplicate topic
}

TEST(MonitoringManagerTest, FlushPublishesEveryRecordedCycleAsPositionalRows) {
  DeviceManager dm;
  setUp(dm);
  // Seed the temperature cache (OP → online → cached + downloaded) to a known value.
  ASSERT_TRUE(dm.writeDeviceParameter(1, 0x2030, 0x01, mm::node::DeviceParameterValue{uint32_t{42}})
                  .has_value());

  MonitoringManager manager(dm);
  std::vector<nlohmann::json> published;
  manager.setPublish([&](std::string /*topic*/, std::string json) {
    published.push_back(nlohmann::json::parse(json));
  });
  ASSERT_TRUE(manager.create(axisConfig()).has_value());

  manager.sampleAll();  // first flush primes the cursor at the current head — no rows yet
  EXPECT_TRUE(published.empty());

  drive(dm, 16);        // record 16 cycles
  manager.sampleAll();  // flush delivers every recorded cycle since the cursor: 16 rows

  ASSERT_EQ(published.size(), 1u);  // one lossless batch of every cycle since the last flush
  const auto& env = published[0];
  EXPECT_EQ(env["type"], "monitoring");
  EXPECT_EQ(env["topic"], "axis");
  ASSERT_EQ(env["data"].size(), 16u);
  const auto& row = env["data"][0];
  ASSERT_EQ(row.size(), 3u);      // [ts(µs), actualPosition, temperature]
  EXPECT_EQ(row[1], 0x11223344);  // actual position decoded from the cycle's recorded input image
  EXPECT_EQ(row[2], 42);          // temperature from the cache

  drive(dm, 4);         // record 4 more cycles
  manager.sampleAll();  // next flush delivers only those 4 (cursor advanced)
  ASSERT_EQ(published.size(), 2u);
  EXPECT_EQ(published[1]["data"].size(), 4u);

  manager.sampleAll();  // nothing recorded since — no message
  EXPECT_EQ(published.size(), 2u);
}

TEST(MonitoringManagerTest, NonExchangingDeviceSamplesNull) {
  DeviceManager dm;
  FakeBus* bus = setUp(dm);
  bus->state = static_cast<uint16_t>(EtherCatState::PreOp);  // not exchanging → live gate closes

  MonitoringManager manager(dm);
  std::vector<nlohmann::json> published;
  manager.setPublish(
      [&](std::string, std::string json) { published.push_back(nlohmann::json::parse(json)); });
  ASSERT_TRUE(manager.create(axisConfig()).has_value());

  manager.sampleAll();  // prime the cursor
  drive(dm, 16);        // cycles still record (image is published); only the live gate is closed
  manager.sampleAll();  // flush

  ASSERT_EQ(published.size(), 1u);
  const auto& row = published[0]["data"][0];
  EXPECT_TRUE(row[1].is_null());  // PDO value absent (not exchanging)
  EXPECT_TRUE(row[2].is_null());  // SDO value absent (not exchanging)
}

TEST(MonitoringManagerTest, RemoveReleasesSdoAndForgetsMonitoring) {
  DeviceManager dm;
  setUp(dm);
  MonitoringManager manager(dm);
  ASSERT_TRUE(manager.create(axisConfig()).has_value());
  EXPECT_EQ(manager.polledSdoCount(), 1u);

  EXPECT_TRUE(manager.remove("axis"));
  EXPECT_EQ(manager.monitoringCount(), 0u);
  EXPECT_EQ(manager.polledSdoCount(), 0u);  // temperature no longer polled
  EXPECT_FALSE(manager.get("axis").has_value());
  EXPECT_FALSE(manager.remove("axis"));  // already gone
}

TEST(MonitoringManagerTest, SchedulerThreadSamplesAndPublishes) {
  DeviceManager dm;
  setUp(dm);
  MonitoringManager manager(dm);
  std::atomic<int> publishCount{0};
  manager.setPublish(
      [&](std::string, std::string) { publishCount.fetch_add(1, std::memory_order_relaxed); });

  Monitoring m = axisConfig();
  m.interval = std::chrono::milliseconds{10};  // minimum flush cadence
  ASSERT_TRUE(manager.create(m).has_value());
  manager.start();

  // Drive cycles into the ring (as the RT loop would) and wait, bounded, for the sampler thread to
  // flush at least one batch of recorded cycles.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (publishCount.load(std::memory_order_relaxed) == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    drive(dm, 5);
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  manager.stop();

  EXPECT_GE(publishCount.load(std::memory_order_relaxed), 1);
}

}  // namespace
