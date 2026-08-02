#include "node/somanet_procedures.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "comm/fieldbus_driver.h"
#include "comm/object_data_types.h"
#include "node/cia402.h"
#include "node/device.h"
#include "node/procedure.h"
#include "node/synapticon.h"

namespace {

using mm::comm::EtherCatState;
using mm::comm::FieldbusDriver;
using mm::comm::ObjectDataType;
using mm::comm::OdEntry;
using mm::comm::SlaveInfo;
using mm::node::Device;
using mm::node::kOsCommand;
using mm::node::kOsCommandMode;
using mm::node::kOsCommandStep;
using mm::node::kSynapticonVendorId;
using mm::node::OsCommandRequest;
using mm::node::osCommandSteps;
using mm::node::ProgressReporter;
using mm::node::ProgressStatus;
using mm::node::runOsCommandProcedure;
using mm::node::cia402::Object;

constexpr uint16_t kPreOp = static_cast<uint16_t>(EtherCatState::PreOp);
constexpr auto kNoDelay = std::chrono::milliseconds(0);

/// Drive double answering OS command response reads from a scripted sequence — the same shape the
/// SomanetDrive tests use, reduced to what these procedure tests need.
class OsCommandFakeDriver : public FieldbusDriver {
 public:
  std::vector<OdEntry> ods;
  std::map<uint32_t, std::vector<uint8_t>> store;
  std::vector<std::vector<uint8_t>> responses{{0, 0, 0, 0, 0, 0, 0, 0}};
  int responseReads = 0;
  uint32_t vendorId = kSynapticonVendorId;

  static uint32_t key(uint16_t index, uint8_t subindex) {
    return (static_cast<uint32_t>(index) << 8) | subindex;
  }

  void programObject(uint16_t index, uint8_t subindex, ObjectDataType type,
                     std::vector<uint8_t> initial) {
    OdEntry e{};
    e.index = index;
    e.subindex = subindex;
    e.objectCode = 0x0007;
    e.dataType = static_cast<uint16_t>(type);
    e.bitLength = static_cast<uint16_t>(initial.size() * 8);
    e.access = 0x003F;
    ods.push_back(e);
    store[key(index, subindex)] = std::move(initial);
  }

  void programDrive() {
    programObject(Object::kControlword, 0, ObjectDataType::UNSIGNED16, {0, 0});
    programObject(Object::kStatusword, 0, ObjectDataType::UNSIGNED16, {0, 0});
    programObject(kOsCommand, 1, ObjectDataType::OCTET_STRING, std::vector<uint8_t>(8, 0));
    programObject(kOsCommand, 3, ObjectDataType::OCTET_STRING, std::vector<uint8_t>(8, 0));
    programObject(kOsCommandMode, 0, ObjectDataType::UNSIGNED8, {0});
  }

  std::expected<std::vector<uint8_t>, std::string> readSdo(uint16_t, uint16_t index,
                                                           uint8_t subindex) override {
    if (index == kOsCommand && subindex == 3) {
      const size_t at = std::min(static_cast<size_t>(responseReads), responses.size() - 1);
      ++responseReads;
      return responses[at];
    }
    auto it = store.find(key(index, subindex));
    if (it == store.end()) {
      return std::unexpected("no such object");
    }
    return it->second;
  }

  std::expected<void, std::string> writeSdo(uint16_t, uint16_t index, uint8_t subindex,
                                            std::span<const uint8_t> data) override {
    store[key(index, subindex)] = std::vector<uint8_t>(data.begin(), data.end());
    return {};
  }

  std::expected<std::vector<OdEntry>, std::string> readObjectDictionary(uint16_t) override {
    return ods;
  }
  SlaveInfo slaveInfo(uint16_t) const override {
    SlaveInfo info{};
    info.vendorId = vendorId;
    return info;
  }
  uint16_t slaveState(uint16_t) const override { return kPreOp; }
  uint16_t mailboxProtocols(uint16_t) const override {
    return mm::comm::MailboxConfig::kProtocolCoe;
  }

  // --- unused stubs ---------------------------------------------------------
  std::expected<std::vector<uint8_t>, std::string> readSdoComplete(uint16_t, uint16_t) override {
    return std::unexpected("SDO abort 0x06010000: Unsupported access to an object");
  }
  std::expected<void, std::string> init() override { return {}; }
  std::expected<int, std::string> scan() override { return 0; }
  std::expected<void, std::string> configureProcessData() override { return {}; }
  mm::comm::PdoLayout processDataLayout() override { return {}; }
  int exchangeProcessData(std::span<const uint8_t>, std::span<uint8_t>) override { return 0; }
  void stop() override {}
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
};

Device makeDevice(OsCommandFakeDriver& driver) {
  driver.programDrive();
  Device device(1, driver);
  EXPECT_TRUE(device.initializeParameters().has_value());
  return device;
}

OsCommandRequest makeRequest() {
  return OsCommandRequest{.command = {8, 0, 0, 0, 0, 0, 0, 0},
                          .timeout = std::chrono::milliseconds(1000),
                          .pollInterval = kNoDelay};
}

TEST(RunOsCommandProcedure, RecordsTheResponseAsTheStepValue) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(osCommandSteps());

  driver.responses = {{1, 0, 1, 2, 3, 4, 5, 6}};  // completed with response data
  auto result = runOsCommandProcedure(device, reporter, std::stop_token{}, makeRequest());
  ASSERT_TRUE(result.has_value()) << result.error();

  const auto steps = reporter.steps();
  ASSERT_EQ(steps.size(), 1u);
  EXPECT_EQ(steps[0].id, kOsCommandStep);
  EXPECT_EQ(steps[0].status, ProgressStatus::kSucceeded);
  EXPECT_EQ(steps[0].value["status"], 1);
  EXPECT_EQ(steps[0].value["data"], (std::vector<uint8_t>{1, 2, 3, 4, 5, 6}));
  EXPECT_FALSE(steps[0].value.contains("errorCode"));
}

TEST(RunOsCommandProcedure, FailsTheStepWhenTheDriveReportsAGeneralError) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(osCommandSteps());

  driver.responses = {{3, 0, 254, 0, 0, 0, 0, 0}};  // unsupported command
  auto result = runOsCommandProcedure(device, reporter, std::stop_token{}, makeRequest());
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("unsupported command"), std::string::npos) << result.error();

  const auto steps = reporter.steps();
  EXPECT_EQ(steps[0].status, ProgressStatus::kFailed);
  ASSERT_TRUE(steps[0].error.has_value());
  EXPECT_NE(steps[0].error->find("254"), std::string::npos) << *steps[0].error;
}

TEST(RunOsCommandProcedure, ReportsACommandSpecificCodeWithoutNamingIt) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(osCommandSteps());

  // Codes counting up from 0 mean something different per command, so the raw path reports the
  // number and says so rather than guessing.
  driver.responses = {{3, 0, 0, 0, 0, 0, 0, 0}};
  auto result = runOsCommandProcedure(device, reporter, std::stop_token{}, makeRequest());
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("command-specific"), std::string::npos) << result.error();
}

TEST(RunOsCommandProcedure, FailsTheStepWhenTheCommandTimesOut) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(osCommandSteps());

  driver.responses = {{255, 0, 0, 0, 0, 0, 0, 0}, {3, 0, 252, 0, 0, 0, 0, 0}};
  auto request = makeRequest();
  request.timeout = kNoDelay;
  auto result = runOsCommandProcedure(device, reporter, std::stop_token{}, request);
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("timed out"), std::string::npos) << result.error();
  EXPECT_EQ(reporter.steps()[0].status, ProgressStatus::kFailed);
}

TEST(RunOsCommandProcedure, LeavesTheStepIdleWhenTheDeviceIsNotASomanetDrive) {
  OsCommandFakeDriver driver;
  driver.vendorId = 0x00000539;  // some other vendor
  Device device = makeDevice(driver);
  ProgressReporter reporter(osCommandSteps());

  auto result = runOsCommandProcedure(device, reporter, std::stop_token{}, makeRequest());
  ASSERT_FALSE(result.has_value());
  // The command never started, so nothing is reported against the step — the run-level error says
  // what was wrong with the device.
  EXPECT_EQ(reporter.steps()[0].status, ProgressStatus::kIdle);
  EXPECT_EQ(driver.responseReads, 0);
}

}  // namespace
