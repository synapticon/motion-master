#include "node/profile_procedures.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "comm/fieldbus_driver.h"
#include "comm/object_data_types.h"
#include "node/device.h"
#include "node/procedure.h"

namespace {

using mm::comm::EtherCatState;
using mm::comm::FieldbusDriver;
using mm::comm::ObjectDataType;
using mm::comm::OdEntry;
using mm::comm::SlaveInfo;
using mm::node::Device;
using mm::node::parseRestoreDefaultParametersRequest;
using mm::node::ProgressReporter;
using mm::node::ProgressStatus;
using mm::node::RestoreDefaultParametersConfig;
using mm::node::restoreDefaultParametersParameters;
using mm::node::RestoreDefaultParametersRequest;
using mm::node::restoreDefaultParametersSteps;
using mm::node::RestoreGroup;
using mm::node::runRestoreDefaultParametersProcedure;
using mm::node::runStoreParametersProcedure;
using mm::node::StoreParametersConfig;
using mm::node::storeParametersSteps;

constexpr uint16_t kPreOp = static_cast<uint16_t>(EtherCatState::PreOp);

// Zero settle and interval, so the confirmation walk runs without real waiting — the reason the
// bodies take a config at all.
constexpr auto kNoDelay = std::chrono::milliseconds(0);

constexpr uint16_t kDeviceType = 0x1000;
constexpr uint16_t kStoreParameters = 0x1010;
constexpr uint16_t kRestoreDefaultParameters = 0x1011;

constexpr uint32_t kSaveSignature = 0x65766173;  // ASCII "save", little-endian
constexpr uint32_t kLoadSignature = 0x64616F6C;  // ASCII "load", little-endian

std::vector<uint8_t> u32le(uint32_t v) {
  return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
          static_cast<uint8_t>(v >> 24)};
}

/// Driver double for the "write a signature, poll until it reads back 1" command objects: it
/// records what was written to the command sub-entry and answers reads of it as configured.
class CommandFakeDriver : public FieldbusDriver {
 public:
  std::map<uint32_t, std::vector<uint8_t>> store;
  std::vector<OdEntry> ods;

  uint16_t cmdIndex = kStoreParameters;
  uint8_t cmdSubindex = 1;

  int writes = 0;              ///< Signature writes to the command sub-entry.
  uint32_t lastSignature = 0;  ///< Last value written to it.
  bool neverConfirm = false;   ///< When set, it always reads back 0.

  static uint32_t key(uint16_t index, uint8_t subindex) {
    return (static_cast<uint32_t>(index) << 8) | subindex;
  }

  void programObject(uint16_t index, uint8_t subindex, ObjectDataType type,
                     std::vector<uint8_t> initial) {
    OdEntry e{};
    e.index = index;
    e.subindex = subindex;
    e.objectCode = 0x0007;  // OTYPE_VAR
    e.dataType = static_cast<uint16_t>(type);
    e.bitLength = static_cast<uint16_t>(initial.size() * 8);
    e.access = 0x003F;  // readable + writable in every AL state
    ods.push_back(e);
    store[key(index, subindex)] = std::move(initial);
  }

  // The mandatory device type (0x1000), so a ProfileDevice binds, plus the command sub-entry.
  void programCommandObject(uint16_t index, uint8_t subindex) {
    cmdIndex = index;
    cmdSubindex = subindex;
    programObject(kDeviceType, 0, ObjectDataType::UNSIGNED32, u32le(0x00020192));  // 402 profile
    programObject(index, subindex, ObjectDataType::UNSIGNED32, u32le(0));
  }

  std::expected<std::vector<uint8_t>, std::string> readSdo(uint16_t, uint16_t index,
                                                           uint8_t subindex) override {
    if (index == cmdIndex && subindex == cmdSubindex) {
      return u32le(neverConfirm ? 0u : 1u);
    }
    auto it = store.find(key(index, subindex));
    if (it == store.end()) {
      return std::unexpected("no such object");
    }
    return it->second;
  }

  std::expected<void, std::string> writeSdo(uint16_t, uint16_t index, uint8_t subindex,
                                            std::span<const uint8_t> data) override {
    if (index == cmdIndex && subindex == cmdSubindex) {
      ++writes;
      if (data.size() >= 4) {
        lastSignature = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                        (static_cast<uint32_t>(data[2]) << 16) |
                        (static_cast<uint32_t>(data[3]) << 24);
      }
    }
    store[key(index, subindex)] = std::vector<uint8_t>(data.begin(), data.end());
    return {};
  }

  std::expected<std::vector<OdEntry>, std::string> readObjectDictionary(uint16_t) override {
    return ods;
  }

  // --- unused stubs ---------------------------------------------------------
  std::expected<std::vector<uint8_t>, std::string> readSdoComplete(uint16_t, uint16_t) override {
    return std::unexpected("SDO abort 0x06010000: Unsupported access to an object");
  }
  std::expected<void, std::string> init() override { return {}; }
  std::expected<int, std::string> scan() override { return 0; }
  SlaveInfo slaveInfo(uint16_t) const override { return SlaveInfo{}; }
  uint16_t slaveState(uint16_t) const override { return kPreOp; }
  uint16_t mailboxProtocols(uint16_t) const override {
    return mm::comm::MailboxConfig::kProtocolCoe;
  }
  std::expected<void, std::string> configureProcessData() override { return {}; }
  mm::comm::PdoLayout processDataLayout() override { return {}; }
  int exchangeProcessData(std::span<const uint8_t>, std::span<uint8_t>) override { return 0; }
  void stop() override {}
  std::expected<std::vector<SlaveStateRaw>, std::string> readStates(
      const std::vector<uint16_t>& positions) override {
    return std::vector<SlaveStateRaw>(positions.size(), SlaveStateRaw{});
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

Device makeDevice(CommandFakeDriver& driver, uint16_t index, uint8_t subindex) {
  driver.programCommandObject(index, subindex);
  Device device(1, driver);
  auto initialized = device.initializeParameters();
  EXPECT_TRUE(initialized.has_value()) << initialized.error();
  return device;
}

StoreParametersConfig instantStore() {
  StoreParametersConfig config;
  config.settle = kNoDelay;
  config.interval = kNoDelay;
  return config;
}

RestoreDefaultParametersConfig instantRestore() {
  RestoreDefaultParametersConfig config;
  config.settle = kNoDelay;
  config.interval = kNoDelay;
  return config;
}

// --- store parameters -----------------------------------------------------------------------

TEST(StoreParametersSteps, DeclaresOneStep) {
  const auto steps = storeParametersSteps();
  ASSERT_EQ(steps.size(), 1u);
  EXPECT_EQ(steps[0].status, ProgressStatus::kIdle);
}

TEST(RunStoreParametersProcedure, WritesTheSaveSignatureAndSucceedsOnConfirmation) {
  CommandFakeDriver driver;
  Device device = makeDevice(driver, kStoreParameters, 1);
  ProgressReporter reporter(storeParametersSteps());

  auto result = runStoreParametersProcedure(device, reporter, std::stop_token{}, instantStore());
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(driver.writes, 1);
  EXPECT_EQ(driver.lastSignature, kSaveSignature);
  EXPECT_EQ(reporter.steps()[0].status, ProgressStatus::kSucceeded);
}

TEST(RunStoreParametersProcedure, FailsTheStepWhenTheDeviceNeverConfirms) {
  CommandFakeDriver driver;
  Device device = makeDevice(driver, kStoreParameters, 1);
  driver.neverConfirm = true;
  ProgressReporter reporter(storeParametersSteps());

  auto config = instantStore();
  config.retries = 2;
  auto result = runStoreParametersProcedure(device, reporter, std::stop_token{}, config);
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("not confirmed"), std::string::npos) << result.error();
  EXPECT_EQ(reporter.steps()[0].status, ProgressStatus::kFailed);
}

TEST(RunStoreParametersProcedure, CancellingAbandonsTheWaitButTheCommandWasAlreadyWritten) {
  // The distinction the procedure has to report honestly: by the time a run can be cancelled the
  // signature is already out, so the device may store anyway. A cancel stops the master waiting.
  CommandFakeDriver driver;
  Device device = makeDevice(driver, kStoreParameters, 1);
  driver.neverConfirm = true;
  ProgressReporter reporter(storeParametersSteps());

  std::stop_source source;
  source.request_stop();
  auto result = runStoreParametersProcedure(device, reporter, source.get_token(), instantStore());
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("cancelled"), std::string::npos) << result.error();
  EXPECT_EQ(driver.writes, 1);
}

TEST(RunStoreParametersProcedure, LeavesTheStepIdleWhenTheDeviceHasNoGenericArea) {
  CommandFakeDriver driver;
  Device device(1, driver);  // nothing enumerated — 0x1000 absent
  ProgressReporter reporter(storeParametersSteps());

  auto result = runStoreParametersProcedure(device, reporter, std::stop_token{}, instantStore());
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(reporter.steps()[0].status, ProgressStatus::kIdle);
  EXPECT_EQ(driver.writes, 0);
}

// --- restore default parameters -------------------------------------------------------------

TEST(ParseRestoreDefaultParametersRequest, DefaultsToAll) {
  auto request = parseRestoreDefaultParametersRequest(nlohmann::json::object());
  ASSERT_TRUE(request.has_value()) << request.error();
  EXPECT_EQ(request->group, RestoreGroup::kAll);
}

TEST(ParseRestoreDefaultParametersRequest, AcceptsEveryGroupToken) {
  const std::pair<const char*, RestoreGroup> cases[] = {
      {"all", RestoreGroup::kAll},
      {"communication", RestoreGroup::kCommunication},
      {"application", RestoreGroup::kApplication},
      {"manufacturer", RestoreGroup::kManufacturer},
  };
  for (const auto& [token, group] : cases) {
    auto request = parseRestoreDefaultParametersRequest(nlohmann::json{{"group", token}});
    ASSERT_TRUE(request.has_value()) << token << ": " << request.error();
    EXPECT_EQ(request->group, group) << token;
  }
}

TEST(ParseRestoreDefaultParametersRequest, RejectsAnUnknownGroup) {
  auto request = parseRestoreDefaultParametersRequest(nlohmann::json{{"group", "everything"}});
  ASSERT_FALSE(request.has_value());
  EXPECT_NE(request.error().find("must be one of"), std::string::npos) << request.error();
}

TEST(ParseRestoreDefaultParametersRequest, RejectsAGroupThatIsNotAString) {
  auto request = parseRestoreDefaultParametersRequest(nlohmann::json{{"group", 1}});
  ASSERT_FALSE(request.has_value());
  EXPECT_NE(request.error().find("must be a string"), std::string::npos) << request.error();
}

TEST(RestoreDefaultParametersParameters, DescribeTheGroupChoiceTheParserAccepts) {
  const auto parameters = restoreDefaultParametersParameters();
  ASSERT_EQ(parameters.size(), 1u);
  EXPECT_EQ(parameters[0].name, "group");
  EXPECT_FALSE(parameters[0].required());
  EXPECT_EQ(parameters[0].defaultValue, "all");
  // Every option must be a token the parser knows, or a client could offer a choice that 400s.
  ASSERT_EQ(parameters[0].options.size(), 4u);
  for (const auto& option : parameters[0].options) {
    auto request = parseRestoreDefaultParametersRequest(nlohmann::json{{"group", option.value}});
    EXPECT_TRUE(request.has_value()) << option.value.dump();
  }
}

TEST(RunRestoreDefaultParametersProcedure, WritesLoadToTheSubEntryTheGroupSelects) {
  // The group enum value is the 0x1011 sub-entry, so "application" must reach :03 and no other.
  CommandFakeDriver driver;
  Device device = makeDevice(driver, kRestoreDefaultParameters, 3);
  ProgressReporter reporter(restoreDefaultParametersSteps());

  auto result = runRestoreDefaultParametersProcedure(
      device, reporter, std::stop_token{},
      RestoreDefaultParametersRequest{.group = RestoreGroup::kApplication}, instantRestore());
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(driver.writes, 1);
  EXPECT_EQ(driver.lastSignature, kLoadSignature);
}

TEST(RunRestoreDefaultParametersProcedure, RecordsWhichGroupWasRestored) {
  // A snapshot read afterwards has to say what was restored; "restore succeeded" alone would not.
  CommandFakeDriver driver;
  Device device = makeDevice(driver, kRestoreDefaultParameters, 2);
  ProgressReporter reporter(restoreDefaultParametersSteps());

  auto result = runRestoreDefaultParametersProcedure(
      device, reporter, std::stop_token{},
      RestoreDefaultParametersRequest{.group = RestoreGroup::kCommunication}, instantRestore());
  ASSERT_TRUE(result.has_value()) << result.error();

  const auto steps = reporter.steps();
  EXPECT_EQ(steps[0].status, ProgressStatus::kSucceeded);
  EXPECT_EQ(steps[0].value["group"], "communication");
}

}  // namespace
