#include "node/device_manager.h"

#include <gtest/gtest.h>

#include <memory>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "comm/fieldbus_driver.h"
#include "node/synapticon.h"

namespace {

using mm::comm::EtherCatState;
using mm::comm::FieldbusDriver;
using mm::comm::OdEntry;
using mm::comm::SlaveDiagnostics;
using mm::comm::SlaveInfo;
using mm::node::DeviceManager;
using mm::node::DeviceParameterValue;

/// Minimal FieldbusDriver test double. init() returns a configurable result;
/// scan() reports a configurable slave count. Every other method is a trivial
/// stub — the lifecycle tests below never reach them.
class FakeDriver : public FieldbusDriver {
 public:
  explicit FakeDriver(bool initSucceeds, int slaves = 1)
      : initSucceeds_(initSucceeds), slaves_(slaves) {}

  /// AL Status reported by readStates() for every queried slave (bits 3:0 = state).
  uint16_t reportState = 0;

  /// Name returned by slaveInfo() for every position (drives Device::name()).
  std::string deviceName;

  /// Vendor/product returned by slaveInfo() (drive Device::vendorId()/productCode()/productName()).
  uint32_t vendorId = 0;
  uint32_t productCode = 0;

  /// Returned verbatim by busConfig() — empty unless a test populates it.
  std::vector<mm::comm::SlaveConfig> busConfigData;

  /// Returned verbatim by readDiagnostics() — empty unless a test populates it.
  std::vector<mm::comm::SlaveDiagnostics> diagnosticsData;

  /// Returned verbatim by readDcSync() — empty unless a test populates it.
  std::vector<mm::comm::DcSyncDiagnostics> dcSyncData;

  /// Returned by processDataWatchdog(); also the echo base for setProcessDataWatchdog().
  mm::comm::ProcessDataWatchdogConfig watchdogConfig{.enabled = true,
                                                     .running = true,
                                                     .timeout = std::chrono::milliseconds(100),
                                                     .divider = 2498,
                                                     .ticks = 1000};

  /// Records the last setProcessDataWatchdog() call so tests can assert the forwarded arguments.
  uint16_t lastWatchdogPosition = 0;
  std::chrono::nanoseconds lastWatchdogTimeout{0};

  std::expected<void, std::string> init() override {
    if (!initSucceeds_) {
      return std::unexpected("fake init failure");
    }
    return {};
  }

  std::expected<int, std::string> scan() override { return slaves_; }

  SlaveInfo slaveInfo(uint16_t) const override {
    return {.name = deviceName,
            .vendorId = vendorId,
            .productCode = productCode,
            .revisionNumber = 0,
            .serialNumber = 0};
  }
  std::expected<void, std::string> configureProcessData() override { return {}; }
  mm::comm::PdoLayout processDataLayout() override { return {}; }
  std::vector<mm::comm::SlaveConfig> busConfig() const override { return busConfigData; }
  int exchangeProcessData(std::span<const uint8_t>, std::span<uint8_t>) override { return 0; }
  void stop() override {}

  std::expected<std::vector<SlaveStateRaw>, std::string> readStates(
      const std::vector<uint16_t>& positions) override {
    // Mimic ecx_readstate: a read refreshes the cached state that slaveState() returns.
    cachedState_ = reportState;
    return std::vector<SlaveStateRaw>(positions.size(),
                                      SlaveStateRaw{.alStatus = reportState, .alStatusCode = 0});
  }

  uint16_t slaveState(uint16_t) const override { return cachedState_; }
  // Emulated SOMANET drives all speak CoE, so their PDO mapping is read over the mailbox
  // (not from SII) — the CoE-mapping reads these tests program are what drive readFlatPdoMapping.
  uint16_t mailboxProtocols(uint16_t) const override {
    return mm::comm::MailboxConfig::kProtocolCoe;
  }

  std::expected<std::vector<SlaveDiagnostics>, std::string> readDiagnostics(
      const std::vector<uint16_t>&) override {
    return diagnosticsData;
  }

  std::expected<std::vector<mm::comm::DcSyncDiagnostics>, std::string> readDcSync(
      const std::vector<uint16_t>&) override {
    return dcSyncData;
  }

  std::expected<std::vector<uint8_t>, std::string> readSdo(uint16_t, uint16_t, uint8_t) override {
    return std::vector<uint8_t>{};
  }

  std::expected<void, std::string> writeSdo(uint16_t, uint16_t, uint8_t,
                                            std::span<const uint8_t>) override {
    return {};
  }

  std::expected<std::vector<OdEntry>, std::string> readObjectDictionary(uint16_t) override {
    return std::vector<OdEntry>{};
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

  std::expected<mm::comm::ProcessDataWatchdogConfig, std::string> processDataWatchdog(
      uint16_t) override {
    return watchdogConfig;
  }

  std::expected<mm::comm::ProcessDataWatchdogConfig, std::string> setProcessDataWatchdog(
      uint16_t slavePosition, std::chrono::nanoseconds timeout) override {
    lastWatchdogPosition = slavePosition;
    lastWatchdogTimeout = timeout;
    // Echo the request as the "programmed" config, mimicking a driver that achieved it exactly.
    watchdogConfig.timeout = timeout;
    watchdogConfig.enabled = timeout != std::chrono::nanoseconds::zero();
    return watchdogConfig;
  }

  void transitionToState(const std::vector<uint16_t>&, std::optional<EtherCatState>, EtherCatState,
                         std::chrono::steady_clock::duration, std::chrono::steady_clock::duration,
                         std::function<void()>, std::function<bool()>) override {}

 private:
  bool initSucceeds_;
  int slaves_;
  uint16_t cachedState_ = 0;  // refreshed by readStates(); returned by slaveState()
};

TEST(DeviceManagerInit, SuccessfulInitMarksInitialised) {
  DeviceManager dm;
  EXPECT_FALSE(dm.initialised());

  auto result = dm.init(std::make_unique<FakeDriver>(true));
  EXPECT_TRUE(result.has_value());
  EXPECT_TRUE(dm.initialised());
}

TEST(DeviceManagerInit, FailedInitLeavesUninitialised) {
  DeviceManager dm;

  auto result = dm.init(std::make_unique<FakeDriver>(false));
  ASSERT_FALSE(result.has_value());
  // The half-dead driver must be dropped — otherwise initialised() lies and the
  // next control-plane call dereferences a context that never opened.
  EXPECT_FALSE(dm.initialised());
}

TEST(DeviceManagerInit, ScanAfterFailedInitReturnsErrorNotCrash) {
  DeviceManager dm;
  (void)dm.init(std::make_unique<FakeDriver>(false));

  // Regression: previously scan() ran on a retained driver whose context was
  // null and segfaulted. It must now fail cleanly.
  auto scan = dm.scan();
  EXPECT_FALSE(scan.has_value());
}

TEST(DeviceManagerInit, InitCanBeRetriedAfterFailureWithoutReset) {
  DeviceManager dm;
  (void)dm.init(std::make_unique<FakeDriver>(false));
  ASSERT_FALSE(dm.initialised());

  auto retry = dm.init(std::make_unique<FakeDriver>(true));
  EXPECT_TRUE(retry.has_value());
  EXPECT_TRUE(dm.initialised());
}

TEST(DeviceManagerInit, SecondInitWhileInitialisedIsRejected) {
  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::make_unique<FakeDriver>(true)).has_value());

  // One-shot contract: re-init must be rejected (caller must reset() first) so a
  // live driver is never replaced out from under the Devices referencing it.
  auto again = dm.init(std::make_unique<FakeDriver>(true));
  EXPECT_FALSE(again.has_value());
  EXPECT_TRUE(dm.initialised());

  dm.reset();
  EXPECT_FALSE(dm.initialised());
  EXPECT_TRUE(dm.init(std::make_unique<FakeDriver>(true)).has_value());
}

TEST(DeviceManagerMailbox, PreOpStateMarksMailboxActive) {
  auto driver = std::make_unique<FakeDriver>(true, 1);
  driver->reportState = static_cast<uint16_t>(EtherCatState::PreOp);
  FakeDriver* raw = driver.get();  // dm keeps ownership; raw stays valid

  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::move(driver)).has_value());
  ASSERT_TRUE(dm.scan().has_value());

  // Devices start with no mailbox until a state read establishes availability.
  ASSERT_NE(dm.findDevice(1), nullptr);
  EXPECT_FALSE(dm.findDevice(1)->mailboxActive());

  ASSERT_TRUE(dm.deviceStates({}).has_value());
  EXPECT_TRUE(dm.findDevice(1)->mailboxActive());

  // Dropping back to INIT must flip the mailbox inactive again.
  raw->reportState = static_cast<uint16_t>(EtherCatState::Init);
  ASSERT_TRUE(dm.deviceStates({}).has_value());
  EXPECT_FALSE(dm.findDevice(1)->mailboxActive());
}

TEST(DeviceManagerMailbox, ErrorIndicatorDoesNotDisableMailbox) {
  // A device in SAFE-OP with the AL error bit set still answers mailbox requests, so the
  // mailbox must report active — the error is surfaced separately via the AL status.
  auto driver = std::make_unique<FakeDriver>(true, 1);
  driver->reportState =
      static_cast<uint16_t>(EtherCatState::SafeOp) | 0x0010u;  // bit 4 = AL error indicator

  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::move(driver)).has_value());
  ASSERT_TRUE(dm.scan().has_value());
  ASSERT_TRUE(dm.deviceStates({}).has_value());

  ASSERT_NE(dm.findDevice(1), nullptr);
  EXPECT_TRUE(dm.findDevice(1)->mailboxActive());
}

TEST(DeviceManagerDelegates, UnknownDeviceErrors) {
  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::make_unique<FakeDriver>(true, 1)).has_value());
  ASSERT_TRUE(dm.scan().has_value());

  // Position 99 does not exist — the find-and-delegate path must report it, not crash.
  EXPECT_FALSE(dm.readDeviceParameter(99, 0x6064, 0x00).has_value());
  EXPECT_FALSE(
      dm.writeDeviceParameter(99, 0x6064, 0x00, DeviceParameterValue{uint32_t{1}}).has_value());
  EXPECT_FALSE(dm.deviceParameterView(99, 0x6064, 0x00, /*refreshFromBus=*/true).has_value());
  EXPECT_FALSE(dm.deviceParameterView(99, 0x6064, 0x00, /*refreshFromBus=*/false).has_value());
}

TEST(DeviceManagerStageOutputs, ReportsPerItemWithoutFailingWholeBatch) {
  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::make_unique<FakeDriver>(true, 1)).has_value());
  ASSERT_TRUE(dm.scan().has_value());

  std::vector<mm::node::OutputStageRequest> reqs = {
      {.slavePosition = 99,  // unknown device
       .index = 0x6040,
       .subindex = 0x00,
       .value = DeviceParameterValue{uint16_t{15}}},
      {.slavePosition = 1,  // known device, but OD not enumerated and no image published
       .index = 0x607A,
       .subindex = 0x00,
       .value = DeviceParameterValue{int32_t{5000}}}};
  auto results = dm.stageProcessDataOutputs(reqs);

  // One result per request, in order — the batch never fails as a whole.
  ASSERT_EQ(results.size(), reqs.size());

  // Unknown device: not staged, echoes the request, error names the position.
  EXPECT_EQ(results[0].slavePosition, 99);
  EXPECT_FALSE(results[0].staged);
  EXPECT_NE(results[0].error.find("not found"), std::string::npos);

  // Known device but nothing to stage into (no image, parameter not enumerated): not staged, with a
  // non-empty reason, and the identity is echoed back.
  EXPECT_EQ(results[1].slavePosition, 1);
  EXPECT_EQ(results[1].index, 0x607A);
  EXPECT_FALSE(results[1].staged);
  EXPECT_FALSE(results[1].error.empty());
}

TEST(DeviceManagerStageOutputs, EmptyBatchYieldsEmptyResults) {
  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::make_unique<FakeDriver>(true, 1)).has_value());
  ASSERT_TRUE(dm.scan().has_value());

  auto results = dm.stageProcessDataOutputs({});
  EXPECT_TRUE(results.empty());
}

TEST(DeviceManagerWatchdog, GetReturnsDriverConfig) {
  auto driver = std::make_unique<FakeDriver>(true, 1);
  driver->watchdogConfig = {.enabled = true,
                            .running = true,
                            .timeout = std::chrono::milliseconds(200),
                            .divider = 2498,
                            .ticks = 2000};
  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::move(driver)).has_value());
  ASSERT_TRUE(dm.scan().has_value());

  auto wd = dm.processDataWatchdog(1);
  ASSERT_TRUE(wd.has_value());
  EXPECT_TRUE(wd->enabled);
  EXPECT_EQ(wd->timeout, std::chrono::milliseconds(200));
  EXPECT_EQ(wd->ticks, 2000);
}

TEST(DeviceManagerWatchdog, SetForwardsPositionAndTimeout) {
  auto driver = std::make_unique<FakeDriver>(true, 2);
  FakeDriver* raw = driver.get();
  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::move(driver)).has_value());
  ASSERT_TRUE(dm.scan().has_value());

  auto result = dm.setProcessDataWatchdog(2, std::chrono::milliseconds(250));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(raw->lastWatchdogPosition, 2);
  EXPECT_EQ(raw->lastWatchdogTimeout, std::chrono::milliseconds(250));
  EXPECT_TRUE(result->enabled);
  EXPECT_EQ(result->timeout, std::chrono::milliseconds(250));

  // A zero timeout disables the watchdog.
  auto disabled = dm.setProcessDataWatchdog(2, std::chrono::nanoseconds::zero());
  ASSERT_TRUE(disabled.has_value());
  EXPECT_FALSE(disabled->enabled);
}

TEST(DeviceManagerWatchdog, UnknownDeviceAndNoDriverError) {
  DeviceManager dm;
  // Before init there is no driver — both accessors must report it, not crash.
  EXPECT_FALSE(dm.processDataWatchdog(1).has_value());
  EXPECT_FALSE(dm.setProcessDataWatchdog(1, std::chrono::milliseconds(100)).has_value());

  ASSERT_TRUE(dm.init(std::make_unique<FakeDriver>(true, 1)).has_value());
  ASSERT_TRUE(dm.scan().has_value());
  // Position 99 does not exist.
  EXPECT_FALSE(dm.processDataWatchdog(99).has_value());
  EXPECT_FALSE(dm.setProcessDataWatchdog(99, std::chrono::milliseconds(100)).has_value());
}

TEST(DeviceManagerMailbox, InitStateKeepsMailboxInactive) {
  auto driver = std::make_unique<FakeDriver>(true, 1);
  driver->reportState = static_cast<uint16_t>(EtherCatState::Init);

  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::move(driver)).has_value());
  ASSERT_TRUE(dm.scan().has_value());
  ASSERT_TRUE(dm.deviceStates({}).has_value());

  ASSERT_NE(dm.findDevice(1), nullptr);
  EXPECT_FALSE(dm.findDevice(1)->mailboxActive());
}

TEST(DeviceManagerBusConfig, EnrichesDriverConfigWithDeviceName) {
  auto driver = std::make_unique<FakeDriver>(true, 1);
  driver->deviceName = "Axis A";
  mm::comm::SlaveConfig cfg{};
  cfg.slavePosition = 1;
  cfg.configuredAddress = 0x1001;
  cfg.outputBits = 96;
  cfg.inputBits = 128;
  cfg.syncManagers.push_back(mm::comm::SyncManagerConfig{
      .index = 2, .physicalStart = 0x1100, .length = 12, .flags = 0x10024, .type = 3});
  cfg.fmmus.push_back(mm::comm::FmmuConfig{.index = 0,
                                           .logicalStart = 0,
                                           .length = 12,
                                           .logicalStartBit = 0,
                                           .logicalEndBit = 7,
                                           .physicalStart = 0x1100,
                                           .physicalStartBit = 0,
                                           .type = 1,
                                           .active = 1});
  driver->busConfigData = {cfg};

  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::move(driver)).has_value());
  ASSERT_TRUE(dm.scan().has_value());

  // busConfig() passes the driver's per-slave config through and attaches the device name.
  auto config = dm.busConfig();
  ASSERT_EQ(config.size(), 1u);
  EXPECT_EQ(config[0].deviceName, "Axis A");
  // Foreign/zero vendor: productName falls back to the SII name.
  EXPECT_EQ(config[0].productName, "Axis A");
  EXPECT_EQ(config[0].config.slavePosition, 1);
  ASSERT_EQ(config[0].config.syncManagers.size(), 1u);
  EXPECT_EQ(config[0].config.syncManagers[0].type, 3);
  ASSERT_EQ(config[0].config.fmmus.size(), 1u);
  EXPECT_EQ(config[0].config.fmmus[0].type, 1);

  // to_json exposes the resolved name and the nested SM/FMMU arrays.
  nlohmann::json j = config[0];
  EXPECT_EQ(j.at("deviceName"), "Axis A");
  EXPECT_EQ(j.at("productName"), "Axis A");
  ASSERT_EQ(j.at("syncManagers").size(), 1u);
  EXPECT_EQ(j.at("syncManagers")[0].at("type"), 3);
  EXPECT_EQ(j.at("fmmus")[0].at("active"), true);
}

TEST(DeviceManagerBusConfig, ResolvesSomanetProductName) {
  auto driver = std::make_unique<FakeDriver>(true, 1);
  driver->deviceName = "SOMANET";  // the generic SII name every SOMANET drive reports
  driver->vendorId = mm::node::kSynapticonVendorId;
  driver->productCode = 0x00000301;  // SOMANET Circulo
  mm::comm::SlaveConfig cfg{};
  cfg.slavePosition = 1;
  driver->busConfigData = {cfg};

  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::move(driver)).has_value());
  ASSERT_TRUE(dm.scan().has_value());

  auto config = dm.busConfig();
  ASSERT_EQ(config.size(), 1u);
  EXPECT_EQ(config[0].deviceName, "SOMANET");
  EXPECT_EQ(config[0].productName, "SOMANET Circulo");
}

TEST(DeviceManagerBusConfig, EmptyWithoutDriver) {
  DeviceManager dm;
  EXPECT_TRUE(dm.busConfig().empty());
}

TEST(DeviceManagerDiagnostics, EnrichesDriverDiagnosticsWithDeviceName) {
  auto driver = std::make_unique<FakeDriver>(true, 1);
  driver->deviceName = "Axis A";
  mm::comm::SlaveDiagnostics diag{};
  diag.slavePosition = 1;
  diag.ports[0] = mm::comm::PortDiagnostics{.linkUp = true,
                                            .loopClosed = false,
                                            .communication = true,
                                            .invalidFrame = 3,
                                            .rxError = 1,
                                            .forwardedError = 0,
                                            .lostLink = 2};
  diag.processingUnitError = 7;
  diag.pdiWatchdog = 5;
  driver->diagnosticsData = {diag};

  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::move(driver)).has_value());
  ASSERT_TRUE(dm.scan().has_value());

  // deviceDiagnostics() passes the driver's per-slave counters through and attaches the name.
  auto result = dm.deviceDiagnostics({});
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->size(), 1u);
  EXPECT_EQ((*result)[0].deviceName, "Axis A");
  EXPECT_EQ((*result)[0].diagnostics.slavePosition, 1);
  EXPECT_EQ((*result)[0].diagnostics.ports[0].invalidFrame, 3);
  EXPECT_EQ((*result)[0].diagnostics.processingUnitError, 7);

  // to_json exposes the resolved name, the per-port array, and the scalar counters.
  nlohmann::json j = (*result)[0];
  EXPECT_EQ(j.at("deviceName"), "Axis A");
  ASSERT_EQ(j.at("ports").size(), 4u);
  EXPECT_EQ(j.at("ports")[0].at("linkUp"), true);
  EXPECT_EQ(j.at("ports")[0].at("lostLink"), 2);
  EXPECT_EQ(j.at("processingUnitError"), 7);
  EXPECT_EQ(j.at("pdiWatchdog"), 5);
}

TEST(DeviceManagerDiagnostics, ErrorsWithoutDriver) {
  DeviceManager dm;
  EXPECT_FALSE(dm.deviceDiagnostics({}).has_value());
}

TEST(DeviceManagerDcSync, EnrichesDriverDcSyncWithDeviceName) {
  auto driver = std::make_unique<FakeDriver>(true, 1);
  driver->deviceName = "Axis A";
  driver->dcSyncData = {mm::comm::DcSyncDiagnostics{.slavePosition = 1,
                                                    .dcCapable = true,
                                                    .referenceClock = true,
                                                    .propagationDelay = 300,
                                                    .systemTimeDifference = -42}};

  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::move(driver)).has_value());
  ASSERT_TRUE(dm.scan().has_value());

  // dcSync() passes the driver's per-slave DC status through and attaches the resolved name.
  auto result = dm.dcSync({});
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->size(), 1u);
  EXPECT_EQ((*result)[0].deviceName, "Axis A");
  EXPECT_EQ((*result)[0].dcSync.slavePosition, 1);
  EXPECT_TRUE((*result)[0].dcSync.referenceClock);
  EXPECT_EQ((*result)[0].dcSync.systemTimeDifference, -42);

  // to_json exposes the resolved name alongside the decoded DC fields.
  nlohmann::json j = (*result)[0];
  EXPECT_EQ(j.at("deviceName"), "Axis A");
  EXPECT_EQ(j.at("dcCapable"), true);
  EXPECT_EQ(j.at("referenceClock"), true);
  EXPECT_EQ(j.at("propagationDelay"), 300);
  EXPECT_EQ(j.at("systemTimeDifference"), -42);
}

TEST(DeviceManagerDcSync, ErrorsWithoutDriver) {
  DeviceManager dm;
  EXPECT_FALSE(dm.dcSync({}).has_value());
}

TEST(DeviceManagerPositions, BulkMethodsRejectUnknownPosition) {
  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::make_unique<FakeDriver>(true, 1)).has_value());
  ASSERT_TRUE(dm.scan().has_value());

  // Only position 1 was discovered. A caller-supplied position outside the device set must be
  // rejected up front (mirroring the single-device 404) — never forwarded to the driver, where
  // it would index a fixed-size slave array out of bounds. Regression for the unvalidated
  // positions path through deviceStates / deviceDiagnostics / dcSync / transitionToState.
  EXPECT_FALSE(dm.deviceStates({99}).has_value());
  EXPECT_FALSE(dm.deviceDiagnostics({99}).has_value());
  EXPECT_FALSE(dm.dcSync({99}).has_value());
  EXPECT_FALSE(
      dm.transitionToState({99}, EtherCatState::PreOp, std::chrono::milliseconds(0)).has_value());

  // A mix of valid and invalid is still rejected — the whole request fails on the unknown one.
  EXPECT_FALSE(dm.deviceStates({1, 99}).has_value());

  // The empty list (all devices) and the known position still succeed.
  EXPECT_TRUE(dm.deviceStates({}).has_value());
  EXPECT_TRUE(dm.deviceStates({1}).has_value());
}

}  // namespace
