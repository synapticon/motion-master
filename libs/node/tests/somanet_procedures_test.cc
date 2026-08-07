#include "node/somanet_procedures.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <ranges>
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
using mm::node::commutationOffsetDetectionSteps;
using mm::node::Device;
using mm::node::encoderRegisterParameters;
using mm::node::EncoderRegisterRequest;
using mm::node::encoderRegisterSteps;
using mm::node::hrdStreamingParameters;
using mm::node::HrdStreamingRequest;
using mm::node::hrdStreamingSteps;
using mm::node::icMuCalibrationModeParameters;
using mm::node::IcMuCalibrationModeRequest;
using mm::node::icMuCalibrationModeSteps;
using mm::node::kEncoderRegisterStep;
using mm::node::kOsCommand;
using mm::node::kOsCommandMode;
using mm::node::kOsCommandStep;
using mm::node::kSynapticonVendorId;
using mm::node::motorPhaseOrderDetectionSteps;
using mm::node::offsetDetectionSteps;
using mm::node::openPhaseDetectionSteps;
using mm::node::OsCommandRequest;
using mm::node::osCommandSteps;
using mm::node::parseEncoderRegisterRequest;
using mm::node::parseHrdStreamingRequest;
using mm::node::parseIcMuCalibrationModeRequest;
using mm::node::phaseInductanceMeasurementSteps;
using mm::node::phaseResistanceMeasurementSteps;
using mm::node::polePairDetectionSteps;
using mm::node::ProgressReporter;
using mm::node::ProgressStatus;
using mm::node::runCommutationOffsetDetectionProcedure;
using mm::node::runEncoderRegisterProcedure;
using mm::node::runHrdStreamingProcedure;
using mm::node::runIcMuCalibrationModeProcedure;
using mm::node::runMotorPhaseOrderDetectionProcedure;
using mm::node::runOffsetDetectionProcedure;
using mm::node::runOpenPhaseDetectionProcedure;
using mm::node::runOsCommandProcedure;
using mm::node::runPhaseInductanceMeasurementProcedure;
using mm::node::runPhaseResistanceMeasurementProcedure;
using mm::node::runPolePairDetectionProcedure;
namespace somanet = mm::node::somanet;
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
  int drainReads = 0;
  bool awaitingCommand = true;
  int commandWrites = 0;
  std::vector<uint8_t> brakeStatusWrites;
  std::vector<uint8_t> commandIds;
  uint32_t vendorId = kSynapticonVendorId;
  uint16_t statusword = 0x0040;  // SwitchOnDisabled
  std::vector<std::pair<uint16_t, uint8_t>> writeLog;

  // Faults the drive once this many commands have been issued, modelling the drive dropping out of
  // Operation Enabled part-way through a sequence — what a real fault or a quick stop does. Zero
  // leaves the drive healthy for the whole run.
  int faultAfterCommands = 0;

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
    programObject(Object::kModeOfOperation, 0, ObjectDataType::INTEGER8, {8});  // CSP, to restore
    programObject(somanet::kBrakeOptions, somanet::kBrakePullVoltage, ObjectDataType::UNSIGNED32,
                  {0, 0, 0, 0});
    programObject(somanet::kBrakeOptions, somanet::kBrakeHoldVoltage, ObjectDataType::UNSIGNED32,
                  {0, 0, 0, 0});
    programObject(somanet::kBrakeOptions, somanet::kBrakePullTime, ObjectDataType::UNSIGNED16,
                  {0, 0});
    programObject(somanet::kBrakeOptions, somanet::kBrakeReleaseStrategy, ObjectDataType::UNSIGNED8,
                  {static_cast<uint8_t>(somanet::BrakeReleaseStrategy::kClutch)});
    programObject(somanet::kBrakeOptions, somanet::kBrakeStatus, ObjectDataType::UNSIGNED8,
                  {static_cast<uint8_t>(somanet::BrakeStatus::kEngaged)});
    // INTEGER8, matching the drive: reading it as unsigned is refused outright.
    programObject(somanet::kCommutationOffsetDetection, somanet::kCommutationOffsetMethod,
                  ObjectDataType::INTEGER8,
                  {static_cast<uint8_t>(somanet::CommutationOffsetMethod::kRotating)});
    // What the drive says about a fault, which a blocked step quotes back.
    programObject(somanet::kErrorReport, somanet::kErrorReportDescription,
                  ObjectDataType::VISIBLE_STRING, {'O', 'V', 'E', 'R', 'C', 'U', 'R', 'R'});
    store[key(Object::kStatusword, 0)] = {0x40, 0x00};
  }

  std::expected<std::vector<uint8_t>, std::string> readSdo(uint16_t, uint16_t index,
                                                           uint8_t subindex) override {
    if (index == kOsCommand && subindex == 3) {
      // A real drive has a response to give only between a command being issued and that response
      // being read: reading it returns the drive to idle. Modelling that is what lets the drain
      // read runOsCommand performs before *every* command avoid eating the next command's scripted
      // reply, which in a composite procedure would hand each command the one after it. drainReads
      // counts those idle reads separately from the polls that follow a command.
      if (awaitingCommand) {
        ++drainReads;
        return std::vector<uint8_t>(8, 0);
      }
      const size_t at = std::min(static_cast<size_t>(responseReads), responses.size() - 1);
      ++responseReads;
      const auto& reply = responses[at];
      if (!reply.empty() && reply[0] <= 3) {  // terminal: the drive is idle again
        awaitingCommand = true;
      }
      return reply;
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
    writeLog.push_back({index, subindex});
    if (index == kOsCommand && subindex == 1) {
      ++commandWrites;
      awaitingCommand = false;
      if (!data.empty()) {
        commandIds.push_back(data[0]);
      }
      if (faultAfterCommands > 0 && commandWrites >= faultAfterCommands) {
        statusword = 0x0008;  // Fault
        store[key(Object::kStatusword, 0)] = {static_cast<uint8_t>(statusword & 0xFF),
                                              static_cast<uint8_t>(statusword >> 8)};
      }
    }
    if (index == somanet::kBrakeOptions && subindex == somanet::kBrakeStatus && !data.empty()) {
      brakeStatusWrites.push_back(data[0]);
    }
    // Model just enough of the CiA402 machine for enable() to converge: a controlword write
    // advances the state and the statusword follows it. Without that the drive never reports
    // OperationEnabled and every enable in a test would sit out its full timeout.
    if (index == Object::kControlword && data.size() >= 2) {
      const uint16_t cw = static_cast<uint16_t>(data[0] | (data[1] << 8));
      const uint16_t cmd = cw & 0x000F;
      if (cmd == 0x0006) {
        statusword = 0x0021;  // ReadyToSwitchOn
      } else if (cmd == 0x0007) {
        statusword = 0x0023;  // SwitchedOn
      } else if (cmd == 0x000F) {
        statusword = 0x0027;  // OperationEnabled
      } else {
        statusword = 0x0040;  // SwitchOnDisabled
      }
      store[key(Object::kStatusword, 0)] = {static_cast<uint8_t>(statusword & 0xFF),
                                            static_cast<uint8_t>(statusword >> 8)};
    }
    return {};
  }

  // Whether index/subindex was written before otherIndex/otherSubindex — the procedure's step order
  // is a correctness property (the brake may only be released once the drive is enabled), so it is
  // asserted rather than assumed.
  bool wroteBefore(uint16_t index, uint8_t subindex, uint16_t otherIndex,
                   uint8_t otherSubindex) const {
    auto at = [this](uint16_t i, uint8_t s) {
      for (size_t k = 0; k < writeLog.size(); ++k) {
        if (writeLog[k].first == i && writeLog[k].second == s) {
          return static_cast<int>(k);
        }
      }
      return -1;
    };
    const int first = at(index, subindex);
    const int second = at(otherIndex, otherSubindex);
    return first >= 0 && second >= 0 && first < second;
  }

  // Position of the *last* write to index/subindex, or -1. The restore's ordering has to be
  // asserted on last writes rather than first ones: the operation mode and the controlword are each
  // written on the way in as well, so wroteBefore would compare the preparation, not the restore.
  int lastWriteIndex(uint16_t index, uint8_t subindex) const {
    int found = -1;
    for (size_t k = 0; k < writeLog.size(); ++k) {
      if (writeLog[k].first == index && writeLog[k].second == subindex) {
        found = static_cast<int>(k);
      }
    }
    return found;
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

// --- Encoder register communication procedure ----------------------------------------------------

TEST(ParseEncoderRegisterRequest, DefaultsEverythingButTheRegisterAddress) {
  auto request = parseEncoderRegisterRequest(nlohmann::json{{"registerAddress", 0x75}});
  ASSERT_TRUE(request.has_value()) << request.error();
  EXPECT_EQ(request->encoder, somanet::EncoderOrdinal::kEncoder1);
  EXPECT_FALSE(request->write);
  EXPECT_EQ(request->registerAddress, 0x75);
  EXPECT_EQ(request->value, 0);
}

TEST(ParseEncoderRegisterRequest, RequiresTheRegisterAddress) {
  // The one parameter with no default: reading whichever register happened to be first would be a
  // guess at what the caller meant.
  auto request = parseEncoderRegisterRequest(nlohmann::json::object());
  ASSERT_FALSE(request.has_value());
  EXPECT_NE(request.error().find("registerAddress"), std::string::npos) << request.error();
}

TEST(ParseEncoderRegisterRequest, RejectsAnOrdinalThatIsNeitherSlot) {
  auto request =
      parseEncoderRegisterRequest(nlohmann::json{{"encoder", 3}, {"registerAddress", 0x11}});
  ASSERT_FALSE(request.has_value());
  EXPECT_NE(request.error().find("must be 1 or 2"), std::string::npos) << request.error();
}

TEST(ParseEncoderRegisterRequest, RejectsAValueWiderThanAByte) {
  auto request =
      parseEncoderRegisterRequest(nlohmann::json{{"registerAddress", 0x11}, {"value", 256}});
  ASSERT_FALSE(request.has_value());
  EXPECT_NE(request.error().find("byte value"), std::string::npos) << request.error();
}

TEST(ParseEncoderRegisterRequest, RejectsAWriteFlagThatIsNotBoolean) {
  auto request =
      parseEncoderRegisterRequest(nlohmann::json{{"registerAddress", 0x11}, {"write", 1}});
  ASSERT_FALSE(request.has_value());
  EXPECT_NE(request.error().find("true or false"), std::string::npos) << request.error();
}

TEST(EncoderRegisterParameters, DescribeExactlyWhatTheParserAccepts) {
  // The descriptor is what a client builds its form from, so a parameter it advertises must be one
  // the parser knows and vice versa — the two drifting apart is what this pairing exists to stop.
  const auto parameters = encoderRegisterParameters();
  std::vector<std::string> names;
  for (const auto& parameter : parameters) {
    names.push_back(parameter.name);
  }
  EXPECT_EQ(names, (std::vector<std::string>{"encoder", "write", "registerAddress", "value"}));

  const auto find = [&parameters](std::string_view name) {
    return std::ranges::find(parameters, name, &mm::node::ProcedureParameter::name);
  };
  EXPECT_TRUE(find("registerAddress")->required());
  EXPECT_FALSE(find("encoder")->required());
  EXPECT_FALSE(find("write")->required());
  EXPECT_FALSE(find("value")->required());
  EXPECT_EQ(find("encoder")->options.size(), 2u);
  EXPECT_EQ(find("value")->maxValue, 0xFF);
}

TEST(RunEncoderRegisterProcedure, RecordsWhatTheRegisterHoldsAsTheStepValue) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(encoderRegisterSteps());

  driver.responses = {{1, 0, 0x08, 0, 0, 0, 0, 0}};
  auto result = runEncoderRegisterProcedure(device, reporter, std::stop_token{},
                                            EncoderRegisterRequest{.registerAddress = 0x11});
  ASSERT_TRUE(result.has_value()) << result.error();

  const auto steps = reporter.steps();
  ASSERT_EQ(steps.size(), 1u);
  EXPECT_EQ(steps[0].id, kEncoderRegisterStep);
  EXPECT_EQ(steps[0].status, ProgressStatus::kSucceeded);
  EXPECT_EQ(steps[0].value["value"], 0x08);
  EXPECT_EQ(steps[0].value["wrote"], false);
}

TEST(RunEncoderRegisterProcedure, AWriteCarriesTheDirectionBitAndTheValue) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(encoderRegisterSteps());

  driver.responses = {{1, 0, 0x08, 0, 0, 0, 0, 0}};
  auto result = runEncoderRegisterProcedure(
      device, reporter, std::stop_token{},
      EncoderRegisterRequest{.encoder = somanet::EncoderOrdinal::kEncoder2,
                             .write = true,
                             .registerAddress = 0x11,
                             .value = 0x08});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(driver.store[OsCommandFakeDriver::key(kOsCommand, 1)],
            (std::vector<uint8_t>{0, 2, 1, 0x11, 0x08, 0, 0, 0}));
}

TEST(RunEncoderRegisterProcedure, PreparesNothingAndRestoresNothing) {
  // The property that makes this procedure a single step: command 0 needs no diagnostics mode, no
  // Operation Enabled and no brake, so nothing but the OS command objects may be touched — running
  // it must not disturb a drive that is doing something else.
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(encoderRegisterSteps());

  driver.responses = {{1, 0, 0x08, 0, 0, 0, 0, 0}};
  ASSERT_TRUE(runEncoderRegisterProcedure(device, reporter, std::stop_token{},
                                          EncoderRegisterRequest{.registerAddress = 0x11})
                  .has_value());

  for (const auto& [index, subindex] : driver.writeLog) {
    EXPECT_TRUE(index == kOsCommand || index == kOsCommandMode)
        << std::format("wrote 0x{:04X}:{:02X}", index, subindex);
  }
  EXPECT_TRUE(driver.brakeStatusWrites.empty());
}

TEST(RunEncoderRegisterProcedure, FailsTheStepWhenTheDriveRefusesTheCommand) {
  // What a non-BiSS or unconfigured encoder produces.
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(encoderRegisterSteps());

  driver.responses = {{3, 0, 251, 0, 0, 0, 0, 0}};
  auto result = runEncoderRegisterProcedure(device, reporter, std::stop_token{},
                                            EncoderRegisterRequest{.registerAddress = 0x11});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("command not allowed"), std::string::npos) << result.error();
  EXPECT_EQ(reporter.steps()[0].status, ProgressStatus::kFailed);
}

// --- iC-MU calibration mode procedure ------------------------------------------------------------

TEST(ParseIcMuCalibrationModeRequest, RequiresTheMode) {
  // The mode is the instruction: a run that named none would change how an encoder is read without
  // anyone saying which way.
  auto request = parseIcMuCalibrationModeRequest(nlohmann::json::object());
  ASSERT_FALSE(request.has_value());
  EXPECT_NE(request.error().find("'mode' is required"), std::string::npos) << request.error();
}

TEST(ParseIcMuCalibrationModeRequest, DefaultsTheEncoderButNotTheMode) {
  auto request = parseIcMuCalibrationModeRequest(nlohmann::json{{"mode", "configuration"}});
  ASSERT_TRUE(request.has_value()) << request.error();
  EXPECT_EQ(request->encoder, somanet::EncoderOrdinal::kEncoder1);
  EXPECT_EQ(request->mode, somanet::IcMuCalibrationMode::kConfiguration);
}

TEST(ParseIcMuCalibrationModeRequest, RejectsAnUnknownMode) {
  auto request = parseIcMuCalibrationModeRequest(nlohmann::json{{"mode", "calibration"}});
  ASSERT_FALSE(request.has_value());
  EXPECT_NE(request.error().find("must be one of"), std::string::npos) << request.error();
}

TEST(ParseIcMuCalibrationModeRequest, RejectsAnOrdinalThatIsNeitherSlot) {
  auto request = parseIcMuCalibrationModeRequest(nlohmann::json{{"mode", "raw"}, {"encoder", 3}});
  ASSERT_FALSE(request.has_value());
  EXPECT_NE(request.error().find("must be 1 or 2"), std::string::npos) << request.error();
}

TEST(IcMuCalibrationModeParameters, DescribeExactlyWhatTheParserAccepts) {
  const auto parameters = icMuCalibrationModeParameters();
  ASSERT_EQ(parameters.size(), 2u);
  EXPECT_EQ(parameters[0].name, "encoder");
  EXPECT_FALSE(parameters[0].required());
  EXPECT_EQ(parameters[1].name, "mode");
  EXPECT_TRUE(parameters[1].required());
  // Every offered mode must be one the parser knows, or a client could present a choice that 400s.
  ASSERT_EQ(parameters[1].options.size(), 3u);
  for (const auto& option : parameters[1].options) {
    auto request = parseIcMuCalibrationModeRequest(nlohmann::json{{"mode", option.value}});
    EXPECT_TRUE(request.has_value()) << option.value.dump();
  }
}

TEST(RunIcMuCalibrationModeProcedure, RecordsWhichEncoderIsInWhichMode) {
  // The command answers with nothing, so the step has to carry what was asked for — there is no
  // restore, so a snapshot read later is the only record of where the encoder was left.
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(icMuCalibrationModeSteps());

  driver.responses = {{0, 0, 0, 0, 0, 0, 0, 0}};
  auto result = runIcMuCalibrationModeProcedure(
      device, reporter, std::stop_token{},
      IcMuCalibrationModeRequest{.encoder = somanet::EncoderOrdinal::kEncoder2,
                                 .mode = somanet::IcMuCalibrationMode::kRaw});
  ASSERT_TRUE(result.has_value()) << result.error();

  const auto steps = reporter.steps();
  ASSERT_EQ(steps.size(), 1u);
  EXPECT_EQ(steps[0].status, ProgressStatus::kSucceeded);
  EXPECT_EQ(steps[0].value["encoder"], 2);
  EXPECT_EQ(steps[0].value["mode"], "raw");
  // Mode 1 shifted up three bits, plus ordinal 2.
  EXPECT_EQ(driver.store[OsCommandFakeDriver::key(kOsCommand, 1)],
            (std::vector<uint8_t>{1, 10, 0, 0, 0, 0, 0, 0}));
}

TEST(RunIcMuCalibrationModeProcedure, FailsTheStepWhenTheDriveRefusesTheCommand) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(icMuCalibrationModeSteps());

  driver.responses = {{3, 0, 251, 0, 0, 0, 0, 0}};
  auto result = runIcMuCalibrationModeProcedure(
      device, reporter, std::stop_token{},
      IcMuCalibrationModeRequest{.mode = somanet::IcMuCalibrationMode::kStandard});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("command not allowed"), std::string::npos) << result.error();
  EXPECT_EQ(reporter.steps()[0].status, ProgressStatus::kFailed);
}

// --- HRD streaming procedure ---------------------------------------------------------------------

TEST(ParseHrdStreamingRequest, RequiresBothFields) {
  auto noData = parseHrdStreamingRequest(nlohmann::json{{"durationMs", 1000}});
  ASSERT_FALSE(noData.has_value());
  EXPECT_NE(noData.error().find("'data' is required"), std::string::npos) << noData.error();

  auto noDuration = parseHrdStreamingRequest(nlohmann::json{{"data", "encoder-raw"}});
  ASSERT_FALSE(noDuration.has_value());
  EXPECT_NE(noDuration.error().find("'durationMs' is required"), std::string::npos)
      << noDuration.error();
}

TEST(ParseHrdStreamingRequest, RejectsAnUnknownData) {
  auto request =
      parseHrdStreamingRequest(nlohmann::json{{"data", "encoder"}, {"durationMs", 1000}});
  ASSERT_FALSE(request.has_value());
  EXPECT_NE(request.error().find("must be one of"), std::string::npos) << request.error();
}

TEST(ParseHrdStreamingRequest, BoundsTheDurationByTheChosenData) {
  // The same duration is valid for one selection and not the other, which is the whole reason this
  // check cannot be a fixed range on the parameter: 7000 ms of 4-byte samples fits the drive's five
  // files, 7000 ms of 6-byte samples does not.
  auto encoder =
      parseHrdStreamingRequest(nlohmann::json{{"data", "encoder-raw"}, {"durationMs", 7000}});
  ASSERT_TRUE(encoder.has_value()) << encoder.error();
  EXPECT_EQ(encoder->duration, std::chrono::milliseconds(7000));

  auto system = parseHrdStreamingRequest(
      nlohmann::json{{"data", "system-identification"}, {"durationMs", 7000}});
  ASSERT_FALSE(system.has_value());
  EXPECT_NE(system.error().find("6000"), std::string::npos) << system.error();
}

TEST(HrdStreamingParameters, DescribeExactlyWhatTheParserAccepts) {
  const auto parameters = hrdStreamingParameters();
  ASSERT_EQ(parameters.size(), 2u);
  EXPECT_EQ(parameters[0].name, "data");
  EXPECT_TRUE(parameters[0].required());
  EXPECT_EQ(parameters[1].name, "durationMs");
  EXPECT_TRUE(parameters[1].required());
  // Every offered selection must be one the parser knows, or a client could present a choice that
  // 400s. The advertised ceiling is the wider format's, since a descriptor carries one range.
  ASSERT_EQ(parameters[0].options.size(), 2u);
  for (const auto& option : parameters[0].options) {
    auto request =
        parseHrdStreamingRequest(nlohmann::json{{"data", option.value}, {"durationMs", 1000}});
    EXPECT_TRUE(request.has_value()) << option.value.dump();
  }
  EXPECT_EQ(parameters[1].maxValue, 10000);
}

TEST(RunHrdStreamingProcedure, ConfiguresThenRecordsAndSaysWhatToReadBack) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(hrdStreamingSteps());

  driver.responses = {{0, 0, 0, 0, 0, 0, 0, 0}};
  auto result =
      runHrdStreamingProcedure(device, reporter, std::stop_token{},
                               HrdStreamingRequest{.data = somanet::HrdData::kEncoderRawData,
                                                   .duration = std::chrono::milliseconds(1000)});
  ASSERT_TRUE(result.has_value()) << result.error();

  const auto steps = reporter.steps();
  ASSERT_EQ(steps.size(), 2u);
  EXPECT_EQ(steps[0].status, ProgressStatus::kSucceeded);
  EXPECT_EQ(steps[1].status, ProgressStatus::kSucceeded);
  // Nothing on the drive records which signal its files hold, so the run has to — reading the
  // recording back takes this same selection.
  EXPECT_EQ(steps[0].value["data"], "encoder-raw");
  EXPECT_EQ(steps[0].value["durationMs"], 1000);
  EXPECT_EQ(driver.commandWrites, 2);
}

TEST(RunHrdStreamingProcedure, DoesNotRecordWhenArmingFailed) {
  // A configure that failed leaves the previous recording's files in place and nothing armed, so
  // starting anyway would record under whatever the last run configured.
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(hrdStreamingSteps());

  driver.responses = {{3, 0, 0, 0, 0, 0, 0, 0}};
  auto result =
      runHrdStreamingProcedure(device, reporter, std::stop_token{},
                               HrdStreamingRequest{.data = somanet::HrdData::kEncoderRawData,
                                                   .duration = std::chrono::milliseconds(1000)});
  ASSERT_FALSE(result.has_value());

  const auto steps = reporter.steps();
  EXPECT_EQ(steps[0].status, ProgressStatus::kFailed);
  EXPECT_EQ(steps[1].status, ProgressStatus::kIdle);
  EXPECT_EQ(driver.commandWrites, 1);
}

// --- Open phase detection procedure --------------------------------------------------------------

TEST(OpenPhaseDetectionSteps, DeclaresThreeStepsWithNoBrakeStep) {
  // No release-brake step: command 6's restrictions do not require a disengaged brake — it only
  // "might rotate the motor if there is no brake, or if it's disengaged" — so releasing one would
  // drop whatever it holds and buy nothing. Pole pair detection is the command that does need it.
  auto steps = openPhaseDetectionSteps();
  ASSERT_EQ(steps.size(), 3u);
  EXPECT_EQ(steps[0].id, "prepare");
  EXPECT_EQ(steps[1].id, "open-phase-detection");
  EXPECT_EQ(steps[2].id, "restore");
  for (const auto& step : steps) {
    EXPECT_EQ(step.status, ProgressStatus::kIdle);
  }
}

// Finds a step by id in a reporter snapshot.
//
// Returns by value on purpose. ProgressReporter::steps() hands back a *copy*, so a function
// returning a pointer into it would dangle the moment it was called on the temporary —
// stepById(reporter.steps(), id) reads freed memory, and does it quietly: the status often survives
// while the json value comes back null.
std::optional<mm::node::ProgressStep> stepById(const std::vector<mm::node::ProgressStep>& steps,
                                               std::string_view id) {
  for (const auto& step : steps) {
    if (step.id == id) {
      return step;
    }
  }
  return std::nullopt;
}

// Whether the brake object was written at all during a run.
int brakeWrites(const OsCommandFakeDriver& driver) {
  return static_cast<int>(std::ranges::count_if(
      driver.writeLog, [](const auto& write) { return write.first == somanet::kBrakeOptions; }));
}

TEST(RunOpenPhaseDetectionProcedure, PreparesChecksAndRestores) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(openPhaseDetectionSteps());

  driver.responses = {{0, 0, 0, 0, 0, 0, 0, 0}};  // no phase open
  auto result = runOpenPhaseDetectionProcedure(device, reporter, std::stop_token{});
  ASSERT_TRUE(result.has_value()) << result.error();

  const auto steps = reporter.steps();
  for (auto id : {"prepare", "open-phase-detection", "restore"}) {
    const auto step = stepById(steps, id);
    ASSERT_TRUE(step.has_value()) << id;
    EXPECT_EQ(step->status, ProgressStatus::kSucceeded) << id;
  }

  // Diagnostics mode was requested before the command ran.
  EXPECT_TRUE(driver.wroteBefore(Object::kModeOfOperation, 0, kOsCommand, 1));
}

TEST(RunOpenPhaseDetectionProcedure, NeverTouchesTheBrake) {
  // Not merely "engaged at the end": the brake object is never written, so a load hanging on it is
  // never let go, not even momentarily. Command 6 does not require a disengaged brake.
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(openPhaseDetectionSteps());

  driver.responses = {{0, 0, 0, 0, 0, 0, 0, 0}};
  ASSERT_TRUE(runOpenPhaseDetectionProcedure(device, reporter, std::stop_token{}).has_value());
  EXPECT_EQ(brakeWrites(driver), 0);
}

TEST(RunOpenPhaseDetectionProcedure, RestoresTheModeItFound) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(openPhaseDetectionSteps());

  driver.responses = {{0, 0, 0, 0, 0, 0, 0, 0}};
  ASSERT_TRUE(runOpenPhaseDetectionProcedure(device, reporter, std::stop_token{}).has_value());

  // Back as found: mode 8 (CSP), not left in diagnostics.
  EXPECT_EQ(driver.store.at(OsCommandFakeDriver::key(Object::kModeOfOperation, 0)),
            std::vector<uint8_t>{8});
}

TEST(RunOpenPhaseDetectionProcedure, AnOpenPhaseFailsTheStepAndStillRestores) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(openPhaseDetectionSteps());

  driver.responses = {{3, 0, 0, 0, 0, 0, 0, 0}};  // failed with data, code 0 → open terminal A
  auto result = runOpenPhaseDetectionProcedure(device, reporter, std::stop_token{});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("open terminal A"), std::string::npos) << result.error();

  const auto steps = reporter.steps();
  const auto detect = stepById(steps, "open-phase-detection");
  ASSERT_TRUE(detect.has_value());
  EXPECT_EQ(detect->status, ProgressStatus::kFailed);
  ASSERT_TRUE(detect->error.has_value());
  EXPECT_NE(detect->error->find("terminal A of the drive is not connected"), std::string::npos);

  // The restore is not conditional on success — a failed run must not leave the drive in
  // diagnostics mode.
  const auto restore = stepById(steps, "restore");
  ASSERT_TRUE(restore.has_value());
  EXPECT_EQ(restore->status, ProgressStatus::kSucceeded);
  EXPECT_EQ(driver.store.at(OsCommandFakeDriver::key(Object::kModeOfOperation, 0)),
            std::vector<uint8_t>{8});
}

TEST(RunOpenPhaseDetectionProcedure, RestoresAfterCancellation) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(openPhaseDetectionSteps());

  std::stop_source source;
  source.request_stop();
  auto result = runOpenPhaseDetectionProcedure(device, reporter, source.get_token());
  ASSERT_FALSE(result.has_value());

  const auto steps = reporter.steps();
  const auto restore = stepById(steps, "restore");
  ASSERT_TRUE(restore.has_value());
  EXPECT_EQ(restore->status, ProgressStatus::kSucceeded);
  EXPECT_EQ(driver.store.at(OsCommandFakeDriver::key(Object::kModeOfOperation, 0)),
            std::vector<uint8_t>{8});
}

// --- Pole pair detection procedure
// ----------------------------------------------------------------

TEST(PolePairDetectionSteps, DeclaresFourStepsIncludingTheBrake) {
  // The brake step is present here and absent from the other measurements, and that difference is
  // the firmware's: command 7's restrictions require a disengaged brake, 6/8/9's do not.
  auto steps = polePairDetectionSteps();
  ASSERT_EQ(steps.size(), 4u);
  EXPECT_EQ(steps[0].id, "prepare");
  EXPECT_EQ(steps[1].id, "release-brake");
  EXPECT_EQ(steps[2].id, "pole-pair-detection");
  EXPECT_EQ(steps[3].id, "restore");
}

TEST(RunPolePairDetectionProcedure, PreparesReleasesDetectsAndRestores) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(polePairDetectionSteps());

  driver.responses = {{1, 0, 3, 0, 0, 0, 0, 0}};  // the specification's example: 3 pole pairs
  auto result = runPolePairDetectionProcedure(device, reporter, std::stop_token{});
  ASSERT_TRUE(result.has_value()) << result.error();

  const auto steps = reporter.steps();
  for (auto id : {"prepare", "release-brake", "pole-pair-detection", "restore"}) {
    const auto step = stepById(steps, id);
    ASSERT_TRUE(step.has_value()) << id;
    EXPECT_EQ(step->status, ProgressStatus::kSucceeded) << id;
  }

  const auto detected = stepById(steps, "pole-pair-detection");
  ASSERT_TRUE(detected.has_value());
  EXPECT_EQ(detected->value.at("polePairs").get<uint8_t>(), 3);

  // The brake may only be released once the drive is enabled: in diagnostics mode enabling does not
  // release it, and the write only takes effect from OP ENABLED. So the controlword walk must come
  // first — this is the ordering the firmware requires, not a stylistic preference.
  EXPECT_TRUE(
      driver.wroteBefore(Object::kControlword, 0, somanet::kBrakeOptions, somanet::kBrakeStatus));
  EXPECT_EQ(driver.store.at(OsCommandFakeDriver::key(kOsCommand, 1))[0], 7);
}

TEST(RunPolePairDetectionProcedure, RestoresTheBrakeAndModeItFound) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(polePairDetectionSteps());

  driver.responses = {{1, 0, 4, 0, 0, 0, 0, 0}};
  ASSERT_TRUE(runPolePairDetectionProcedure(device, reporter, std::stop_token{}).has_value());

  // Back as found: brake engaged, mode 8 (CSP) — not left in diagnostics with the brake released.
  EXPECT_EQ(
      driver.store.at(OsCommandFakeDriver::key(somanet::kBrakeOptions, somanet::kBrakeStatus)),
      std::vector<uint8_t>{static_cast<uint8_t>(somanet::BrakeStatus::kEngaged)});
  EXPECT_EQ(driver.store.at(OsCommandFakeDriver::key(Object::kModeOfOperation, 0)),
            std::vector<uint8_t>{8});
}

TEST(RunPolePairDetectionProcedure, RestoresTheBrakeThenDisablesThenPutsTheModeBack) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(polePairDetectionSteps());

  driver.responses = {{1, 0, 4, 0, 0, 0, 0, 0}};
  ASSERT_TRUE(runPolePairDetectionProcedure(device, reporter, std::stop_token{}).has_value());

  const int brake = driver.lastWriteIndex(somanet::kBrakeOptions, somanet::kBrakeStatus);
  const int controlword = driver.lastWriteIndex(Object::kControlword, 0);
  const int mode = driver.lastWriteIndex(Object::kModeOfOperation, 0);
  ASSERT_GE(brake, 0);
  ASSERT_GE(controlword, 0);
  ASSERT_GE(mode, 0);

  // The brake goes back while the drive is still enabled and in diagnostics mode, which is the only
  // state it is the master's to command from.
  EXPECT_LT(brake, controlword);
  // And the mode goes back only *after* the disable, never before: restoring a motion mode while
  // the drive is still in Operation Enabled asks it to follow a setpoint no procedure ever wrote.
  EXPECT_LT(controlword, mode);
}

TEST(RunPolePairDetectionProcedure, AFailedDetectionStillRestoresTheBrake) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(polePairDetectionSteps());

  driver.responses = {{3, 0, 0, 0, 0, 0, 0, 0}};  // command-specific code 0: current amplitude
  auto result = runPolePairDetectionProcedure(device, reporter, std::stop_token{});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("could not raise the motor phase currents"), std::string::npos)
      << result.error();

  const auto restore = stepById(reporter.steps(), "restore");
  ASSERT_TRUE(restore.has_value());
  EXPECT_EQ(restore->status, ProgressStatus::kSucceeded);
  EXPECT_EQ(
      driver.store.at(OsCommandFakeDriver::key(somanet::kBrakeOptions, somanet::kBrakeStatus)),
      std::vector<uint8_t>{static_cast<uint8_t>(somanet::BrakeStatus::kEngaged)});
}

// --- Motor phase order detection procedure -------------------------------------------------------

TEST(MotorPhaseOrderDetectionSteps, DeclaresFourStepsIncludingTheBrake) {
  auto steps = motorPhaseOrderDetectionSteps();
  ASSERT_EQ(steps.size(), 4u);
  EXPECT_EQ(steps[0].id, "prepare");
  EXPECT_EQ(steps[1].id, "release-brake");
  EXPECT_EQ(steps[2].id, "motor-phase-order-detection");
  EXPECT_EQ(steps[3].id, "restore");
}

TEST(RunMotorPhaseOrderDetectionProcedure, PreparesReleasesDetectsAndRestores) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(motorPhaseOrderDetectionSteps());

  driver.responses = {{1, 0, 1, 0, 0, 0, 0, 0}};  // inverted
  auto result = runMotorPhaseOrderDetectionProcedure(device, reporter, std::stop_token{});
  ASSERT_TRUE(result.has_value()) << result.error();

  const auto steps = reporter.steps();
  for (auto id : {"prepare", "release-brake", "motor-phase-order-detection", "restore"}) {
    const auto step = stepById(steps, id);
    ASSERT_TRUE(step.has_value()) << id;
    EXPECT_EQ(step->status, ProgressStatus::kSucceeded) << id;
  }

  const auto detected = stepById(steps, "motor-phase-order-detection");
  ASSERT_TRUE(detected.has_value());
  EXPECT_EQ(detected->value.at("order").get<std::string>(), "inverted");
  EXPECT_TRUE(detected->value.at("inverted").get<bool>());

  EXPECT_EQ(driver.store.at(OsCommandFakeDriver::key(kOsCommand, 1))[0], 4);
  // The brake may only be released once the drive is enabled.
  EXPECT_TRUE(
      driver.wroteBefore(Object::kControlword, 0, somanet::kBrakeOptions, somanet::kBrakeStatus));
}

TEST(RunMotorPhaseOrderDetectionProcedure, RestoresTheBrakeAndModeButNotThePhaseOrder) {
  // The drive writes the detected order into 0x2003:05 itself, and that is the result of the run
  // rather than a side effect — so the restore must put back the mode and the brake and leave the
  // phase order alone. Motion Master never writes 0x2003:05 at all.
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(motorPhaseOrderDetectionSteps());

  driver.responses = {{1, 0, 1, 0, 0, 0, 0, 0}};
  ASSERT_TRUE(
      runMotorPhaseOrderDetectionProcedure(device, reporter, std::stop_token{}).has_value());

  EXPECT_EQ(
      driver.store.at(OsCommandFakeDriver::key(somanet::kBrakeOptions, somanet::kBrakeStatus)),
      std::vector<uint8_t>{static_cast<uint8_t>(somanet::BrakeStatus::kEngaged)});
  EXPECT_EQ(driver.store.at(OsCommandFakeDriver::key(Object::kModeOfOperation, 0)),
            std::vector<uint8_t>{8});
  const auto motorSettingWrites = std::ranges::count_if(
      driver.writeLog, [](const auto& write) { return write.first == 0x2003; });
  EXPECT_EQ(motorSettingWrites, 0);
}

TEST(RunMotorPhaseOrderDetectionProcedure, RestoresAfterCancellation) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(motorPhaseOrderDetectionSteps());

  std::stop_source source;
  source.request_stop();
  auto result = runMotorPhaseOrderDetectionProcedure(device, reporter, source.get_token());
  ASSERT_FALSE(result.has_value());

  const auto restore = stepById(reporter.steps(), "restore");
  ASSERT_TRUE(restore.has_value());
  EXPECT_EQ(restore->status, ProgressStatus::kSucceeded);
  EXPECT_EQ(
      driver.store.at(OsCommandFakeDriver::key(somanet::kBrakeOptions, somanet::kBrakeStatus)),
      std::vector<uint8_t>{static_cast<uint8_t>(somanet::BrakeStatus::kEngaged)});
}

// --- Offset detection (the whole commissioning sequence)
// ------------------------------------------

TEST(OffsetDetectionSteps, DeclaresEveryCommandInTheOrderItRuns) {
  auto steps = offsetDetectionSteps();
  ASSERT_EQ(steps.size(), 10u);
  EXPECT_EQ(steps[0].id, "prepare");
  EXPECT_EQ(steps[1].id, "open-phase-detection");
  EXPECT_EQ(steps[2].id, "phase-resistance-measurement");
  EXPECT_EQ(steps[3].id, "phase-inductance-measurement");
  EXPECT_EQ(steps[4].id, "release-brake");
  EXPECT_EQ(steps[5].id, "pole-pair-detection");
  EXPECT_EQ(steps[6].id, "motor-phase-order-detection");
  EXPECT_EQ(steps[7].id, "set-brake");
  EXPECT_EQ(steps[8].id, "commutation-offset-measurement");
  EXPECT_EQ(steps[9].id, "restore");
}

TEST(RunOffsetDetectionProcedure, RunsEverySixCommandsInOrderAndRecordsEachResult) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(offsetDetectionSteps());

  // One reply per command, in sequence: no open phase, 100000 mOhm, 4244 uH, 3 pole pairs,
  // inverted, offset 2035.
  driver.responses = {
      {0, 0, 0, 0, 0, 0, 0, 0},
      {1, 0, 0x00, 0x01, 0x86, 0xA0, 0, 0},
      {1, 0, 0x00, 0x00, 0x10, 0x94, 0, 0},
      {1, 0, 3, 0, 0, 0, 0, 0},
      {1, 0, 1, 0, 0, 0, 0, 0},
      {1, 0, 0x07, 0xF3, 0, 0, 0, 0},
  };
  auto result = runOffsetDetectionProcedure(device, reporter, std::stop_token{});
  ASSERT_TRUE(result.has_value()) << result.error();

  const auto steps = reporter.steps();
  for (const auto& step : steps) {
    EXPECT_EQ(step.status, ProgressStatus::kSucceeded) << step.id;
  }

  // The order is the whole point of running these as one procedure, so it is asserted rather than
  // assumed: open phase, resistance, inductance, pole pair, phase order, offset.
  EXPECT_EQ(driver.commandIds, (std::vector<uint8_t>{6, 8, 9, 7, 4, 5}));

  // Every measurement kept its own value, so a reader gets the whole commissioning result from one
  // snapshot rather than having to run six procedures and collect them.
  EXPECT_EQ(stepById(steps, "phase-resistance-measurement")->value.at("milliohms").get<uint32_t>(),
            100000u);
  EXPECT_EQ(
      stepById(steps, "phase-inductance-measurement")->value.at("microhenries").get<uint32_t>(),
      4244u);
  EXPECT_EQ(stepById(steps, "pole-pair-detection")->value.at("polePairs").get<uint8_t>(), 3);
  EXPECT_EQ(stepById(steps, "motor-phase-order-detection")->value.at("order").get<std::string>(),
            "inverted");
  EXPECT_EQ(
      stepById(steps, "commutation-offset-measurement")->value.at("angleOffset").get<int16_t>(),
      2035);
}

TEST(RunOffsetDetectionProcedure, ReleasesTheBrakeOnlyAfterTheWindingMeasurements) {
  // The brake is released as late as the sequence allows: open phase, resistance and inductance do
  // not need it, so the load stays held until pole pair detection does. Asserting on order rather
  // than on the final state is the only way to catch a release that happens too early.
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(offsetDetectionSteps());

  driver.responses = {
      {0, 0, 0, 0, 0, 0, 0, 0},
      {1, 0, 0x00, 0x01, 0x86, 0xA0, 0, 0},
      {1, 0, 0x00, 0x00, 0x10, 0x94, 0, 0},
      {1, 0, 3, 0, 0, 0, 0, 0},
      {1, 0, 1, 0, 0, 0, 0, 0},
      {1, 0, 0x07, 0xF3, 0, 0, 0, 0},
  };
  ASSERT_TRUE(runOffsetDetectionProcedure(device, reporter, std::stop_token{}).has_value());

  // Three commands were issued before the brake was ever written.
  const auto& log = driver.writeLog;
  const auto firstBrakeWrite = std::ranges::find_if(log, [](const auto& write) {
    return write.first == somanet::kBrakeOptions && write.second == somanet::kBrakeStatus;
  });
  ASSERT_NE(firstBrakeWrite, log.end());
  const auto commandsBeforeBrake = std::ranges::count_if(
      log.begin(), firstBrakeWrite,
      [](const auto& write) { return write.first == kOsCommand && write.second == 1; });
  EXPECT_EQ(commandsBeforeBrake, 3);
}

TEST(RunOffsetDetectionProcedure, AnOpenPhaseStopsTheRunBeforeAnythingElseIsMeasured) {
  // Every measurement after this one assumes the phases are connected, so a fault has to stop the
  // run rather than let five more steps fail in ways that point at the wrong thing.
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(offsetDetectionSteps());

  driver.responses = {{3, 0, 1, 0, 0, 0, 0, 0}};  // open terminal B
  auto result = runOffsetDetectionProcedure(device, reporter, std::stop_token{});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("open terminal B"), std::string::npos) << result.error();

  const auto steps = reporter.steps();
  EXPECT_EQ(stepById(steps, "open-phase-detection")->status, ProgressStatus::kFailed);
  for (auto id : {"phase-resistance-measurement", "phase-inductance-measurement", "release-brake",
                  "pole-pair-detection", "motor-phase-order-detection", "set-brake",
                  "commutation-offset-measurement"}) {
    EXPECT_EQ(stepById(steps, id)->status, ProgressStatus::kIdle) << id;
  }
  // Nothing was released, and the drive was put back.
  EXPECT_TRUE(driver.brakeStatusWrites.empty());
  EXPECT_EQ(stepById(steps, "restore")->status, ProgressStatus::kSucceeded);
}

TEST(RunOffsetDetectionProcedure, AFailedMeasurementStopsTheRestOfTheSequence) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(offsetDetectionSteps());

  // Open phase passes, then phase resistance is refused.
  driver.responses = {{0, 0, 0, 0, 0, 0, 0, 0}, {3, 0, 251, 0, 0, 0, 0, 0}};
  auto result = runOffsetDetectionProcedure(device, reporter, std::stop_token{});
  ASSERT_FALSE(result.has_value());

  const auto steps = reporter.steps();
  EXPECT_EQ(stepById(steps, "open-phase-detection")->status, ProgressStatus::kSucceeded);
  EXPECT_EQ(stepById(steps, "phase-resistance-measurement")->status, ProgressStatus::kFailed);
  EXPECT_EQ(stepById(steps, "pole-pair-detection")->status, ProgressStatus::kIdle);
  EXPECT_EQ(stepById(steps, "restore")->status, ProgressStatus::kSucceeded);
}

TEST(RunOffsetDetectionProcedure, AFaultMidSequenceStopsAndQuotesTheDriveErrorReport) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(offsetDetectionSteps());

  // Open phase detection passes, and the drive faults on the way out of it — what a real fault or a
  // quick stop does part-way through the sequence.
  driver.responses = {{0, 0, 0, 0, 0, 0, 0, 0}};
  driver.faultAfterCommands = 1;

  auto result = runOffsetDetectionProcedure(device, reporter, std::stop_token{});
  ASSERT_FALSE(result.has_value());

  // Named for what actually happened, not the OS error 251 ("command not allowed") the drive would
  // have answered every later command with — that code names a precondition, so it would have
  // pointed at the operation mode or the brake instead of at the fault.
  EXPECT_NE(result.error().find("Fault"), std::string::npos) << result.error();
  EXPECT_NE(result.error().find("OVERCURR"), std::string::npos) << result.error();

  const auto steps = reporter.steps();
  EXPECT_EQ(stepById(steps, "open-phase-detection")->status, ProgressStatus::kSucceeded);
  // Reported against the step that could not run, which is the one a user is waiting on.
  EXPECT_EQ(stepById(steps, "phase-resistance-measurement")->status, ProgressStatus::kFailed);
  EXPECT_EQ(stepById(steps, "phase-inductance-measurement")->status, ProgressStatus::kIdle);
  EXPECT_EQ(stepById(steps, "pole-pair-detection")->status, ProgressStatus::kIdle);

  // The point of checking first: no further command is put on the wire once the drive has left
  // Operation Enabled.
  EXPECT_EQ(driver.commandIds, std::vector<uint8_t>{6});
  EXPECT_EQ(stepById(steps, "restore")->status, ProgressStatus::kSucceeded);
}

// --- Commutation offset measurement procedure ----------------------------------------------------

TEST(CommutationOffsetDetectionSteps, DeclaresSixStepsIncludingThePhaseOrderDetection) {
  // Command 4 is part of the procedure, not a prerequisite left to the caller: an offset measured
  // against an unknown phase order is wrong, and the drive does not check that it was run.
  auto steps = commutationOffsetDetectionSteps();
  ASSERT_EQ(steps.size(), 6u);
  EXPECT_EQ(steps[0].id, "prepare");
  EXPECT_EQ(steps[1].id, "release-brake");
  EXPECT_EQ(steps[2].id, "motor-phase-order-detection");
  EXPECT_EQ(steps[3].id, "set-brake");
  EXPECT_EQ(steps[4].id, "commutation-offset-measurement");
  EXPECT_EQ(steps[5].id, "restore");
}

TEST(RunCommutationOffsetDetectionProcedure, DetectsThePhaseOrderThenMeasuresTheOffset) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(commutationOffsetDetectionSteps());

  // One scripted reply per command, in order: command 4 answers "inverted", command 5 answers 2035.
  driver.responses = {{1, 0, 1, 0, 0, 0, 0, 0}, {1, 0, 0x07, 0xF3, 0, 0, 0, 0}};
  auto result = runCommutationOffsetDetectionProcedure(device, reporter, std::stop_token{});
  ASSERT_TRUE(result.has_value()) << result.error();

  const auto steps = reporter.steps();
  for (auto id : {"prepare", "release-brake", "motor-phase-order-detection", "set-brake",
                  "commutation-offset-measurement", "restore"}) {
    const auto step = stepById(steps, id);
    ASSERT_TRUE(step.has_value()) << id;
    EXPECT_EQ(step->status, ProgressStatus::kSucceeded) << id;
  }

  const auto phaseOrder = stepById(steps, "motor-phase-order-detection");
  ASSERT_TRUE(phaseOrder.has_value());
  EXPECT_EQ(phaseOrder->value.at("order").get<std::string>(), "inverted");

  const auto measured = stepById(steps, "commutation-offset-measurement");
  ASSERT_TRUE(measured.has_value());
  EXPECT_EQ(measured->value.at("angleOffset").get<int16_t>(), 2035);
  EXPECT_EQ(measured->value.at("method").get<std::string>(), "rotating");
}

TEST(RunCommutationOffsetDetectionProcedure, TheStationaryMethodEngagesTheBrakeForTheMeasurement) {
  // Method 2 cannot hold the load, so the brake goes back on before the offset measurement — but it
  // is still released first, because command 4 requires that unconditionally. So "stationary
  // method" does not mean "the brake never moves"; it means the brake is on for the measurement
  // itself.
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  driver.store[OsCommandFakeDriver::key(somanet::kCommutationOffsetDetection,
                                        somanet::kCommutationOffsetMethod)] = {
      static_cast<uint8_t>(somanet::CommutationOffsetMethod::kStationary)};
  ProgressReporter reporter(commutationOffsetDetectionSteps());

  driver.responses = {{1, 0, 0, 0, 0, 0, 0, 0}, {1, 0, 0x00, 0x10, 0, 0, 0, 0}};
  ASSERT_TRUE(
      runCommutationOffsetDetectionProcedure(device, reporter, std::stop_token{}).has_value());

  // Released for command 4, then engaged again before command 5 — in that order.
  const auto disengaged = static_cast<uint8_t>(somanet::BrakeStatus::kDisengaged);
  const auto engaged = static_cast<uint8_t>(somanet::BrakeStatus::kEngaged);
  const auto& writes = driver.brakeStatusWrites;
  const auto releasedAt = std::ranges::find(writes, disengaged);
  ASSERT_NE(releasedAt, writes.end());
  EXPECT_NE(std::ranges::find(releasedAt, writes.end(), engaged), writes.end());

  const auto measured = stepById(reporter.steps(), "commutation-offset-measurement");
  ASSERT_TRUE(measured.has_value());
  EXPECT_EQ(measured->value.at("method").get<std::string>(), "stationary");
}

TEST(RunCommutationOffsetDetectionProcedure, RestoresTheBrakeItFoundAfterChangingItTwice) {
  // The brake is written twice in this procedure, so the restore must hold the status found
  // *before* either change. A drive found with the brake already released must be left that way.
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  driver.store[OsCommandFakeDriver::key(somanet::kBrakeOptions, somanet::kBrakeStatus)] = {
      static_cast<uint8_t>(somanet::BrakeStatus::kDisengaged)};
  driver.store[OsCommandFakeDriver::key(somanet::kCommutationOffsetDetection,
                                        somanet::kCommutationOffsetMethod)] = {
      static_cast<uint8_t>(somanet::CommutationOffsetMethod::kStationary)};
  ProgressReporter reporter(commutationOffsetDetectionSteps());

  driver.responses = {{1, 0, 0, 0, 0, 0, 0, 0}, {1, 0, 0x00, 0x10, 0, 0, 0, 0}};
  ASSERT_TRUE(
      runCommutationOffsetDetectionProcedure(device, reporter, std::stop_token{}).has_value());

  EXPECT_EQ(
      driver.store.at(OsCommandFakeDriver::key(somanet::kBrakeOptions, somanet::kBrakeStatus)),
      std::vector<uint8_t>{static_cast<uint8_t>(somanet::BrakeStatus::kDisengaged)});
}

TEST(RunCommutationOffsetDetectionProcedure, AFaultAfterThePhaseOrderBlocksTheMeasurement) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(commutationOffsetDetectionSteps());

  // Command 4 succeeds and the drive faults on the way out of it. Command 5 must not be issued: an
  // offset is only meaningful measured on a healthy, enabled drive.
  driver.responses = {{1, 0, 1, 0, 0, 0, 0, 0}};
  driver.faultAfterCommands = 1;

  auto result = runCommutationOffsetDetectionProcedure(device, reporter, std::stop_token{});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("Fault"), std::string::npos) << result.error();

  const auto steps = reporter.steps();
  EXPECT_EQ(stepById(steps, "motor-phase-order-detection")->status, ProgressStatus::kSucceeded);
  EXPECT_EQ(stepById(steps, "commutation-offset-measurement")->status, ProgressStatus::kFailed);
  EXPECT_EQ(driver.commandIds, std::vector<uint8_t>{4});
  EXPECT_EQ(stepById(steps, "restore")->status, ProgressStatus::kSucceeded);
}

TEST(RunCommutationOffsetDetectionProcedure, AFailedPhaseOrderStepStopsBeforeTheMeasurement) {
  // The offset measurement must not run on a phase order that was not established.
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(commutationOffsetDetectionSteps());

  driver.responses = {{3, 0, 251, 0, 0, 0, 0, 0}};  // command 4 refused
  auto result = runCommutationOffsetDetectionProcedure(device, reporter, std::stop_token{});
  ASSERT_FALSE(result.has_value());

  const auto steps = reporter.steps();
  const auto phaseOrder = stepById(steps, "motor-phase-order-detection");
  ASSERT_TRUE(phaseOrder.has_value());
  EXPECT_EQ(phaseOrder->status, ProgressStatus::kFailed);

  const auto measured = stepById(steps, "commutation-offset-measurement");
  ASSERT_TRUE(measured.has_value());
  EXPECT_EQ(measured->status, ProgressStatus::kIdle);

  const auto restore = stepById(steps, "restore");
  ASSERT_TRUE(restore.has_value());
  EXPECT_EQ(restore->status, ProgressStatus::kSucceeded);
}

TEST(RunCommutationOffsetDetectionProcedure, AnUnreadableMethodLeavesTheDriveUntouched) {
  // Not knowing the method means not knowing which way the brake goes, so the run must fail before
  // the drive is enabled rather than guess — no step runs, and nothing is written.
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  driver.store[OsCommandFakeDriver::key(somanet::kCommutationOffsetDetection,
                                        somanet::kCommutationOffsetMethod)] = {7};
  ProgressReporter reporter(commutationOffsetDetectionSteps());

  auto result = runCommutationOffsetDetectionProcedure(device, reporter, std::stop_token{});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("outside the defined range"), std::string::npos) << result.error();

  for (const auto& step : reporter.steps()) {
    EXPECT_EQ(step.status, ProgressStatus::kIdle) << step.id;
  }
  EXPECT_TRUE(driver.brakeStatusWrites.empty());
  EXPECT_EQ(driver.commandWrites, 0);
}

// --- Phase resistance measurement procedure ------------------------------------------------------

TEST(PhaseResistanceMeasurementSteps, DeclaresThreeStepsWithNoBrakeStep) {
  // The absence of a release-brake step is the point: this command's restrictions do not include a
  // released brake, so the procedure must not drop a load for it.
  auto steps = phaseResistanceMeasurementSteps();
  ASSERT_EQ(steps.size(), 3u);
  EXPECT_EQ(steps[0].id, "prepare");
  EXPECT_EQ(steps[1].id, "phase-resistance-measurement");
  EXPECT_EQ(steps[2].id, "restore");
}

TEST(RunPhaseResistanceMeasurementProcedure, PreparesMeasuresAndRestores) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(phaseResistanceMeasurementSteps());

  driver.responses = {{1, 0, 0x00, 0x01, 0x86, 0xA0, 0, 0}};  // 100000 mΩ
  auto result = runPhaseResistanceMeasurementProcedure(device, reporter, std::stop_token{});
  ASSERT_TRUE(result.has_value()) << result.error();

  const auto steps = reporter.steps();
  for (auto id : {"prepare", "phase-resistance-measurement", "restore"}) {
    const auto step = stepById(steps, id);
    ASSERT_TRUE(step.has_value()) << id;
    EXPECT_EQ(step->status, ProgressStatus::kSucceeded) << id;
  }

  const auto measured = stepById(steps, "phase-resistance-measurement");
  ASSERT_TRUE(measured.has_value());
  EXPECT_EQ(measured->value.at("milliohms").get<uint32_t>(), 100000u);

  // Diagnostics mode was requested before the command ran, and the mode was put back afterwards.
  EXPECT_TRUE(driver.wroteBefore(Object::kModeOfOperation, 0, kOsCommand, 1));
  EXPECT_EQ(driver.store.at(OsCommandFakeDriver::key(Object::kModeOfOperation, 0)),
            std::vector<uint8_t>{8});
}

TEST(RunPhaseResistanceMeasurementProcedure, NeverTouchesTheBrake) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(phaseResistanceMeasurementSteps());

  driver.responses = {{1, 0, 0, 0, 0x27, 0x10, 0, 0}};
  ASSERT_TRUE(
      runPhaseResistanceMeasurementProcedure(device, reporter, std::stop_token{}).has_value());

  // Not merely "engaged at the end" — the brake object is never written at all, so a load hanging
  // on it is never let go, not even momentarily.
  EXPECT_EQ(brakeWrites(driver), 0);
}

TEST(RunPhaseResistanceMeasurementProcedure, FailsTheMeasurementStepAndStillRestores) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(phaseResistanceMeasurementSteps());

  driver.responses = {{3, 0, 0, 0, 0, 0, 0, 0}};  // command-specific code 0: current amplitude
  auto result = runPhaseResistanceMeasurementProcedure(device, reporter, std::stop_token{});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("could not raise the motor phase currents"), std::string::npos)
      << result.error();

  const auto steps = reporter.steps();
  const auto measured = stepById(steps, "phase-resistance-measurement");
  ASSERT_TRUE(measured.has_value());
  EXPECT_EQ(measured->status, ProgressStatus::kFailed);

  const auto restore = stepById(steps, "restore");
  ASSERT_TRUE(restore.has_value());
  EXPECT_EQ(restore->status, ProgressStatus::kSucceeded);
  EXPECT_EQ(driver.store.at(OsCommandFakeDriver::key(Object::kModeOfOperation, 0)),
            std::vector<uint8_t>{8});
}

TEST(RunPhaseResistanceMeasurementProcedure, RestoresAfterCancellation) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(phaseResistanceMeasurementSteps());

  std::stop_source source;
  source.request_stop();
  auto result = runPhaseResistanceMeasurementProcedure(device, reporter, source.get_token());
  ASSERT_FALSE(result.has_value());

  const auto restore = stepById(reporter.steps(), "restore");
  ASSERT_TRUE(restore.has_value());
  EXPECT_EQ(restore->status, ProgressStatus::kSucceeded);
  EXPECT_EQ(driver.store.at(OsCommandFakeDriver::key(Object::kModeOfOperation, 0)),
            std::vector<uint8_t>{8});
}

// --- Phase inductance measurement procedure ------------------------------------------------------

TEST(PhaseInductanceMeasurementSteps, DeclaresThreeStepsWithNoBrakeStep) {
  auto steps = phaseInductanceMeasurementSteps();
  ASSERT_EQ(steps.size(), 3u);
  EXPECT_EQ(steps[0].id, "prepare");
  EXPECT_EQ(steps[1].id, "phase-inductance-measurement");
  EXPECT_EQ(steps[2].id, "restore");
}

TEST(RunPhaseInductanceMeasurementProcedure, PreparesMeasuresAndRestores) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(phaseInductanceMeasurementSteps());

  driver.responses = {{1, 0, 0x00, 0x00, 0x10, 0x94, 0, 0}};  // 4244 µH
  auto result = runPhaseInductanceMeasurementProcedure(device, reporter, std::stop_token{});
  ASSERT_TRUE(result.has_value()) << result.error();

  const auto steps = reporter.steps();
  for (auto id : {"prepare", "phase-inductance-measurement", "restore"}) {
    const auto step = stepById(steps, id);
    ASSERT_TRUE(step.has_value()) << id;
    EXPECT_EQ(step->status, ProgressStatus::kSucceeded) << id;
  }

  const auto measured = stepById(steps, "phase-inductance-measurement");
  ASSERT_TRUE(measured.has_value());
  EXPECT_EQ(measured->value.at("microhenries").get<uint32_t>(), 4244u);

  // Command 9, not 8 — the two procedures share their whole body, so which command each issues is
  // the one thing that could silently be wrong.
  EXPECT_EQ(driver.store.at(OsCommandFakeDriver::key(kOsCommand, 1))[0], 9);
}

TEST(RunPhaseInductanceMeasurementProcedure, NeverTouchesTheBrake) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(phaseInductanceMeasurementSteps());

  driver.responses = {{1, 0, 0, 0, 0x10, 0x94, 0, 0}};
  ASSERT_TRUE(
      runPhaseInductanceMeasurementProcedure(device, reporter, std::stop_token{}).has_value());

  EXPECT_EQ(brakeWrites(driver), 0);
}

TEST(RunOpenPhaseDetectionProcedure, FailsTheDeviceCheckBeforeAnyStepRuns) {
  OsCommandFakeDriver driver;
  driver.vendorId = 0x00000002;
  Device device = makeDevice(driver);
  ProgressReporter reporter(openPhaseDetectionSteps());

  auto result = runOpenPhaseDetectionProcedure(device, reporter, std::stop_token{});
  ASSERT_FALSE(result.has_value());
  for (const auto& step : reporter.steps()) {
    EXPECT_EQ(step.status, ProgressStatus::kIdle) << step.id;
  }
}

}  // namespace
