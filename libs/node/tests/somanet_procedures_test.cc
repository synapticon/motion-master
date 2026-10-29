#include "node/somanet_procedures.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <ranges>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "comm/fieldbus_driver.h"
#include "comm/object_data_types.h"
#include "comm/spoe_fieldbus_driver.h"
#include "core/base64.h"
#include "core/platform.h"
#include "fake_spoe_server.h"
#include "node/cia402.h"
#include "node/device.h"
#include "node/device_manager.h"
#include "node/ic_mu_encoder.h"
#include "node/procedure.h"
#include "node/profile_procedures.h"
#include "node/synapticon.h"

namespace {

using mm::comm::EtherCatState;
using mm::comm::FieldbusDriver;
using mm::comm::ObjectDataType;
using mm::comm::OdEntry;
using mm::comm::OdRead;
using mm::comm::SlaveInfo;
using mm::node::commutationOffsetMeasurementSteps;
using mm::node::Device;
using mm::node::encoderRegisterParameters;
using mm::node::EncoderRegisterRequest;
using mm::node::encoderRegisterSteps;
using mm::node::FirmwareLatencyAction;
using mm::node::firmwareLatencyParameters;
using mm::node::FirmwareLatencyRequest;
using mm::node::firmwareLatencySteps;
using mm::node::FsBufferTransfer;
using mm::node::hrdStreamingParameters;
using mm::node::HrdStreamingRequest;
using mm::node::hrdStreamingSteps;
using mm::node::icMuCalibrationModeParameters;
using mm::node::IcMuCalibrationModeRequest;
using mm::node::icMuCalibrationModeSteps;
using mm::node::IgnoreBissStatusBitsRequest;
using mm::node::ignoreBissStatusBitsSteps;
using mm::node::kEncoderRegisterStep;
using mm::node::kFsBufferFilename;
using mm::node::kOsCommand;
using mm::node::kOsCommandMode;
using mm::node::kOsCommandStep;
using mm::node::kReadObjectDictionaryStep;
using mm::node::kSynapticonVendorId;
using mm::node::motorPhaseOrderDetectionSteps;
using mm::node::offsetDetectionSteps;
using mm::node::openPhaseDetectionSteps;
using mm::node::OsCommandRequest;
using mm::node::osCommandSteps;
using mm::node::parseEncoderRegisterRequest;
using mm::node::parseFirmwareLatencyAction;
using mm::node::parseFirmwareLatencyRequest;
using mm::node::parseHrdStreamingRequest;
using mm::node::parseIcMuCalibrationModeRequest;
using mm::node::parseIgnoreBissStatusBitsRequest;
using mm::node::parseOsCommandRequest;
using mm::node::parseSkippedCyclesRequest;
using mm::node::parseSystemIdentificationRequest;
using mm::node::parseVelocitySourceRequest;
using mm::node::phaseInductanceMeasurementSteps;
using mm::node::phaseResistanceMeasurementSteps;
using mm::node::polePairDetectionSteps;
using mm::node::ProgressReporter;
using mm::node::ProgressStatus;
using mm::node::readObjectDictionarySteps;
using mm::node::runCommutationOffsetMeasurementProcedure;
using mm::node::runEncoderRegisterProcedure;
using mm::node::runFirmwareLatencyProcedure;
using mm::node::runHrdStreamingProcedure;
using mm::node::runIcMuCalibrationModeProcedure;
using mm::node::runIgnoreBissStatusBitsProcedure;
using mm::node::runMotorPhaseOrderDetectionProcedure;
using mm::node::runOffsetDetectionProcedure;
using mm::node::runOpenPhaseDetectionProcedure;
using mm::node::runOsCommandProcedure;
using mm::node::runPhaseInductanceMeasurementProcedure;
using mm::node::runPhaseResistanceMeasurementProcedure;
using mm::node::runPolePairDetectionProcedure;
using mm::node::runReadObjectDictionaryProcedure;
using mm::node::runSkippedCyclesProcedure;
using mm::node::runSystemIdentificationProcedure;
using mm::node::runTorqueConstantMeasurementProcedure;
using mm::node::runVelocitySourceProcedure;
using mm::node::SkippedCyclesRequest;
using mm::node::skippedCyclesSteps;
using mm::node::systemIdentificationSteps;
using mm::node::torqueConstantMeasurementSteps;
using mm::node::VelocitySourceRequest;
using mm::node::velocitySourceSteps;
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
  // The whole request of each command, for the procedures that care about its parameter bytes and
  // not only which command was issued.
  std::vector<std::vector<uint8_t>> commands;
  uint32_t vendorId = kSynapticonVendorId;
  uint16_t statusword = 0x0040;  // SwitchOnDisabled
  std::vector<std::pair<uint16_t, uint8_t>> writeLog;

  // Faults the drive once this many commands go out, which models the drive dropping out of
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
        commands.emplace_back(data.begin(), data.end());
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

  std::expected<OdRead, std::string> readObjectDictionary(uint16_t) override {
    return OdRead{.entries = ods};
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
  /// What a read of each filename returns. A name that is absent reads as not found.
  std::map<std::string, std::vector<uint8_t>> files;
  /// Every file read and every file written, in order — the fs-buffer transfers a test asserts on.
  std::vector<std::string> fileReads;
  std::vector<std::pair<std::string, std::vector<uint8_t>>> fileWrites;

  std::expected<std::vector<uint8_t>, mm::comm::FoeError> readFile(
      uint16_t, const std::string& name, std::chrono::milliseconds) override {
    fileReads.push_back(name);
    auto it = files.find(name);
    if (it == files.end()) {
      return std::unexpected(
          mm::comm::makeFoeError(mm::comm::FoeErrorKind::FileNotFound, "FOEread", 1, name));
    }
    return it->second;
  }
  std::expected<void, mm::comm::FoeError> writeFile(uint16_t, const std::string& name,
                                                    std::span<const uint8_t> data) override {
    fileWrites.emplace_back(name, std::vector<uint8_t>(data.begin(), data.end()));
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
                          .pollInterval = kNoDelay,
                          .fsBuffer = {}};
}

// Bodies are parsed from text rather than built from initialiser lists: a JSON integer literal
// arrives unsigned off the wire, and an initialiser list makes it signed, which the byte check
// rejects for reasons that have nothing to do with what is under test.
nlohmann::json osCommandBody(std::string_view json) { return nlohmann::json::parse(json); }

TEST(ParseOsCommandRequest, DefaultsToMovingNoBulkData) {
  auto request = parseOsCommandRequest(osCommandBody(R"({"command": [8, 0, 0, 0, 0, 0, 0, 0]})"));
  ASSERT_TRUE(request.has_value()) << request.error();
  EXPECT_EQ(request->fsBuffer.transfer, FsBufferTransfer::kNone);
  EXPECT_TRUE(request->fsBuffer.payload.empty());
}

TEST(ParseOsCommandRequest, AcceptsAReadTransfer) {
  auto request = parseOsCommandRequest(
      osCommandBody(R"({"command": [21, 0, 0, 0, 0, 0, 0, 0], "fsBuffer": "read"})"));
  ASSERT_TRUE(request.has_value()) << request.error();
  EXPECT_EQ(request->fsBuffer.transfer, FsBufferTransfer::kRead);
}

TEST(ParseOsCommandRequest, DecodesTheWritePayloadFromBase64) {
  // "AQID" is 0x01 0x02 0x03.
  auto request = parseOsCommandRequest(osCommandBody(
      R"({"command": [11, 0, 0, 0, 0, 0, 0, 0], "fsBuffer": "write", "fsBufferData": "AQID"})"));
  ASSERT_TRUE(request.has_value()) << request.error();
  EXPECT_EQ(request->fsBuffer.transfer, FsBufferTransfer::kWrite);
  EXPECT_EQ(request->fsBuffer.payload, std::vector<uint8_t>({1, 2, 3}));
}

TEST(ParseOsCommandRequest, RejectsAnUnknownDirection) {
  auto request = parseOsCommandRequest(
      osCommandBody(R"({"command": [8, 0, 0, 0, 0, 0, 0, 0], "fsBuffer": "sideways"})"));
  ASSERT_FALSE(request.has_value());
  EXPECT_NE(request.error().find("'fsBuffer'"), std::string::npos) << request.error();
}

TEST(ParseOsCommandRequest, RequiresThePayloadForAWrite) {
  auto request = parseOsCommandRequest(
      osCommandBody(R"({"command": [11, 0, 0, 0, 0, 0, 0, 0], "fsBuffer": "write"})"));
  ASSERT_FALSE(request.has_value());
  EXPECT_NE(request.error().find("'fsBufferData' is required"), std::string::npos)
      << request.error();
}

TEST(ParseOsCommandRequest, RefusesAPayloadWithNoDirectionToSendItIn) {
  // Ignoring it would run the command with no transfer at all and look like it worked.
  auto request = parseOsCommandRequest(
      osCommandBody(R"({"command": [8, 0, 0, 0, 0, 0, 0, 0], "fsBufferData": "AQID"})"));
  ASSERT_FALSE(request.has_value());
  EXPECT_NE(request.error().find("only applies"), std::string::npos) << request.error();
}

TEST(ParseOsCommandRequest, TreatsAnEmptyPayloadAsNoPayload) {
  // A client that renders a control for every parameter sends the field whether or not the user
  // filled it in, so an empty one must read as "no bulk data" rather than as a contradiction.
  auto request = parseOsCommandRequest(
      osCommandBody(R"({"command": [8, 0, 0, 0, 0, 0, 0, 0], "fsBufferData": ""})"));
  ASSERT_TRUE(request.has_value()) << request.error();
  EXPECT_EQ(request->fsBuffer.transfer, FsBufferTransfer::kNone);
}

TEST(ParseOsCommandRequest, RejectsAPayloadThatIsNotBase64) {
  auto request = parseOsCommandRequest(osCommandBody(
      R"({"command": [11, 0, 0, 0, 0, 0, 0, 0], "fsBuffer": "write", "fsBufferData": "no!"})"));
  ASSERT_FALSE(request.has_value());
  EXPECT_NE(request.error().find("base64"), std::string::npos) << request.error();
}

TEST(RunOsCommandProcedure, RecordsWhatTheFsBufferTransferRead) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(osCommandSteps());

  driver.files[std::string(kFsBufferFilename)] = {7, 7, 7};
  OsCommandRequest request = makeRequest();
  request.fsBuffer.transfer = FsBufferTransfer::kRead;

  auto result = runOsCommandProcedure(device, reporter, std::stop_token{}, request);
  ASSERT_TRUE(result.has_value()) << result.error();
  const auto steps = reporter.steps();
  ASSERT_EQ(steps.size(), 1U);
  ASSERT_FALSE(steps[0].value.is_null());
  EXPECT_EQ(steps[0].value["fsBuffer"], nlohmann::json({7, 7, 7}));
  ASSERT_EQ(driver.fileReads.size(), 1U);
  EXPECT_EQ(driver.fileReads[0], kFsBufferFilename);
}

TEST(RunOsCommandProcedure, SendsTheFsBufferPayload) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(osCommandSteps());

  OsCommandRequest request = makeRequest();
  request.fsBuffer.transfer = FsBufferTransfer::kWrite;
  request.fsBuffer.payload = {4, 5, 6};

  auto result = runOsCommandProcedure(device, reporter, std::stop_token{}, request);
  ASSERT_TRUE(result.has_value()) << result.error();
  ASSERT_EQ(driver.fileWrites.size(), 1U);
  EXPECT_EQ(driver.fileWrites[0].first, kFsBufferFilename);
  EXPECT_EQ(driver.fileWrites[0].second, std::vector<uint8_t>({4, 5, 6}));
  // Nothing was read, so the result carries no fs-buffer at all rather than an empty array.
  const auto steps = reporter.steps();
  ASSERT_FALSE(steps[0].value.is_null());
  EXPECT_FALSE(steps[0].value.contains("fsBuffer"));
}

TEST(RunReadObjectDictionaryProcedure, ReadsAndDecodesEveryValue) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(readObjectDictionarySteps());

  // The blob has to match the device's own dictionary exactly, so it is built from it.
  std::vector<uint8_t> blob;
  for (const auto& parameter : device.parametersOrdered()) {
    blob.insert(blob.end(), parameter.bitLength / 8U, uint8_t{0});
  }
  driver.files[std::string(kFsBufferFilename)] = blob;

  auto result = runReadObjectDictionaryProcedure(device, reporter, std::stop_token{});
  ASSERT_TRUE(result.has_value()) << result.error();
  const auto steps = reporter.steps();
  ASSERT_EQ(steps.size(), 1U);
  EXPECT_EQ(steps[0].status, ProgressStatus::kSucceeded);
  ASSERT_FALSE(steps[0].value.is_null());
  EXPECT_EQ(steps[0].value["byteCount"], blob.size());
  EXPECT_EQ(steps[0].value["values"].size(), device.parametersOrdered().size());
  ASSERT_EQ(driver.commandIds.size(), 1U);
  EXPECT_EQ(driver.commandIds[0], 21);
}

TEST(RunReadObjectDictionaryProcedure, FailsTheStepWhenTheTransferDoesNotMatchTheDictionary) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(readObjectDictionarySteps());

  driver.files[std::string(kFsBufferFilename)] = {1, 2, 3};  // far too short
  auto result = runReadObjectDictionaryProcedure(device, reporter, std::stop_token{});
  ASSERT_FALSE(result.has_value());
  const auto steps = reporter.steps();
  ASSERT_EQ(steps.size(), 1U);
  EXPECT_EQ(steps[0].status, ProgressStatus::kFailed);
  ASSERT_TRUE(steps[0].error.has_value());
  EXPECT_NE(steps[0].error->find("sent 3 bytes"), std::string::npos) << *steps[0].error;
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
  EXPECT_EQ(request->encoderOrdinal, somanet::EncoderOrdinal::kEncoder1);
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
      parseEncoderRegisterRequest(nlohmann::json{{"encoderOrdinal", 3}, {"registerAddress", 0x11}});
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
  names.reserve(parameters.size());
  for (const auto& parameter : parameters) {
    names.push_back(parameter.name);
  }
  EXPECT_EQ(names,
            (std::vector<std::string>{"encoderOrdinal", "write", "registerAddress", "value"}));

  const auto find = [&parameters](std::string_view name) {
    return std::ranges::find(parameters, name, &mm::node::ProcedureParameter::name);
  };
  EXPECT_TRUE(find("registerAddress")->required());
  EXPECT_FALSE(find("encoderOrdinal")->required());
  EXPECT_FALSE(find("write")->required());
  EXPECT_FALSE(find("value")->required());
  EXPECT_EQ(find("encoderOrdinal")->options.size(), 2u);
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
      EncoderRegisterRequest{.encoderOrdinal = somanet::EncoderOrdinal::kEncoder2,
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
  EXPECT_EQ(request->encoderOrdinal, somanet::EncoderOrdinal::kEncoder1);
  EXPECT_EQ(request->mode, somanet::IcMuCalibrationMode::kConfiguration);
}

TEST(ParseIcMuCalibrationModeRequest, RejectsAnUnknownMode) {
  auto request = parseIcMuCalibrationModeRequest(nlohmann::json{{"mode", "calibration"}});
  ASSERT_FALSE(request.has_value());
  EXPECT_NE(request.error().find("must be one of"), std::string::npos) << request.error();
}

TEST(ParseIcMuCalibrationModeRequest, RejectsAnOrdinalThatIsNeitherSlot) {
  auto request =
      parseIcMuCalibrationModeRequest(nlohmann::json{{"mode", "raw"}, {"encoderOrdinal", 3}});
  ASSERT_FALSE(request.has_value());
  EXPECT_NE(request.error().find("must be 1 or 2"), std::string::npos) << request.error();
}

TEST(IcMuCalibrationModeParameters, DescribeExactlyWhatTheParserAccepts) {
  const auto parameters = icMuCalibrationModeParameters();
  ASSERT_EQ(parameters.size(), 2u);
  EXPECT_EQ(parameters[0].name, "encoderOrdinal");
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
      IcMuCalibrationModeRequest{.encoderOrdinal = somanet::EncoderOrdinal::kEncoder2,
                                 .mode = somanet::IcMuCalibrationMode::kRaw});
  ASSERT_TRUE(result.has_value()) << result.error();

  const auto steps = reporter.steps();
  ASSERT_EQ(steps.size(), 1u);
  EXPECT_EQ(steps[0].status, ProgressStatus::kSucceeded);
  EXPECT_EQ(steps[0].value["encoderOrdinal"], 2);
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
  // answer every later command with — that code names a precondition, so it would point
  // at the operation mode or the brake instead of at the fault.
  EXPECT_NE(result.error().find("Fault"), std::string::npos) << result.error();
  EXPECT_NE(result.error().find("OVERCURR"), std::string::npos) << result.error();

  const auto steps = reporter.steps();
  EXPECT_EQ(stepById(steps, "open-phase-detection")->status, ProgressStatus::kSucceeded);
  // Reported against the step that could not run, which is the one a user is waiting on.
  EXPECT_EQ(stepById(steps, "phase-resistance-measurement")->status, ProgressStatus::kFailed);
  EXPECT_EQ(stepById(steps, "phase-inductance-measurement")->status, ProgressStatus::kIdle);
  EXPECT_EQ(stepById(steps, "pole-pair-detection")->status, ProgressStatus::kIdle);

  // The point of checking first: no further command is put on the wire once the drive leaves
  // Operation Enabled.
  EXPECT_EQ(driver.commandIds, std::vector<uint8_t>{6});
  EXPECT_EQ(stepById(steps, "restore")->status, ProgressStatus::kSucceeded);
}

// --- Commutation offset measurement procedure ----------------------------------------------------

TEST(CommutationOffsetMeasurementSteps, DeclaresFourStepsIncludingTheBrake) {
  // One command, the same shape as every other measurement procedure. The pairing with motor phase
  // order detection lives in the commissioning sequence, not here.
  auto steps = commutationOffsetMeasurementSteps();
  ASSERT_EQ(steps.size(), 4u);
  EXPECT_EQ(steps[0].id, "prepare");
  EXPECT_EQ(steps[1].id, "release-brake");
  EXPECT_EQ(steps[2].id, "commutation-offset-measurement");
  EXPECT_EQ(steps[3].id, "restore");
}

TEST(RunCommutationOffsetMeasurementProcedure, PreparesReleasesMeasuresAndRestores) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(commutationOffsetMeasurementSteps());

  driver.responses = {{1, 0, 0x07, 0xF3, 0, 0, 0, 0}};  // command 5 answers 2035
  auto result = runCommutationOffsetMeasurementProcedure(device, reporter, std::stop_token{});
  ASSERT_TRUE(result.has_value()) << result.error();

  const auto steps = reporter.steps();
  for (auto id : {"prepare", "release-brake", "commutation-offset-measurement", "restore"}) {
    const auto step = stepById(steps, id);
    ASSERT_TRUE(step.has_value()) << id;
    EXPECT_EQ(step->status, ProgressStatus::kSucceeded) << id;
  }

  const auto measured = stepById(steps, "commutation-offset-measurement");
  ASSERT_TRUE(measured.has_value());
  EXPECT_EQ(measured->value.at("angleOffset").get<int16_t>(), 2035);
  EXPECT_EQ(measured->value.at("method").get<std::string>(), "rotating");

  // Command 5 and nothing else: motor phase order detection is a prerequisite the caller runs, not
  // a step this procedure smuggles in.
  EXPECT_EQ(driver.commandIds, std::vector<uint8_t>{5});
}

TEST(RunCommutationOffsetMeasurementProcedure, TheStationaryMethodNeverTouchesTheBrake) {
  // Method 2 asks nothing of the brake, so nothing releases it and whatever it holds stays held.
  // Its release-brake step stays idle rather than reporting work that did not happen.
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  driver.store[OsCommandFakeDriver::key(somanet::kCommutationOffsetDetection,
                                        somanet::kCommutationOffsetMethod)] = {
      static_cast<uint8_t>(somanet::CommutationOffsetMethod::kStationary)};
  ProgressReporter reporter(commutationOffsetMeasurementSteps());

  driver.responses = {{1, 0, 0x00, 0x10, 0, 0, 0, 0}};
  ASSERT_TRUE(
      runCommutationOffsetMeasurementProcedure(device, reporter, std::stop_token{}).has_value());

  EXPECT_TRUE(driver.brakeStatusWrites.empty());

  const auto steps = reporter.steps();
  EXPECT_EQ(stepById(steps, "release-brake")->status, ProgressStatus::kIdle);

  const auto measured = stepById(steps, "commutation-offset-measurement");
  ASSERT_TRUE(measured.has_value());
  EXPECT_EQ(measured->value.at("method").get<std::string>(), "stationary");
}

TEST(RunCommutationOffsetMeasurementProcedure, RestoresTheBrakeItFound) {
  // A rotating method releases the brake, so the restore must put back the status found before it.
  // A drive found with the brake already released must be left that way.
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  driver.store[OsCommandFakeDriver::key(somanet::kBrakeOptions, somanet::kBrakeStatus)] = {
      static_cast<uint8_t>(somanet::BrakeStatus::kDisengaged)};
  ProgressReporter reporter(commutationOffsetMeasurementSteps());

  driver.responses = {{1, 0, 0x00, 0x10, 0, 0, 0, 0}};
  ASSERT_TRUE(
      runCommutationOffsetMeasurementProcedure(device, reporter, std::stop_token{}).has_value());

  EXPECT_EQ(
      driver.store.at(OsCommandFakeDriver::key(somanet::kBrakeOptions, somanet::kBrakeStatus)),
      std::vector<uint8_t>{static_cast<uint8_t>(somanet::BrakeStatus::kDisengaged)});
}

TEST(RunCommutationOffsetMeasurementProcedure, AFailedMeasurementStillRestoresTheBrake) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(commutationOffsetMeasurementSteps());

  driver.responses = {{3, 0, 251, 0, 0, 0, 0, 0}};  // command 5 refused
  auto result = runCommutationOffsetMeasurementProcedure(device, reporter, std::stop_token{});
  ASSERT_FALSE(result.has_value());

  const auto steps = reporter.steps();
  EXPECT_EQ(stepById(steps, "commutation-offset-measurement")->status, ProgressStatus::kFailed);
  EXPECT_EQ(stepById(steps, "restore")->status, ProgressStatus::kSucceeded);
  EXPECT_EQ(
      driver.store.at(OsCommandFakeDriver::key(somanet::kBrakeOptions, somanet::kBrakeStatus)),
      std::vector<uint8_t>{static_cast<uint8_t>(somanet::BrakeStatus::kEngaged)});
}

TEST(RunCommutationOffsetMeasurementProcedure, AnUnreadableMethodLeavesTheDriveUntouched) {
  // Not knowing the method means not knowing whether the brake has to be released, so the run must
  // fail before the drive is enabled rather than guess — no step runs, and nothing is written.
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  driver.store[OsCommandFakeDriver::key(somanet::kCommutationOffsetDetection,
                                        somanet::kCommutationOffsetMethod)] = {7};
  ProgressReporter reporter(commutationOffsetMeasurementSteps());

  auto result = runCommutationOffsetMeasurementProcedure(device, reporter, std::stop_token{});
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

// --- Velocity source procedure
// --------------------------------------------------------------------

TEST(VelocitySourceSteps, DeclaresOneStep) {
  auto steps = velocitySourceSteps();
  ASSERT_EQ(steps.size(), 1u);
  EXPECT_EQ(steps[0].id, "set-source");
}

TEST(ParseVelocitySourceRequest, RequiresTheSource) {
  // No default: which source is already active depends on the product, so a run that named nothing
  // could not be said to be asking for a change at all.
  auto missing = parseVelocitySourceRequest(nlohmann::json::object());
  ASSERT_FALSE(missing.has_value());
  EXPECT_NE(missing.error().find("'source' is required"), std::string::npos) << missing.error();

  auto unknown = parseVelocitySourceRequest(nlohmann::json{{"source", "kuebler"}});
  ASSERT_FALSE(unknown.has_value());
  EXPECT_NE(unknown.error().find("firmware"), std::string::npos) << unknown.error();
}

TEST(RunVelocitySourceProcedure, AppliesTheChoiceAndTouchesNothingElse) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(velocitySourceSteps());

  driver.responses = {{0, 0, 0, 0, 0, 0, 0, 0}};
  auto result = runVelocitySourceProcedure(
      device, reporter, std::stop_token{},
      VelocitySourceRequest{.source = somanet::VelocitySource::kFirmware});
  ASSERT_TRUE(result.has_value()) << result.error();

  const auto step = stepById(reporter.steps(), "set-source");
  ASSERT_TRUE(step.has_value());
  EXPECT_EQ(step->status, ProgressStatus::kSucceeded);
  EXPECT_EQ(step->value.at("source").get<std::string>(), "firmware");
  EXPECT_EQ(driver.commandIds, std::vector<uint8_t>{18});
  // Prepares nothing: no mode, no controlword, no brake.
  EXPECT_EQ(driver.lastWriteIndex(Object::kModeOfOperation, 0), -1);
  EXPECT_EQ(driver.lastWriteIndex(Object::kControlword, 0), -1);
  EXPECT_EQ(brakeWrites(driver), 0);
}

// --- System identification procedure -------------------------------------------------------------

namespace {

nlohmann::json validChirp() {
  return nlohmann::json{{"startFrequencyMilliHz", 1000},
                        {"targetFrequencyMilliHz", 100000},
                        {"targetAmplitudePermil", 50},
                        {"transitionTimeMs", 5000}};
}

}  // namespace

TEST(SystemIdentificationSteps, DeclaresConfigureThenArm) {
  auto steps = systemIdentificationSteps();
  ASSERT_EQ(steps.size(), 2u);
  EXPECT_EQ(steps[0].id, "configure-chirp");
  EXPECT_EQ(steps[1].id, "arm");
}

TEST(ParseSystemIdentificationRequest, AcceptsAValidChirpAndDefaultsTheRest) {
  auto request = parseSystemIdentificationRequest(validChirp());
  ASSERT_TRUE(request.has_value()) << request.error();
  EXPECT_EQ(request->startFrequencyMilliHz, 1000u);
  EXPECT_EQ(request->targetFrequencyMilliHz, 100000u);
  EXPECT_EQ(request->targetAmplitudePermil, 50u);
  EXPECT_EQ(request->transitionTimeMs, 5000u);
  EXPECT_EQ(request->signalType, somanet::ChirpSignalType::kLogarithmic);
  // Not arming by default: a request that says nothing should not excite a motor.
  EXPECT_EQ(request->start, somanet::SystemIdentificationStart::kNone);
}

TEST(ParseSystemIdentificationRequest, EnforcesTheFirmwaresOwnBounds) {
  // Every one of these is rejected by the firmware too — but there it is rejected *when armed*,
  // as an Invalid Parameter fault with a quick stop. Catching them here is the difference between
  // a 400 and a faulted drive, which is the whole reason this validation exists.
  auto tooSlow = validChirp();
  tooSlow["startFrequencyMilliHz"] = somanet::kMinChirpFrequencyMilliHz - 1;
  EXPECT_FALSE(parseSystemIdentificationRequest(tooSlow).has_value());

  auto tooFast = validChirp();
  tooFast["targetFrequencyMilliHz"] = somanet::kMaxChirpFrequencyMilliHz + 1;
  EXPECT_FALSE(parseSystemIdentificationRequest(tooFast).has_value());

  auto tooShort = validChirp();
  tooShort["transitionTimeMs"] = somanet::kMinChirpTransitionTimeMs - 1;
  EXPECT_FALSE(parseSystemIdentificationRequest(tooShort).has_value());

  auto tooLong = validChirp();
  tooLong["transitionTimeMs"] = somanet::kMaxChirpTransitionTimeMs + 1;
  EXPECT_FALSE(parseSystemIdentificationRequest(tooLong).has_value());
}

TEST(ParseSystemIdentificationRequest, RejectsADescendingSweep) {
  auto descending = validChirp();
  descending["startFrequencyMilliHz"] = 100000;
  descending["targetFrequencyMilliHz"] = 1000;
  auto request = parseSystemIdentificationRequest(descending);
  ASSERT_FALSE(request.has_value());
  EXPECT_NE(request.error().find("runs upwards"), std::string::npos) << request.error();
}

TEST(ParseSystemIdentificationRequest, LeavesTheAmplitudeToTheCaller) {
  // The firmware does not check it, so neither does this: a limit invented here would refuse a
  // value some machine legitimately needs.
  auto large = validChirp();
  large["targetAmplitudePermil"] = 5000;
  auto request = parseSystemIdentificationRequest(large);
  ASSERT_TRUE(request.has_value()) << request.error();
  EXPECT_EQ(request->targetAmplitudePermil, 5000u);
}

TEST(RunSystemIdentificationProcedure, DisarmsBeforeConfiguringAndArmsLast) {
  // The order is the whole correctness argument: the drive starts on the *rising* edge of the arm
  // parameter, so a run that did not disarm first would silently not start after a previous one.
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(systemIdentificationSteps());

  driver.responses = {{0, 0, 0, 0, 0, 0, 0, 0}};
  auto request = parseSystemIdentificationRequest(validChirp());
  ASSERT_TRUE(request.has_value()) << request.error();
  request->start = somanet::SystemIdentificationStart::kAfterHrdStreamStart;

  auto result = runSystemIdentificationProcedure(device, reporter, std::stop_token{}, *request);
  ASSERT_TRUE(result.has_value()) << result.error();

  // Seven writes: disarm, the five settings, then the arm.
  ASSERT_EQ(driver.commandWrites, 7);
  const auto& first = driver.commands.front();
  EXPECT_EQ(first[1], 5) << "the first write must be the arm parameter";
  EXPECT_EQ(first[5], 0) << "and it must clear it";
  const auto& last = driver.commands.back();
  EXPECT_EQ(last[1], 5);
  EXPECT_EQ(last[5],
            static_cast<uint8_t>(somanet::SystemIdentificationStart::kAfterHrdStreamStart));

  for (auto id : {"configure-chirp", "arm"}) {
    const auto step = stepById(reporter.steps(), id);
    ASSERT_TRUE(step.has_value()) << id;
    EXPECT_EQ(step->status, ProgressStatus::kSucceeded) << id;
  }
  // Nothing reads the settings back, so the step's record is the only account of them.
  const auto configured = stepById(reporter.steps(), "configure-chirp");
  EXPECT_EQ(configured->value.at("transitionTimeMs").get<uint32_t>(), 5000u);
  EXPECT_EQ(configured->value.at("signalType").get<std::string>(), "logarithmic");
  EXPECT_EQ(stepById(reporter.steps(), "arm")->value.at("start").get<std::string>(),
            "after-hrd-stream-start");
}

TEST(RunSystemIdentificationProcedure, ConfiguringWithoutArmingWritesAZeroTrigger) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(systemIdentificationSteps());

  driver.responses = {{0, 0, 0, 0, 0, 0, 0, 0}};
  auto request = parseSystemIdentificationRequest(validChirp());
  ASSERT_TRUE(request.has_value()) << request.error();

  ASSERT_TRUE(
      runSystemIdentificationProcedure(device, reporter, std::stop_token{}, *request).has_value());
  EXPECT_EQ(driver.commands.back()[1], 5);
  EXPECT_EQ(driver.commands.back()[5], 0);
  EXPECT_EQ(stepById(reporter.steps(), "arm")->value.at("start").get<std::string>(), "none");
}

TEST(RunSystemIdentificationProcedure, AFailedSettingStopsBeforeArming) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(systemIdentificationSteps());

  driver.responses = {{2, 0, 0, 0, 0, 0, 0, 0}};  // the drive rejects the very first write
  auto request = parseSystemIdentificationRequest(validChirp());
  ASSERT_TRUE(request.has_value()) << request.error();
  request->start = somanet::SystemIdentificationStart::kImmediately;

  auto result = runSystemIdentificationProcedure(device, reporter, std::stop_token{}, *request);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(stepById(reporter.steps(), "configure-chirp")->status, ProgressStatus::kFailed);
  // Never armed: a half-written chirp must not be started.
  EXPECT_EQ(stepById(reporter.steps(), "arm")->status, ProgressStatus::kIdle);
}

// --- Ignore BiSS status bits procedure -----------------------------------------------------------

TEST(IgnoreBissStatusBitsSteps, DeclaresOneStep) {
  auto steps = ignoreBissStatusBitsSteps();
  ASSERT_EQ(steps.size(), 1u);
  EXPECT_EQ(steps[0].id, "set-ignore");
}

TEST(ParseIgnoreBissStatusBitsRequest, RequiresTheDirection) {
  // Defaulting it either way would let a run that named nothing change how the drive reacts to its
  // encoder — in one direction, silently suppressing a fault.
  auto missing = parseIgnoreBissStatusBitsRequest(nlohmann::json{{"encoderOrdinal", 1}});
  ASSERT_FALSE(missing.has_value());
  EXPECT_NE(missing.error().find("'ignore' is required"), std::string::npos) << missing.error();

  auto wrongType = parseIgnoreBissStatusBitsRequest(nlohmann::json{{"ignore", 1}});
  ASSERT_FALSE(wrongType.has_value());
  EXPECT_NE(wrongType.error().find("must be a boolean"), std::string::npos) << wrongType.error();
}

TEST(ParseIgnoreBissStatusBitsRequest, DefaultsToEncoderOne) {
  auto request = parseIgnoreBissStatusBitsRequest(nlohmann::json{{"ignore", true}});
  ASSERT_TRUE(request.has_value()) << request.error();
  EXPECT_EQ(request->encoderOrdinal, somanet::EncoderOrdinal::kEncoder1);
  EXPECT_TRUE(request->ignore);
}

TEST(RunIgnoreBissStatusBitsProcedure, AppliesTheChangeAndTouchesNothingElse) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(ignoreBissStatusBitsSteps());

  driver.responses = {{0, 0, 0, 0, 0, 0, 0, 0}};  // completed, no reply — all this command answers
  auto result = runIgnoreBissStatusBitsProcedure(
      device, reporter, std::stop_token{},
      IgnoreBissStatusBitsRequest{.encoderOrdinal = somanet::EncoderOrdinal::kEncoder2,
                                  .ignore = true});
  ASSERT_TRUE(result.has_value()) << result.error();

  const auto step = stepById(reporter.steps(), "set-ignore");
  ASSERT_TRUE(step.has_value());
  EXPECT_EQ(step->status, ProgressStatus::kSucceeded);
  // Nothing on the drive reports the flag back, so the step's record of what was asked for is the
  // only account of it.
  EXPECT_EQ(step->value.at("encoderOrdinal").get<uint8_t>(), 2);
  EXPECT_TRUE(step->value.at("ignore").get<bool>());

  EXPECT_EQ(driver.store.at(OsCommandFakeDriver::key(kOsCommand, 1))[1], 0b101);
  // Prepares nothing: no operation mode, no controlword, no brake.
  EXPECT_EQ(driver.lastWriteIndex(Object::kModeOfOperation, 0), -1);
  EXPECT_EQ(driver.lastWriteIndex(Object::kControlword, 0), -1);
  EXPECT_EQ(brakeWrites(driver), 0);
}

TEST(RunIgnoreBissStatusBitsProcedure, AFailedCommandFailsTheStep) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(ignoreBissStatusBitsSteps());

  driver.responses = {{3, 0, 253, 0, 0, 0, 0, 0}};
  auto result = runIgnoreBissStatusBitsProcedure(device, reporter, std::stop_token{},
                                                 IgnoreBissStatusBitsRequest{.ignore = true});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("not a BiSS encoder"), std::string::npos) << result.error();

  const auto step = stepById(reporter.steps(), "set-ignore");
  ASSERT_TRUE(step.has_value());
  EXPECT_EQ(step->status, ProgressStatus::kFailed);
}

// --- Skipped cycles counter procedure ------------------------------------------------------------

TEST(SkippedCyclesSteps, DeclaresOneStep) {
  auto steps = skippedCyclesSteps();
  ASSERT_EQ(steps.size(), 1u);
  EXPECT_EQ(steps[0].id, "read-counter");
}

TEST(ParseSkippedCyclesRequest, DefaultsToTheDriveControlService) {
  auto request = parseSkippedCyclesRequest(nlohmann::json::object());
  ASSERT_TRUE(request.has_value()) << request.error();
  EXPECT_EQ(request->service, somanet::FirmwareService::kDriveControl);
}

TEST(ParseSkippedCyclesRequest, RejectsAServiceTheFirmwareHasNoIdFor) {
  // Not pedantry: each service answers only requests naming itself, so an unknown id is answered by
  // nothing and costs the drive's whole command timeout before failing for a reason that says
  // nothing about the actual mistake.
  auto numeric = parseSkippedCyclesRequest(nlohmann::json{{"service", 2}});
  ASSERT_FALSE(numeric.has_value());
  EXPECT_NE(numeric.error().find("must be a string"), std::string::npos) << numeric.error();

  auto unknown = parseSkippedCyclesRequest(nlohmann::json{{"service", "position-control"}});
  ASSERT_FALSE(unknown.has_value());
  EXPECT_NE(unknown.error().find("drive-control"), std::string::npos) << unknown.error();
}

TEST(RunSkippedCyclesProcedure, ReadsTheCounterAndTouchesNothingElse) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(skippedCyclesSteps());

  driver.responses = {{1, 0, 0x00, 0x00, 0x01, 0x2C, 0, 0}};  // 300
  auto result = runSkippedCyclesProcedure(
      device, reporter, std::stop_token{},
      SkippedCyclesRequest{.service = somanet::FirmwareService::kMotionControl});
  ASSERT_TRUE(result.has_value()) << result.error();

  const auto step = stepById(reporter.steps(), "read-counter");
  ASSERT_TRUE(step.has_value());
  EXPECT_EQ(step->status, ProgressStatus::kSucceeded);
  EXPECT_EQ(step->value.at("skippedCycles").get<uint32_t>(), 300u);
  EXPECT_EQ(step->value.at("service").get<std::string>(), "motion-control");

  // The whole point of this procedure: it prepares nothing. No operation mode, no controlword, no
  // brake — a pure read, safe on a drive that is enabled and moving.
  EXPECT_EQ(driver.lastWriteIndex(Object::kModeOfOperation, 0), -1);
  EXPECT_EQ(driver.lastWriteIndex(Object::kControlword, 0), -1);
  EXPECT_EQ(brakeWrites(driver), 0);
}

TEST(RunSkippedCyclesProcedure, AFailedReadFailsTheStep) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(skippedCyclesSteps());

  driver.responses = {{3, 0, 253, 0, 0, 0, 0, 0}};
  auto result =
      runSkippedCyclesProcedure(device, reporter, std::stop_token{}, SkippedCyclesRequest{});
  ASSERT_FALSE(result.has_value());

  const auto step = stepById(reporter.steps(), "read-counter");
  ASSERT_TRUE(step.has_value());
  EXPECT_EQ(step->status, ProgressStatus::kFailed);
}

// --- Firmware latency measurement procedure ------------------------------------------------------

TEST(ParseFirmwareLatencyRequest, DefaultsToMeasuringTheSetpointLatency) {
  auto request = parseFirmwareLatencyRequest(nlohmann::json::object());
  ASSERT_TRUE(request.has_value()) << request.error();
  EXPECT_EQ(request->action, FirmwareLatencyAction::kMeasure);
  EXPECT_EQ(request->latency, somanet::FirmwareLatency::kSetpoint);
  EXPECT_EQ(request->duration, std::chrono::milliseconds(2000));
}

TEST(ParseFirmwareLatencyRequest, ReadsEveryActionAndBothLatencies) {
  for (auto action : {FirmwareLatencyAction::kMeasure, FirmwareLatencyAction::kStart,
                      FirmwareLatencyAction::kReadMaximum, FirmwareLatencyAction::kStop}) {
    auto request = parseFirmwareLatencyRequest(
        nlohmann::json{{"action", mm::node::toString(action)}, {"latency", "feedback"}});
    ASSERT_TRUE(request.has_value()) << request.error();
    EXPECT_EQ(request->action, action);
    EXPECT_EQ(request->latency, somanet::FirmwareLatency::kFeedback);
  }
}

TEST(ParseFirmwareLatencyRequest, RejectsUnknownTokensAndAnOutOfRangeDuration) {
  auto action = parseFirmwareLatencyRequest(nlohmann::json{{"action", "reset"}});
  ASSERT_FALSE(action.has_value());
  EXPECT_NE(action.error().find("read-maximum"), std::string::npos) << action.error();

  auto latency = parseFirmwareLatencyRequest(nlohmann::json{{"latency", "cycle"}});
  ASSERT_FALSE(latency.has_value());
  EXPECT_NE(latency.error().find("setpoint"), std::string::npos) << latency.error();

  auto tooShort = parseFirmwareLatencyRequest(nlohmann::json{{"durationMs", 10}});
  ASSERT_FALSE(tooShort.has_value());

  auto tooLong = parseFirmwareLatencyRequest(nlohmann::json{{"durationMs", 60001}});
  ASSERT_FALSE(tooLong.has_value());
}

TEST(FirmwareLatencySteps, DeclaresOneStepPerAction) {
  auto steps = firmwareLatencySteps();
  ASSERT_EQ(steps.size(), 4u);
  EXPECT_EQ(steps[0].id, "start-measurement");
  EXPECT_EQ(steps[1].id, "observe");
  EXPECT_EQ(steps[2].id, "read-maximum");
  EXPECT_EQ(steps[3].id, "stop-measurements");
}

TEST(FirmwareLatencyDescriptor, EveryParameterHasABoundOrOptions) {
  auto parameters = firmwareLatencyParameters();
  ASSERT_EQ(parameters.size(), 3u);
  EXPECT_EQ(parameters[0].name, "action");
  EXPECT_EQ(parameters[0].options.size(), 4u);
  EXPECT_EQ(parameters[1].name, "latency");
  EXPECT_EQ(parameters[1].options.size(), 2u);
  EXPECT_EQ(parameters[2].name, "durationMs");
  EXPECT_EQ(parameters[2].minValue, 100);
  EXPECT_EQ(parameters[2].maxValue, 60000);
}

TEST(RunFirmwareLatencyProcedure, MeasureStartsObservesAndReads) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(firmwareLatencySteps());

  // Two commands: the start answers status 0, the read answers status 1 with 1234 and 3000 ticks
  // little-endian — 12340 ns measured against 30000 ns configured.
  driver.responses = {{0, 0, 0, 0, 0, 0, 0, 0}, {1, 0, 0xD2, 0x04, 0x00, 0xB8, 0x0B, 0x00}};
  auto result = runFirmwareLatencyProcedure(
      device, reporter, std::stop_token{},
      FirmwareLatencyRequest{.action = FirmwareLatencyAction::kMeasure,
                             .latency = somanet::FirmwareLatency::kFeedback,
                             .duration = std::chrono::milliseconds(100)});
  ASSERT_TRUE(result.has_value()) << result.error();

  const auto steps = reporter.steps();
  for (auto id : {"start-measurement", "observe", "read-maximum"}) {
    const auto step = stepById(steps, id);
    ASSERT_TRUE(step.has_value()) << id;
    EXPECT_EQ(step->status, ProgressStatus::kSucceeded) << id;
  }
  const auto read = stepById(steps, "read-maximum");
  EXPECT_EQ(read->value.at("maximumNanoseconds").get<uint32_t>(), 12340u);
  EXPECT_EQ(read->value.at("configuredNanoseconds").get<uint32_t>(), 30000u);
  EXPECT_EQ(read->value.at("latency").get<std::string>(), "feedback");

  // Left running on purpose: stopping is not per-latency, so ending this measurement would end a
  // measurement of the other latency too.
  const auto stopped = stepById(steps, "stop-measurements");
  ASSERT_TRUE(stopped.has_value());
  EXPECT_EQ(stopped->status, ProgressStatus::kIdle);

  // Prepares nothing, like the counter read: no operation mode, no controlword, no brake.
  EXPECT_EQ(driver.lastWriteIndex(Object::kModeOfOperation, 0), -1);
  EXPECT_EQ(driver.lastWriteIndex(Object::kControlword, 0), -1);
  EXPECT_EQ(brakeWrites(driver), 0);
}

TEST(RunFirmwareLatencyProcedure, StartOnlyStartsAndLeavesTheReadIdle) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(firmwareLatencySteps());

  driver.responses = {{0, 0, 0, 0, 0, 0, 0, 0}};
  auto result =
      runFirmwareLatencyProcedure(device, reporter, std::stop_token{},
                                  FirmwareLatencyRequest{.action = FirmwareLatencyAction::kStart});
  ASSERT_TRUE(result.has_value()) << result.error();

  const auto steps = reporter.steps();
  EXPECT_EQ(stepById(steps, "start-measurement")->status, ProgressStatus::kSucceeded);
  for (auto id : {"observe", "read-maximum", "stop-measurements"}) {
    EXPECT_EQ(stepById(steps, id)->status, ProgressStatus::kIdle) << id;
  }
  // One command, and it is the start action.
  const auto written = driver.store.at(OsCommandFakeDriver::key(kOsCommand, 1));
  EXPECT_EQ(written[0], 22);
  EXPECT_EQ(written[1], 0);
}

TEST(RunFirmwareLatencyProcedure, StopIssuesTheStopActionAlone) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(firmwareLatencySteps());

  driver.responses = {{0, 0, 0, 0, 0, 0, 0, 0}};
  auto result =
      runFirmwareLatencyProcedure(device, reporter, std::stop_token{},
                                  FirmwareLatencyRequest{.action = FirmwareLatencyAction::kStop});
  ASSERT_TRUE(result.has_value()) << result.error();

  const auto steps = reporter.steps();
  EXPECT_EQ(stepById(steps, "stop-measurements")->status, ProgressStatus::kSucceeded);
  EXPECT_EQ(stepById(steps, "start-measurement")->status, ProgressStatus::kIdle);
  const auto written = driver.store.at(OsCommandFakeDriver::key(kOsCommand, 1));
  EXPECT_EQ(written[1], 2);
}

TEST(RunFirmwareLatencyProcedure, AFailedStartFailsTheStepAndSkipsTheRest) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(firmwareLatencySteps());

  driver.responses = {{3, 0, 253, 0, 0, 0, 0, 0}};
  auto result = runFirmwareLatencyProcedure(
      device, reporter, std::stop_token{},
      FirmwareLatencyRequest{.action = FirmwareLatencyAction::kMeasure,
                             .duration = std::chrono::milliseconds(100)});
  ASSERT_FALSE(result.has_value());

  const auto steps = reporter.steps();
  EXPECT_EQ(stepById(steps, "start-measurement")->status, ProgressStatus::kFailed);
  // Never observed, so never waited the duration out — a failed start is not a measurement.
  EXPECT_EQ(stepById(steps, "observe")->status, ProgressStatus::kIdle);
  EXPECT_EQ(stepById(steps, "read-maximum")->status, ProgressStatus::kIdle);
}

TEST(ParseFirmwareLatencyAction, RoundTripsItsOwnTokens) {
  EXPECT_EQ(parseFirmwareLatencyAction("measure"), FirmwareLatencyAction::kMeasure);
  EXPECT_EQ(parseFirmwareLatencyAction("start"), FirmwareLatencyAction::kStart);
  EXPECT_EQ(parseFirmwareLatencyAction("read-maximum"), FirmwareLatencyAction::kReadMaximum);
  EXPECT_EQ(parseFirmwareLatencyAction("stop"), FirmwareLatencyAction::kStop);
  EXPECT_FALSE(parseFirmwareLatencyAction("").has_value());
  EXPECT_FALSE(parseFirmwareLatencyAction("read").has_value());
}

// --- Torque constant measurement procedure -------------------------------------------------------

TEST(TorqueConstantMeasurementSteps, DeclaresFourStepsIncludingTheBrake) {
  // With the brake step, unlike the two winding measurements: command 10's restrictions require a
  // disengaged brake, which is the firmware grouping it with pole pair and phase order detection.
  auto steps = torqueConstantMeasurementSteps();
  ASSERT_EQ(steps.size(), 4u);
  EXPECT_EQ(steps[0].id, "prepare");
  EXPECT_EQ(steps[1].id, "release-brake");
  EXPECT_EQ(steps[2].id, "torque-constant-measurement");
  EXPECT_EQ(steps[3].id, "restore");
}

TEST(RunTorqueConstantMeasurementProcedure, PreparesReleasesMeasuresAndRestores) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(torqueConstantMeasurementSteps());

  driver.responses = {{1, 0, 0x00, 0x12, 0x5C, 0x0A, 0, 0}};  // 1203210 mNm/A_rms
  auto result = runTorqueConstantMeasurementProcedure(device, reporter, std::stop_token{});
  ASSERT_TRUE(result.has_value()) << result.error();

  const auto steps = reporter.steps();
  for (auto id : {"prepare", "release-brake", "torque-constant-measurement", "restore"}) {
    const auto step = stepById(steps, id);
    ASSERT_TRUE(step.has_value()) << id;
    EXPECT_EQ(step->status, ProgressStatus::kSucceeded) << id;
  }

  const auto measured = stepById(steps, "torque-constant-measurement");
  ASSERT_TRUE(measured.has_value());
  EXPECT_EQ(measured->value.at("milliNewtonMetresPerAmpere").get<int32_t>(), 1203210);

  // The brake is released only once the drive is enabled, as for pole pair detection.
  EXPECT_TRUE(
      driver.wroteBefore(Object::kControlword, 0, somanet::kBrakeOptions, somanet::kBrakeStatus));
  EXPECT_EQ(driver.store.at(OsCommandFakeDriver::key(kOsCommand, 1))[0], 10);
}

TEST(RunTorqueConstantMeasurementProcedure, AFailedMeasurementStillRestoresTheBrake) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(torqueConstantMeasurementSteps());

  driver.responses = {{3, 0, 251, 0, 0, 0, 0, 0}};
  auto result = runTorqueConstantMeasurementProcedure(device, reporter, std::stop_token{});
  ASSERT_FALSE(result.has_value());

  const auto restore = stepById(reporter.steps(), "restore");
  ASSERT_TRUE(restore.has_value());
  EXPECT_EQ(restore->status, ProgressStatus::kSucceeded);
  EXPECT_EQ(
      driver.store.at(OsCommandFakeDriver::key(somanet::kBrakeOptions, somanet::kBrakeStatus)),
      std::vector<uint8_t>{static_cast<uint8_t>(somanet::BrakeStatus::kEngaged)});
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

// ── Firmware installation ──────────────────────────────────────────────────────────────────────

using mm::node::firmwareInstallationParameters;
using mm::node::firmwareInstallationSteps;
using mm::node::kDefaultSkippedFirmwareFiles;
using mm::node::parseFirmwareInstallationRequest;

constexpr std::string_view kCirculoPackageName =
    "package_SOMANET-Circulo-7_8500-04-2332_motion-drive_v5.6.10.zip";

// The real package, base64-encoded exactly as a client would send it in the request body.
std::string circuloPackageBase64() {
  const std::filesystem::path path =
      std::filesystem::path(MM_NODE_TEST_DATA_DIR) / kCirculoPackageName;
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in) << "missing test fixture: " << path;
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in),
                                   std::istreambuf_iterator<char>()};
  return mm::core::base64Encode(bytes);
}

nlohmann::json requestWithPackage() {
  return nlohmann::json{{"packageContent", circuloPackageBase64()},
                        {"packageFilename", std::string(kCirculoPackageName)}};
}

TEST(FirmwareInstallationRequest, AcceptsAnInlinePackage) {
  auto request = parseFirmwareInstallationRequest(requestWithPackage());
  ASSERT_TRUE(request) << request.error();
  // Round-tripped through base64 and back to the exact zip.
  EXPECT_EQ(request->package.size(), 608513u);
  EXPECT_EQ(request->package[0], 'P');
  EXPECT_EQ(request->package[1], 'K');
  ASSERT_TRUE(request->name.has_value());
  EXPECT_EQ(request->name->softwareVersion, "v5.6.10");
}

TEST(FirmwareInstallationRequest, DefaultsMatchTheDescriptor) {
  auto request = parseFirmwareInstallationRequest(requestWithPackage());
  ASSERT_TRUE(request) << request.error();
  EXPECT_TRUE(request->cachePackage);
  EXPECT_EQ(request->finalState, EtherCatState::PreOp);
  ASSERT_EQ(request->skipFiles.size(), kDefaultSkippedFirmwareFiles.size());
  EXPECT_EQ(request->skipFiles[0], "SOMANET_CiA_402.xml.zip");
  EXPECT_EQ(request->skipFiles[1], "stack_image.svg.zip");
}

// An explicit list replaces the defaults wholesale rather than adding to them — including an empty
// list, which is the only way to say "write everything the package holds".
TEST(FirmwareInstallationRequest, AnExplicitSkipListReplacesTheDefaults) {
  nlohmann::json body = requestWithPackage();
  body["skipFiles"] = nlohmann::json::array();
  auto request = parseFirmwareInstallationRequest(body);
  ASSERT_TRUE(request) << request.error();
  EXPECT_TRUE(request->skipFiles.empty());
}

// The AL state numbers POST /api/devices/state uses, not a vocabulary of this procedure's own.
TEST(FirmwareInstallationRequest, ReadsEveryFinalState) {
  for (const auto state : {EtherCatState::Init, EtherCatState::PreOp, EtherCatState::Boot,
                           EtherCatState::SafeOp, EtherCatState::Op}) {
    nlohmann::json body = requestWithPackage();
    body["finalState"] = static_cast<unsigned>(state);
    auto request = parseFirmwareInstallationRequest(body);
    ASSERT_TRUE(request) << request.error();
    EXPECT_EQ(request->finalState, state) << mm::comm::toString(state);
  }
}

TEST(FirmwareInstallationRequest, RejectsAFinalStateThatIsNotAnAlState) {
  for (const unsigned value : {0u, 5u, 9u, 255u}) {
    nlohmann::json body = requestWithPackage();
    body["finalState"] = value;
    EXPECT_FALSE(parseFirmwareInstallationRequest(body)) << value;
  }
  nlohmann::json body = requestWithPackage();
  body["finalState"] = "preop";  // AL state is a number everywhere in this API, never a name.
  EXPECT_FALSE(parseFirmwareInstallationRequest(body));
}

// A name outside the convention is not an error: the bytes install, the package just is not cached.
TEST(FirmwareInstallationRequest, AcceptsAnUnrecognisedFilename) {
  nlohmann::json body = requestWithPackage();
  body["packageFilename"] = "my-firmware.zip";
  auto request = parseFirmwareInstallationRequest(body);
  ASSERT_TRUE(request) << request.error();
  EXPECT_FALSE(request->name.has_value());
  EXPECT_FALSE(request->package.empty());
}

TEST(FirmwareInstallationRequest, RejectsARequestWithNoPackageAtAll) {
  auto request = parseFirmwareInstallationRequest(nlohmann::json::object());
  ASSERT_FALSE(request);
  EXPECT_NE(request.error().find("packageContent"), std::string::npos) << request.error();
}

TEST(FirmwareInstallationRequest, RejectsContentThatIsNotBase64) {
  auto request =
      parseFirmwareInstallationRequest(nlohmann::json{{"packageContent", "not base64 at all!!"}});
  ASSERT_FALSE(request);
  EXPECT_NE(request.error().find("base64"), std::string::npos) << request.error();
}

TEST(FirmwareInstallationRequest, RejectsWrongFieldTypes) {
  nlohmann::json body = requestWithPackage();
  body["skipFiles"] = "SOMANET_CiA_402.xml.zip";
  EXPECT_FALSE(parseFirmwareInstallationRequest(body));

  body = requestWithPackage();
  body["cachePackage"] = "yes";
  EXPECT_FALSE(parseFirmwareInstallationRequest(body));
}

TEST(FirmwareInstallationDescriptor, StepsAndParametersLineUp) {
  const auto steps = firmwareInstallationSteps();
  ASSERT_EQ(steps.size(), 9u);
  EXPECT_EQ(steps.front().id, "package");
  EXPECT_EQ(steps.back().id, "final-state");

  // Every parameter the parser reads is advertised, so a client's form covers the whole request.
  std::vector<std::string> names;
  for (const auto& parameter : firmwareInstallationParameters()) {
    names.push_back(parameter.name);
  }
  EXPECT_NE(std::ranges::find(names, "packageContent"), names.end());
  EXPECT_NE(std::ranges::find(names, "packageFilename"), names.end());
  EXPECT_NE(std::ranges::find(names, "skipFiles"), names.end());
  EXPECT_NE(std::ranges::find(names, "cachePackage"), names.end());
  EXPECT_NE(std::ranges::find(names, "finalState"), names.end());
}

// ── Firmware installation: the whole body ──────────────────────────────────────────────────────

using mm::comm::FieldbusDriver;
using mm::node::DeviceManager;
using mm::node::firmwareCacheDir;
using mm::node::FirmwareInstallationRequest;
using mm::node::parseFirmwarePackageName;
using mm::node::runFirmwareInstallationProcedure;

// Driver double that models the two things the firmware body actually depends on and nothing else:
// an AL state that transitions really change, and a record of every FoE and EEPROM write. The other
// fakes in this file cannot be reused because their transitionToState is a no-op, which the body's
// own state read-back would reject at the very first step.
class FirmwareFakeDriver : public FieldbusDriver {
 public:
  std::map<std::string, std::vector<uint8_t>>
      written;                       ///< FoE writes, by the name sent on the wire.
  std::vector<std::string> removed;  ///< Targets of the fs-remove= pseudo-file.
  std::vector<uint8_t> siiWritten;   ///< Last EEPROM image written.
  std::vector<uint16_t> commanded;   ///< Every AL state commanded, in order.
  std::vector<std::chrono::steady_clock::duration> timeouts;  ///< The timeout of each command.

  /// Programmed failures, keyed by the name as sent over FoE.
  std::map<std::string, int> transientFailures;  ///< Fails this many times, then succeeds.
  std::string permanentFailure;                  ///< Always fails, permanently.

  std::expected<void, std::string> init() override { return {}; }
  std::expected<int, std::string> scan() override { return 1; }
  SlaveInfo slaveInfo(uint16_t) const override {
    SlaveInfo info{};
    info.vendorId = mm::node::kSynapticonVendorId;
    return info;
  }
  uint16_t slaveState(uint16_t) const override { return alStatus_; }
  std::expected<std::vector<SlaveStateRaw>, std::string> readStates(
      const std::vector<uint16_t>& positions) override {
    return std::vector<SlaveStateRaw>(positions.size(), SlaveStateRaw{alStatus_, 0});
  }
  void transitionToState(const std::vector<uint16_t>&, std::optional<EtherCatState>,
                         EtherCatState target, std::chrono::steady_clock::duration timeout,
                         std::chrono::steady_clock::duration, std::function<void()>,
                         std::function<bool()>) override {
    alStatus_ = static_cast<uint16_t>(target);
    commanded.push_back(alStatus_);
    timeouts.push_back(timeout);
  }

  std::expected<void, mm::comm::FoeError> writeFile(uint16_t, const std::string& name,
                                                    std::span<const uint8_t> data) override {
    if (name == permanentFailure) {
      return std::unexpected(
          mm::comm::makeFoeError(mm::comm::FoeErrorKind::BufferTooSmall, "FOEwrite", 1, name));
    }
    if (auto it = transientFailures.find(name); it != transientFailures.end() && it->second > 0) {
      --it->second;
      return std::unexpected(
          mm::comm::makeFoeError(mm::comm::FoeErrorKind::PacketMismatch, "FOEwrite", 1, name));
    }
    written[name] = std::vector<uint8_t>(data.begin(), data.end());
    return {};
  }
  std::expected<std::vector<uint8_t>, mm::comm::FoeError> readFile(
      uint16_t, const std::string& name, std::chrono::milliseconds) override {
    constexpr std::string_view kRemove = "fs-remove=";
    if (name.starts_with(kRemove)) {
      removed.push_back(name.substr(kRemove.size()));
      return std::vector<uint8_t>{};
    }
    return std::unexpected(
        mm::comm::makeFoeError(mm::comm::FoeErrorKind::FileNotFound, "FOEread", 1, name));
  }
  std::expected<void, std::string> writeSii(uint16_t, std::span<const uint8_t> data) override {
    siiWritten.assign(data.begin(), data.end());
    return {};
  }

  // --- unused stubs ---------------------------------------------------------
  std::expected<OdRead, std::string> readObjectDictionary(uint16_t) override { return OdRead{}; }
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
  std::expected<void, std::string> readRegister(uint16_t, uint16_t, std::span<uint8_t>) override {
    return {};
  }
  std::expected<void, std::string> writeRegister(uint16_t, uint16_t,
                                                 std::span<const uint8_t>) override {
    return {};
  }

 private:
  uint16_t alStatus_ = static_cast<uint16_t>(EtherCatState::PreOp);
};

/// Points mm::core::userCacheDir (and so firmwareCacheDir) at a throwaway directory, so a test that
/// exercises caching cannot write into the developer's real cache. Restores the variable on the way
/// out, whatever the test did.
// The environment calls below carry a NOLINT for concurrency-mt-unsafe: this fixture is the
// setenv half of the getenv/setenv race, on purpose. It is safe because it runs on the gtest main
// thread before the code under test starts any — which is also what lets the shipped binary's
// getenv callers be justified from the other side (see mm::core::envVar).
class CacheRedirect {
 public:
  CacheRedirect() {
    root_ = std::filesystem::temp_directory_path() /
            std::format("mm-firmware-test-{}", static_cast<const void*>(this));
    std::filesystem::create_directories(root_);
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    if (const char* previous = std::getenv(kVar)) {
      previous_ = previous;
    }
    setVar(root_.string());
  }
  ~CacheRedirect() {
    if (previous_) {
      setVar(*previous_);
    } else {
#ifdef _WIN32
      // NOLINTNEXTLINE(concurrency-mt-unsafe)
      _putenv_s(kVar, "");
#else
      // NOLINTNEXTLINE(concurrency-mt-unsafe)
      unsetenv(kVar);
#endif
    }
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }
  CacheRedirect(const CacheRedirect&) = delete;
  CacheRedirect& operator=(const CacheRedirect&) = delete;

 private:
  // Whichever variable userCacheDir consults on this platform.
#ifdef _WIN32
  static constexpr const char* kVar = "LOCALAPPDATA";
#elif defined(__APPLE__)
  static constexpr const char* kVar = "HOME";
#else
  static constexpr const char* kVar = "XDG_CACHE_HOME";
#endif
  static void setVar(const std::string& value) {
#ifdef _WIN32
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    _putenv_s(kVar, value.c_str());
#else
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    setenv(kVar, value.c_str(), 1);
#endif
  }
  std::filesystem::path root_;
  std::optional<std::string> previous_;
};

struct FirmwareBus {
  FirmwareFakeDriver* driver = nullptr;
  DeviceManager dm;

  FirmwareBus() {
    auto owned = std::make_unique<FirmwareFakeDriver>();
    driver = owned.get();
    EXPECT_TRUE(dm.init(std::move(owned)).has_value());
    EXPECT_TRUE(dm.scan().has_value());
  }
};

std::vector<uint8_t> circuloPackageBytes() {
  const std::filesystem::path path =
      std::filesystem::path(MM_NODE_TEST_DATA_DIR) / kCirculoPackageName;
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in) << "missing test fixture: " << path;
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

FirmwareInstallationRequest installRequest() {
  FirmwareInstallationRequest request;
  request.package = circuloPackageBytes();
  request.filename = std::string(kCirculoPackageName);
  request.name = parseFirmwarePackageName(kCirculoPackageName).value();
  for (std::string_view name : kDefaultSkippedFirmwareFiles) {
    request.skipFiles.emplace_back(name);
  }
  request.cachePackage = false;  // Redirected explicitly by the one test that wants it.
  request.bootWarmUp = std::chrono::milliseconds::zero();
  return request;
}

mm::node::ProgressStep stepNamed(const std::vector<mm::node::ProgressStep>& steps,
                                 std::string_view id) {
  for (const auto& step : steps) {
    if (step.id == id) {
      return step;
    }
  }
  ADD_FAILURE() << "no step '" << id << "'";
  return {};
}

TEST(FirmwareInstallationBody, WritesThePackageAndReturnsToPreOp) {
  FirmwareBus bus;
  ProgressReporter reporter(mm::node::firmwareInstallationSteps());
  std::stop_source source;

  auto result =
      runFirmwareInstallationProcedure(bus.dm, 1, reporter, source.get_token(), installRequest());
  ASSERT_TRUE(result) << result.error();

  // The bootloader is addressed by the short constant name, never the package's own long one.
  ASSERT_TRUE(bus.driver->written.contains("app_firmware.bin"));
  EXPECT_EQ(bus.driver->written["app_firmware.bin"].size(), 397312u);
  EXPECT_FALSE(bus.driver->written.contains("app_8500-04_motion-drive_v5.6.10_2332.bin"));
  EXPECT_FALSE(bus.driver->written.contains("com_firmware.bin"));  // None in this package.

  EXPECT_EQ(bus.driver->siiWritten.size(), 252u);
  // The two default-skipped extras are never written, and so never removed either.
  EXPECT_TRUE(bus.driver->removed.empty());
  EXPECT_EQ(bus.driver->written.size(), 1u);

  // INIT and BOOT on the way in, INIT and PRE-OP on the way out — BOOT is only ever paired with
  // INIT, which is what the AL state machine allows.
  EXPECT_EQ(bus.driver->commanded,
            (std::vector<uint16_t>{static_cast<uint16_t>(EtherCatState::Init),
                                   static_cast<uint16_t>(EtherCatState::Boot),
                                   static_cast<uint16_t>(EtherCatState::Init),
                                   static_cast<uint16_t>(EtherCatState::PreOp)}));

  for (const auto& step : reporter.steps()) {
    EXPECT_EQ(step.status, ProgressStatus::kSucceeded) << step.id;
  }
}

TEST(FirmwareInstallationBody, WaitsForARestartOnBothStepsOutOfBoot) {
  FirmwareBus bus;
  ProgressReporter reporter(mm::node::firmwareInstallationSteps());
  std::stop_source source;

  auto result =
      runFirmwareInstallationProcedure(bus.dm, 1, reporter, source.get_token(), installRequest());
  ASSERT_TRUE(result) << result.error();

  // INIT and BOOT on the way in, INIT and PRE-OP on the way out. The two steps in are ordinary
  // mailbox round trips. The two steps out each cover a restart: an Integro that got a com binary
  // restarts its netX at the step to INIT, and every SOMANET drive restarts at the step to PRE-OP.
  // After that reset the netX is off the bus for 6 to 19 s, measured on an ACTILINK-S.
  ASSERT_EQ(bus.driver->timeouts.size(), 4u);
  EXPECT_EQ(bus.driver->timeouts[0], bus.driver->timeouts[1]);
  EXPECT_EQ(bus.driver->timeouts[2], bus.driver->timeouts[3]);
  EXPECT_GT(bus.driver->timeouts[2], bus.driver->timeouts[0]);
  EXPECT_GE(bus.driver->timeouts[2], std::chrono::seconds(30));
}

TEST(FirmwareInstallationBody, StaysInBootWhenAsked) {
  FirmwareBus bus;
  ProgressReporter reporter(mm::node::firmwareInstallationSteps());
  std::stop_source source;

  auto request = installRequest();
  request.finalState = EtherCatState::Boot;
  auto result = runFirmwareInstallationProcedure(bus.dm, 1, reporter, source.get_token(), request);
  ASSERT_TRUE(result) << result.error();

  EXPECT_EQ(bus.driver->commanded.back(), static_cast<uint16_t>(EtherCatState::Boot));
  EXPECT_EQ(stepNamed(reporter.steps(), "final-state").status, ProgressStatus::kSucceeded);
}

TEST(FirmwareInstallationBody, RetriesATransientFoeFailure) {
  FirmwareBus bus;
  bus.driver->transientFailures["app_firmware.bin"] = 2;
  ProgressReporter reporter(mm::node::firmwareInstallationSteps());
  std::stop_source source;

  auto result =
      runFirmwareInstallationProcedure(bus.dm, 1, reporter, source.get_token(), installRequest());
  ASSERT_TRUE(result) << result.error();
  EXPECT_TRUE(bus.driver->written.contains("app_firmware.bin"));
}

// Three attempts per file, and a transient failure on every one of them ends the installation.
TEST(FirmwareInstallationBody, GivesUpAfterThreeTransientFoeFailures) {
  FirmwareBus bus;
  bus.driver->transientFailures["app_firmware.bin"] = 3;
  ProgressReporter reporter(mm::node::firmwareInstallationSteps());
  std::stop_source source;

  auto result =
      runFirmwareInstallationProcedure(bus.dm, 1, reporter, source.get_token(), installRequest());
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().find("gave up after 3 attempts"), std::string::npos) << result.error();
  EXPECT_FALSE(bus.driver->written.contains("app_firmware.bin"));
}

// A permanent failure must not be retried into and must not leave the device in the bootloader.
TEST(FirmwareInstallationBody, AbortsOnAPermanentFoeFailureButStillLeavesBoot) {
  FirmwareBus bus;
  bus.driver->permanentFailure = "app_firmware.bin";
  ProgressReporter reporter(mm::node::firmwareInstallationSteps());
  std::stop_source source;

  auto result =
      runFirmwareInstallationProcedure(bus.dm, 1, reporter, source.get_token(), installRequest());
  ASSERT_FALSE(result);
  EXPECT_EQ(stepNamed(reporter.steps(), "app-firmware").status, ProgressStatus::kFailed);
  EXPECT_EQ(bus.driver->commanded.back(), static_cast<uint16_t>(EtherCatState::PreOp))
      << "a failed install must not strand the device in BOOT";
}

// The descriptive extras are best effort: one that cannot be written is recorded and the firmware
// is installed anyway.
TEST(FirmwareInstallationBody, CarriesOnWhenAnExtraFileCannotBeWritten) {
  FirmwareBus bus;
  bus.driver->permanentFailure = "stack_image.svg.zip";
  ProgressReporter reporter(mm::node::firmwareInstallationSteps());
  std::stop_source source;

  auto request = installRequest();
  request.skipFiles.clear();  // Write everything, so the extras path runs at all.
  auto result = runFirmwareInstallationProcedure(bus.dm, 1, reporter, source.get_token(), request);
  ASSERT_TRUE(result) << result.error();

  const auto extras = stepNamed(reporter.steps(), "extra-files");
  EXPECT_EQ(extras.status, ProgressStatus::kSucceeded);
  EXPECT_NE(extras.value["stack_image.svg.zip"].get<std::string>().find("not written"),
            std::string::npos)
      << extras.value.dump();
  EXPECT_EQ(extras.value["SOMANET_CiA_402.xml.zip"], "written");
  // Each extra is removed before being rewritten, so a rewrite cannot mix with what was there.
  EXPECT_EQ(bus.driver->removed.size(), 2u);
  EXPECT_TRUE(bus.driver->written.contains("app_firmware.bin"));
}

TEST(FirmwareInstallationBody, CachesThePackageUnderItsName) {
  const CacheRedirect redirect;
  FirmwareBus bus;
  ProgressReporter reporter(mm::node::firmwareInstallationSteps());
  std::stop_source source;

  auto request = installRequest();
  request.cachePackage = true;
  auto result = runFirmwareInstallationProcedure(bus.dm, 1, reporter, source.get_token(), request);
  ASSERT_TRUE(result) << result.error();

  const std::filesystem::path cached = firmwareCacheDir() / kCirculoPackageName;
  ASSERT_TRUE(std::filesystem::exists(cached)) << cached;
  EXPECT_EQ(std::filesystem::file_size(cached), 608513u);
  EXPECT_EQ(stepNamed(reporter.steps(), "cache").value, cached.string());

  // The package's ESI goes beside it, named for the vendor it declares and the package's version.
  EXPECT_TRUE(
      std::filesystem::exists(mm::core::userCacheDir() / "esis" / "esi-000022d2-v5.6.10.xml"));
}

TEST(FirmwareInstallationBody, DoesNotCacheAPackageWithAnUnrecognisedName) {
  const CacheRedirect redirect;
  FirmwareBus bus;
  ProgressReporter reporter(mm::node::firmwareInstallationSteps());
  std::stop_source source;

  auto request = installRequest();
  request.cachePackage = true;
  request.filename = "my-firmware.zip";
  request.name.reset();
  auto result = runFirmwareInstallationProcedure(bus.dm, 1, reporter, source.get_token(), request);
  ASSERT_TRUE(result) << result.error();

  EXPECT_FALSE(std::filesystem::exists(firmwareCacheDir() / "my-firmware.zip"));
  EXPECT_NE(stepNamed(reporter.steps(), "cache").value.get<std::string>().find("not cached"),
            std::string::npos);
}

// A package already in the cache installs under whatever it is called. The SOMANET naming rule
// governs whether a package is *written* to the cache, never whether one can be read back — a file
// uploaded straight to `firmwares/` through /api/user-cache is named by whoever uploaded it.
TEST(FirmwareInstallationRequest, InstallsACachedPackageWithAnyName) {
  const CacheRedirect redirect;
  std::filesystem::create_directories(firmwareCacheDir());
  {
    std::ofstream out(firmwareCacheDir() / "my-firmware.zip", std::ios::binary);
    const auto bytes = circuloPackageBytes();
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  }

  auto request =
      parseFirmwareInstallationRequest(nlohmann::json{{"packageFilename", "my-firmware.zip"}});
  ASSERT_TRUE(request) << request.error();
  EXPECT_EQ(request->package.size(), 608513u);
  EXPECT_FALSE(request->name.has_value());
}

TEST(FirmwareInstallationRequest, ReportsAPackageThatIsNotCached) {
  const CacheRedirect redirect;
  auto request =
      parseFirmwareInstallationRequest(nlohmann::json{{"packageFilename", "absent.zip"}});
  ASSERT_FALSE(request);
  EXPECT_NE(request.error().find("firmware cache"), std::string::npos) << request.error();
}

// The API is unauthenticated and the filename is joined onto a server path, so a name that escapes
// the cache directory must be refused. Note the third case: it *is* a well-formed SOMANET package
// name — five underscore-separated fields — which is exactly why the grammar cannot be the guard.
TEST(FirmwareInstallationRequest, RefusesAFilenameThatEscapesTheCache) {
  const CacheRedirect redirect;
  for (const std::string name : {"../escape.zip", "sub/dir.zip", "..\\escape.zip",
                                 "package_../../etc_8500-04_motion-drive_v1.0.zip"}) {
    auto request = parseFirmwareInstallationRequest(nlohmann::json{{"packageFilename", name}});
    EXPECT_FALSE(request) << "accepted '" << name << "'";
  }
}

// The same filename on the *write* path: caching must not put request-controlled bytes outside the
// cache directory, and the install itself carries on regardless since caching is best effort.
TEST(FirmwareInstallationBody, RefusesToCacheUnderAnEscapingName) {
  const CacheRedirect redirect;
  FirmwareBus bus;
  ProgressReporter reporter(mm::node::firmwareInstallationSteps());
  std::stop_source source;

  auto request = installRequest();
  request.cachePackage = true;
  request.filename = "package_../../pwned_8500-04_motion-drive_v1.0.zip";
  request.name = parseFirmwarePackageName(request.filename).value();
  auto result = runFirmwareInstallationProcedure(bus.dm, 1, reporter, source.get_token(), request);
  ASSERT_TRUE(result) << result.error();

  EXPECT_NE(stepNamed(reporter.steps(), "cache").value.get<std::string>().find("not cached"),
            std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(firmwareCacheDir().parent_path().parent_path() / "pwned"));
  EXPECT_TRUE(bus.driver->written.contains("app_firmware.bin"));
}

constexpr std::string_view kActilinkPackageName =
    "package_ACTILINK_6000-01-2332-1_motion-drive_v5.6.10.zip";

// The ACTILINK package is the one with both binaries — the Circulo has no COM processor — so it is
// the only fixture that can exercise a second transfer at all.
FirmwareInstallationRequest actilinkRequest() {
  const std::filesystem::path path =
      std::filesystem::path(MM_NODE_TEST_DATA_DIR) / kActilinkPackageName;
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in) << "missing test fixture: " << path;

  FirmwareInstallationRequest request;
  request.package = {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  request.filename = std::string(kActilinkPackageName);
  for (std::string_view name : kDefaultSkippedFirmwareFiles) {
    request.skipFiles.emplace_back(name);
  }
  request.cachePackage = false;
  request.bootWarmUp = std::chrono::milliseconds::zero();
  return request;
}

TEST(FirmwareInstallationBody, WritesBothBinariesOfAPackageThatHasThem) {
  FirmwareBus bus;
  ProgressReporter reporter(mm::node::firmwareInstallationSteps());
  std::stop_source source;

  auto result =
      runFirmwareInstallationProcedure(bus.dm, 1, reporter, source.get_token(), actilinkRequest());
  ASSERT_TRUE(result) << result.error();
  EXPECT_TRUE(bus.driver->written.contains("app_firmware.bin"));
  EXPECT_TRUE(bus.driver->written.contains("com_firmware.bin"));
}

// ── Over SPoE ────────────────────────────────────────────────────────────────────────────────────
//
// The same installation against the SPoE driver and the fake drive. SPoE has no SII, and its drive
// applies the firmware on an explicit request that restarts it, not on leaving BOOT.

TEST(FirmwareInstallationOverSpoe, WritesBothBinariesRestartsTheDriveAndReachesPreOp) {
  mm::comm::testing::FakeSpoeServer server;
  server.setRestartTiming(std::chrono::milliseconds{200}, std::chrono::milliseconds{500});
  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::make_unique<mm::comm::spoe::SpoeFieldbusDriver>(
                          mm::comm::spoe::SpoeFieldbusDriverConfig{
                              .hosts = {"127.0.0.1"},
                              .port = server.port(),
                              .mode = mm::comm::spoe::SpoeMode::kControl,
                              .watchdogMs = 75,
                              .requestTimeout = std::chrono::milliseconds{500},
                              .connectTimeout = std::chrono::milliseconds{500},
                              .fileTimeout = std::chrono::milliseconds{2000}}))
                  .has_value());
  ASSERT_TRUE(dm.scan().has_value());

  ProgressReporter reporter(mm::node::firmwareInstallationSteps());
  std::stop_source source;
  auto result =
      runFirmwareInstallationProcedure(dm, 1, reporter, source.get_token(), actilinkRequest());
  ASSERT_TRUE(result) << result.error();

  EXPECT_TRUE(server.file("app_firmware.bin").has_value());
  EXPECT_TRUE(server.file("com_firmware.bin").has_value());
  EXPECT_EQ(server.firmwareUpdates(), 1);
  EXPECT_EQ(server.state(), mm::comm::testing::kSpoeStatePreOp);
  EXPECT_EQ(stepNamed(reporter.steps(), "activate-firmware").value,
            "the device restarted into the new firmware");
  for (const auto& step : reporter.steps()) {
    EXPECT_EQ(step.status, ProgressStatus::kSucceeded) << step.id;
  }
}

TEST(DeviceLocateOverSpoe, ACancelledRunStopsTheLeds) {
  mm::comm::testing::FakeSpoeServer server;
  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::make_unique<mm::comm::spoe::SpoeFieldbusDriver>(
                          mm::comm::spoe::SpoeFieldbusDriverConfig{.hosts = {"127.0.0.1"},
                                                                   .port = server.port()}))
                  .has_value());
  ASSERT_TRUE(dm.scan().has_value());
  const auto device = dm.deviceAt(1);
  ASSERT_TRUE(device);
  ASSERT_TRUE(device->supportsLocate());

  ProgressReporter reporter(mm::node::deviceLocateSteps());
  std::stop_source source;
  std::thread canceller([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    source.request_stop();
  });
  const auto result = mm::node::runDeviceLocateProcedure(*device, reporter, source.get_token(),
                                                         mm::node::DeviceLocateRequest{});
  canceller.join();
  EXPECT_FALSE(result.has_value());
  EXPECT_FALSE(server.locating());
}

// --- Plant identification procedure --------------------------------------------------------------

using mm::node::kPlantIdentificationProcedure;
using mm::node::parsePlantIdentificationRequest;
using mm::node::plantIdentificationParameters;
using mm::node::PlantIdentificationRequest;
using mm::node::plantIdentificationSteps;
using mm::node::PlantMeasurement;
using mm::node::PlantModelFit;
using mm::node::PlantModelFitFn;
using mm::node::runPlantIdentificationProcedure;

constexpr uint32_t kRatedTorqueMilliNm = 2000;

// The drive as a plant identification finds it: a rated torque to scale the recording by, a target
// torque to zero, and a recording on its files. The chirp settings and the HRD commands answer
// through the scripted OS command replies like every other command.
void programPlant(OsCommandFakeDriver& driver) {
  driver.programObject(Object::kMotorRatedTorque, 0, ObjectDataType::UNSIGNED32,
                       {static_cast<uint8_t>(kRatedTorqueMilliNm & 0xFF),
                        static_cast<uint8_t>(kRatedTorqueMilliNm >> 8), 0, 0});
  driver.programObject(Object::kTargetTorque, 0, ObjectDataType::INTEGER16, {0x10, 0x00});
}

// A system identification recording: one 6-byte sample per millisecond, velocity in rpm as Q15 and
// torque in per mille, both alternating in sign so that the means of the absolute values are what
// the amplitude checks see.
void recordPlant(OsCommandFakeDriver& driver, int count, double rpm, int16_t torquePermil) {
  std::vector<uint8_t> bytes;
  for (int i = 0; i < count; ++i) {
    const double sign = (i % 2 == 0) ? 1.0 : -1.0;
    const auto velocity = static_cast<int32_t>(sign * rpm * 32768.0);
    const auto torque = static_cast<int16_t>(sign * torquePermil);
    const auto v = static_cast<uint32_t>(velocity);
    const auto t = static_cast<uint16_t>(torque);
    bytes.insert(bytes.end(), {static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>(v >> 8),
                               static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24),
                               static_cast<uint8_t>(t & 0xFF), static_cast<uint8_t>(t >> 8)});
  }
  const std::string listing = std::format("hr_data0.bin, size: {}\n", bytes.size());
  driver.files["fs-getlist"] = std::vector<uint8_t>(listing.begin(), listing.end());
  driver.files["hr_data0.bin"] = std::move(bytes);
}

// A fit that records what it was asked and answers a fixed stable model.
struct RecordingFit {
  std::optional<PlantMeasurement> measurement;
  double startHz = 0.0;
  double endHz = 0.0;
  std::vector<double> denominator{0.01, 0.5};
  std::optional<std::string> refusal;

  PlantModelFitFn fn() {
    return [this](const PlantMeasurement& m, double f0,
                  double f1) -> std::expected<PlantModelFit, std::string> {
      measurement = m;
      startHz = f0;
      endHz = f1;
      if (refusal) {
        return std::unexpected(*refusal);
      }
      return PlantModelFit{.numerator = {1.0},
                           .denominator = denominator,
                           .bode = nlohmann::json{{"Frequency_Hz", {1.0, 2.0}}}};
    };
  }
};

PlantIdentificationRequest plantRequest() {
  auto request = parsePlantIdentificationRequest(validChirp());
  EXPECT_TRUE(request.has_value()) << request.error();
  return *request;
}

TEST(PlantIdentificationSteps, DeclaresTheRestoreBeforeTheFit) {
  // The drive is put back before the program computes, so the step order says so.
  auto steps = plantIdentificationSteps();
  ASSERT_EQ(steps.size(), 7u);
  EXPECT_EQ(steps[0].id, "prepare");
  EXPECT_EQ(steps[1].id, "release-brake");
  EXPECT_EQ(steps[2].id, "chirp");
  EXPECT_EQ(steps[3].id, "record");
  EXPECT_EQ(steps[4].id, "restore");
  EXPECT_EQ(steps[5].id, "fit");
  EXPECT_EQ(steps[6].id, "store");
}

TEST(ParsePlantIdentificationRequest, ArmsTheChirpAfterTheRecordingStartsAndPadsTheDuration) {
  auto request = parsePlantIdentificationRequest(validChirp());
  ASSERT_TRUE(request.has_value()) << request.error();
  EXPECT_EQ(request->chirp.start, somanet::SystemIdentificationStart::kAfterHrdStreamStart);
  EXPECT_EQ(request->chirp.transitionTimeMs, 5000u);
  EXPECT_EQ(request->streamDuration, std::chrono::milliseconds(5200));
}

TEST(ParsePlantIdentificationRequest, RefusesAChirpThatDoesNotFitTheRecording) {
  // 5900 ms is a legal chirp for the firmware and does not fit in the 6000 ms the drive records
  // once the padding is added. Refused here, before anything moves.
  auto body = validChirp();
  body["transitionTimeMs"] = 5900;
  auto request = parsePlantIdentificationRequest(body);
  ASSERT_FALSE(request.has_value());
  EXPECT_NE(request.error().find("5800"), std::string::npos) << request.error();
}

TEST(ParsePlantIdentificationRequest, IgnoresAStartField) {
  // The run arms the chirp itself. A client that copied a system identification body must not be
  // able to leave the chirp unarmed, or to start it immediately.
  auto body = validChirp();
  body["start"] = "none";
  auto request = parsePlantIdentificationRequest(body);
  ASSERT_TRUE(request.has_value()) << request.error();
  EXPECT_EQ(request->chirp.start, somanet::SystemIdentificationStart::kAfterHrdStreamStart);
}

TEST(PlantIdentificationParameters, AreTheChirpsWithoutAStartAndWithALowerTimeCeiling) {
  const auto parameters = plantIdentificationParameters();
  std::vector<std::string> names;
  for (const auto& parameter : parameters) {
    names.push_back(parameter.name);
    EXPECT_NE(parameter.name, "start");
    if (parameter.name == "transitionTimeMs") {
      EXPECT_EQ(parameter.maxValue, 5800);
    }
  }
  EXPECT_EQ(names,
            (std::vector<std::string>{"startFrequencyMilliHz", "targetFrequencyMilliHz",
                                      "targetAmplitudePermil", "transitionTimeMs", "signalType"}));
}

TEST(RunPlantIdentificationProcedure, ExcitesRecordsFitsStoresAndRestores) {
  OsCommandFakeDriver driver;
  programPlant(driver);
  Device device = makeDevice(driver);
  recordPlant(driver, 100, 60.0, 500);
  RecordingFit fit;
  ProgressReporter reporter(plantIdentificationSteps());

  auto result = runPlantIdentificationProcedure(device, reporter, std::stop_token{}, plantRequest(),
                                                fit.fn());
  ASSERT_TRUE(result.has_value()) << result.error();

  const auto steps = reporter.steps();
  for (auto id : {"prepare", "release-brake", "chirp", "record", "restore", "fit", "store"}) {
    const auto step = stepById(steps, id);
    ASSERT_TRUE(step.has_value()) << id;
    EXPECT_EQ(step->status, ProgressStatus::kSucceeded) << id;
  }

  // The fit got the recording in its units: 500 per mille of 2 Nm is 1 Nm, 60 rpm is 2π rad/s,
  // and the sweep in Hz.
  ASSERT_TRUE(fit.measurement.has_value());
  ASSERT_EQ(fit.measurement->torquesNm.size(), 100u);
  EXPECT_DOUBLE_EQ(fit.measurement->torquesNm[0], 1.0);
  EXPECT_DOUBLE_EQ(fit.measurement->torquesNm[1], -1.0);
  EXPECT_NEAR(fit.measurement->velocitiesRadPerSecond[0], 6.2832, 1e-3);
  EXPECT_DOUBLE_EQ(fit.startHz, 1.0);
  EXPECT_DOUBLE_EQ(fit.endHz, 100.0);

  // The model, its derived values, the chirp and the Bode data are the fit step's value.
  const auto fitted = stepById(steps, "fit");
  EXPECT_EQ(fitted->value.at("denominator"), (std::vector<double>{0.01, 0.5}));
  EXPECT_DOUBLE_EQ(fitted->value.at("inertia").get<double>(), 0.01);
  EXPECT_DOUBLE_EQ(fitted->value.at("friction").get<double>(), 0.5);
  EXPECT_EQ(fitted->value.at("chirp").at("durationMs"), 5000);
  EXPECT_EQ(fitted->value.at("bode").at("Frequency_Hz"), (std::vector<double>{1.0, 2.0}));
  EXPECT_TRUE(fitted->value.at("warning").is_null());

  // The file on the drive: numerator, denominator, chirp with the duration in seconds.
  ASSERT_EQ(driver.fileWrites.size(), 1u);
  EXPECT_EQ(driver.fileWrites[0].first, "plant_model.csv");
  const std::string csv(driver.fileWrites[0].second.begin(), driver.fileWrites[0].second.end());
  EXPECT_EQ(csv, "1\n0.01,0.5\n1,100,50,5,0\n");
  EXPECT_EQ(stepById(steps, "store")->value.at("content"), csv);

  // CST and a zero target torque before the enable.
  EXPECT_EQ(driver.store.at(OsCommandFakeDriver::key(Object::kTargetTorque, 0)),
            (std::vector<uint8_t>{0, 0}));
  EXPECT_TRUE(driver.wroteBefore(Object::kModeOfOperation, 0, Object::kControlword, 0));
  EXPECT_TRUE(driver.wroteBefore(Object::kTargetTorque, 0, Object::kControlword, 0));

  // Restored: the mode it found, the drive in Switched On, and the chirp disarmed last of all the
  // OS commands. The brake was the drive's to release, so it was never written.
  EXPECT_EQ(driver.store.at(OsCommandFakeDriver::key(Object::kModeOfOperation, 0)),
            std::vector<uint8_t>{8});
  EXPECT_EQ(driver.statusword, 0x0023);
  ASSERT_FALSE(driver.commands.empty());
  EXPECT_EQ(driver.commands.back()[0], 15);
  EXPECT_EQ(driver.commands.back()[1], 5);
  EXPECT_EQ(driver.commands.back()[5], 0);
  EXPECT_EQ(brakeWrites(driver), 0);
  EXPECT_EQ(stepById(steps, "release-brake")->value.at("brake"), "released by the drive");
}

TEST(RunPlantIdentificationProcedure, ReleasesAManualBrakeAfterTheEnableAndEngagesItFirst) {
  OsCommandFakeDriver driver;
  programPlant(driver);
  Device device = makeDevice(driver);
  recordPlant(driver, 100, 60.0, 500);
  driver.store[OsCommandFakeDriver::key(somanet::kBrakeOptions, somanet::kBrakeReleaseStrategy)] = {
      static_cast<uint8_t>(somanet::BrakeReleaseStrategy::kManualOutputVoltage)};
  RecordingFit fit;
  ProgressReporter reporter(plantIdentificationSteps());

  auto result = runPlantIdentificationProcedure(device, reporter, std::stop_token{}, plantRequest(),
                                                fit.fn());
  ASSERT_TRUE(result.has_value()) << result.error();

  // Released once the drive was enabled, and engaged again before the drive left Operation
  // Enabled: the last brake write precedes the last controlword write.
  EXPECT_EQ(driver.brakeStatusWrites,
            (std::vector<uint8_t>{static_cast<uint8_t>(somanet::BrakeStatus::kDisengaged),
                                  static_cast<uint8_t>(somanet::BrakeStatus::kEngaged)}));
  EXPECT_TRUE(
      driver.wroteBefore(Object::kControlword, 0, somanet::kBrakeOptions, somanet::kBrakeStatus));
  EXPECT_LT(driver.lastWriteIndex(somanet::kBrakeOptions, somanet::kBrakeStatus),
            driver.lastWriteIndex(Object::kControlword, 0));
  EXPECT_EQ(stepById(reporter.steps(), "release-brake")->value.at("brake"), "released manually");
}

TEST(RunPlantIdentificationProcedure, RestoresTheDriveBeforeAFitThatFails) {
  OsCommandFakeDriver driver;
  programPlant(driver);
  Device device = makeDevice(driver);
  recordPlant(driver, 100, 60.0, 500);
  RecordingFit fit;
  fit.refusal = "the auto-tuning program is not running";
  ProgressReporter reporter(plantIdentificationSteps());

  auto result = runPlantIdentificationProcedure(device, reporter, std::stop_token{}, plantRequest(),
                                                fit.fn());
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("not running"), std::string::npos) << result.error();

  const auto steps = reporter.steps();
  EXPECT_EQ(stepById(steps, "restore")->status, ProgressStatus::kSucceeded);
  EXPECT_EQ(stepById(steps, "fit")->status, ProgressStatus::kFailed);
  EXPECT_EQ(stepById(steps, "store")->status, ProgressStatus::kIdle);
  EXPECT_EQ(driver.statusword, 0x0023);
  EXPECT_TRUE(driver.fileWrites.empty());
}

TEST(RunPlantIdentificationProcedure, RefusesAnUnstableModelAndWritesNothing) {
  OsCommandFakeDriver driver;
  programPlant(driver);
  Device device = makeDevice(driver);
  recordPlant(driver, 100, 60.0, 500);
  RecordingFit fit;
  fit.denominator = {0.01, -0.5};
  ProgressReporter reporter(plantIdentificationSteps());

  auto result = runPlantIdentificationProcedure(device, reporter, std::stop_token{}, plantRequest(),
                                                fit.fn());
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("unstable"), std::string::npos) << result.error();
  EXPECT_EQ(stepById(reporter.steps(), "fit")->status, ProgressStatus::kFailed);
  EXPECT_TRUE(driver.fileWrites.empty());
}

TEST(RunPlantIdentificationProcedure, WarnsWhenTheMotionWasSmall) {
  OsCommandFakeDriver driver;
  programPlant(driver);
  Device device = makeDevice(driver);
  recordPlant(driver, 100, 30.0, 500);  // 30 rpm is 3.1 rad/s: enough to fit, less than ideal
  RecordingFit fit;
  ProgressReporter reporter(plantIdentificationSteps());

  auto result = runPlantIdentificationProcedure(device, reporter, std::stop_token{}, plantRequest(),
                                                fit.fn());
  ASSERT_TRUE(result.has_value()) << result.error();
  const auto steps = reporter.steps();
  EXPECT_FALSE(stepById(steps, "record")->value.at("warning").is_null());
  EXPECT_FALSE(stepById(steps, "fit")->value.at("warning").is_null());
}

TEST(RunPlantIdentificationProcedure, RefusesARecordingTheChirpDidNotExcite) {
  OsCommandFakeDriver driver;
  programPlant(driver);
  Device device = makeDevice(driver);
  recordPlant(driver, 100, 60.0, 0);  // the motor moved, the torque says the chirp never ran
  RecordingFit fit;
  ProgressReporter reporter(plantIdentificationSteps());

  auto result = runPlantIdentificationProcedure(device, reporter, std::stop_token{}, plantRequest(),
                                                fit.fn());
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("torque"), std::string::npos) << result.error();

  const auto steps = reporter.steps();
  EXPECT_EQ(stepById(steps, "record")->status, ProgressStatus::kFailed);
  EXPECT_EQ(stepById(steps, "restore")->status, ProgressStatus::kSucceeded);
  EXPECT_EQ(stepById(steps, "fit")->status, ProgressStatus::kIdle);
  EXPECT_FALSE(fit.measurement.has_value());
  EXPECT_EQ(driver.statusword, 0x0023);
}

TEST(RunPlantIdentificationProcedure, AFaultDuringTheChirpFailsTheRecordingAndRestores) {
  OsCommandFakeDriver driver;
  programPlant(driver);
  Device device = makeDevice(driver);
  recordPlant(driver, 100, 60.0, 500);
  // The recording's start is the ninth OS command: the HRD configure, the six chirp writes, the
  // arm, then the start. A fault on it is a fault while the chirp runs.
  driver.faultAfterCommands = 9;
  RecordingFit fit;
  ProgressReporter reporter(plantIdentificationSteps());

  auto result = runPlantIdentificationProcedure(device, reporter, std::stop_token{}, plantRequest(),
                                                fit.fn());
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("left Operation Enabled"), std::string::npos) << result.error();
  EXPECT_NE(result.error().find("OVERCURR"), std::string::npos) << result.error();
  EXPECT_EQ(stepById(reporter.steps(), "record")->status, ProgressStatus::kFailed);
  EXPECT_EQ(stepById(reporter.steps(), "restore")->status, ProgressStatus::kSucceeded);
}

TEST(RunPlantIdentificationProcedure, RefusesADriveWithNoRatedTorqueBeforeAnythingMoves) {
  OsCommandFakeDriver driver;
  driver.programObject(Object::kMotorRatedTorque, 0, ObjectDataType::UNSIGNED32, {0, 0, 0, 0});
  driver.programObject(Object::kTargetTorque, 0, ObjectDataType::INTEGER16, {0, 0});
  Device device = makeDevice(driver);
  RecordingFit fit;
  ProgressReporter reporter(plantIdentificationSteps());

  auto result = runPlantIdentificationProcedure(device, reporter, std::stop_token{}, plantRequest(),
                                                fit.fn());
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("0x6076"), std::string::npos) << result.error();
  EXPECT_EQ(stepById(reporter.steps(), "prepare")->status, ProgressStatus::kFailed);
  EXPECT_EQ(driver.commandWrites, 0);
  EXPECT_EQ(driver.statusword, 0x0040);
}

TEST(RunPlantIdentificationProcedure, FailsTheFitWhenNoFitIsConfigured) {
  OsCommandFakeDriver driver;
  programPlant(driver);
  Device device = makeDevice(driver);
  recordPlant(driver, 100, 60.0, 500);
  ProgressReporter reporter(plantIdentificationSteps());

  auto result = runPlantIdentificationProcedure(device, reporter, std::stop_token{}, plantRequest(),
                                                PlantModelFitFn{});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("not configured"), std::string::npos) << result.error();
  EXPECT_EQ(stepById(reporter.steps(), "restore")->status, ProgressStatus::kSucceeded);
}

TEST(RunPlantIdentificationProcedure, RestoresAfterCancellation) {
  OsCommandFakeDriver driver;
  programPlant(driver);
  Device device = makeDevice(driver);
  recordPlant(driver, 100, 60.0, 500);
  RecordingFit fit;
  ProgressReporter reporter(plantIdentificationSteps());

  std::stop_source source;
  source.request_stop();
  auto result = runPlantIdentificationProcedure(device, reporter, source.get_token(),
                                                plantRequest(), fit.fn());
  ASSERT_FALSE(result.has_value());
  EXPECT_FALSE(fit.measurement.has_value());
  EXPECT_EQ(driver.store.at(OsCommandFakeDriver::key(Object::kModeOfOperation, 0)),
            std::vector<uint8_t>{8});
}

// --- iC-MU encoder configuration --------------------------------------------------------------

// A drive whose OS command 0 reaches a model of one iC-MU: a write stores the byte and echoes it, a
// read returns it, and an I²C_COM from the iC-PVL copies an iC-PVL register into 0x60.
class IcMuFakeDriver : public OsCommandFakeDriver {
 public:
  std::array<uint8_t, 256> icMu{};
  std::array<uint8_t, 256> icPvl{};
  // The (address, value) of every register write, in order.
  std::vector<std::pair<uint8_t, uint8_t>> registerWrites;
  // Whether the next OS command 0 write echoes another value than it was given.
  std::optional<uint8_t> wrongEcho;

  void programIcMuDrive(std::string_view firmware, uint8_t sensorPort, uint8_t singleturnBits,
                        uint8_t multiturnBits) {
    programDrive();
    std::vector<uint8_t> version(firmware.begin(), firmware.end());
    programObject(0x100A, 0, ObjectDataType::VISIBLE_STRING, version);
    for (const uint16_t index : {uint16_t{0x2110}, uint16_t{0x2112}}) {
      const bool mine = index == 0x2110;
      programObject(index, 1, ObjectDataType::UNSIGNED8,
                    {static_cast<uint8_t>(mine ? sensorPort : 0)});
      programObject(index, 2, ObjectDataType::UNSIGNED8, {static_cast<uint8_t>(mine ? 4 : 0)});
      programObject(index, 3, ObjectDataType::UNSIGNED32, {0, 0, 8, 0});
      programObject(index, 5, ObjectDataType::UNSIGNED8, {0});
      programObject(index, 10, ObjectDataType::UNSIGNED8, {mine ? multiturnBits : uint8_t{0}});
      programObject(index, 12, ObjectDataType::UNSIGNED8, {mine ? singleturnBits : uint8_t{0}});
    }
    programObject(0x2111, 1, ObjectDataType::UNSIGNED32, {0x00, 0x00, 0x04, 0x00});
    // Encoder 1 commutates, so it is on the motor shaft.
    programObject(0x2010, 12, ObjectDataType::UNSIGNED8, {1});
    programObject(0x2011, 5, ObjectDataType::UNSIGNED8, {1});
    programObject(0x2012, 9, ObjectDataType::UNSIGNED8, {1});
    programObject(0x6091, 0, ObjectDataType::UNSIGNED8, {2});
    programObject(0x6091, 1, ObjectDataType::UNSIGNED32, {1, 0, 0, 0});
    programObject(0x6091, 2, ObjectDataType::UNSIGNED32, {1, 0, 0, 0});
    icMu[0x74] = 0x15;
    icMu[0x11] = 0xA5;
  }

  std::expected<std::vector<uint8_t>, std::string> readSdo(uint16_t slave, uint16_t index,
                                                           uint8_t subindex) override {
    if (index != kOsCommand || subindex != 3 || awaitingCommand || commands.empty() ||
        commands.back()[0] != 0) {
      return OsCommandFakeDriver::readSdo(slave, index, subindex);
    }
    awaitingCommand = true;
    const auto& command = commands.back();
    const uint8_t address = command[3];
    if (command[2] != 0) {
      registerWrites.emplace_back(address, command[4]);
      if (address == 0x75) {
        if (command[4] == 0x0A && icMu[0x5E] == 0xC3) {
          icMu[0x60] = icPvl[icMu[0x5B]];
        }
        return std::vector<uint8_t>{1, 0, command[4], 0, 0, 0, 0, 0};
      }
      icMu[address] = command[4];
      if (wrongEcho) {
        const uint8_t echo = *wrongEcho;
        wrongEcho.reset();
        return std::vector<uint8_t>{1, 0, echo, 0, 0, 0, 0, 0};
      }
    }
    return std::vector<uint8_t>{1, 0, icMu[address], 0, 0, 0, 0, 0};
  }
};

mm::node::IcMuEncoderConfigurationRequest icMuRequest() {
  return {.encoderOrdinal = somanet::EncoderOrdinal::kEncoder1,
          .batteryMode = std::nullopt,
          .waits = {.icPvlEeprom = kNoDelay, .icPvlReboot = kNoDelay, .absReset = kNoDelay}};
}

// Whether every OS command sent so far is an encoder register read: nothing was written and no
// status bit was ignored.
bool onlyRegisterReads(const IcMuFakeDriver& driver) {
  return std::ranges::all_of(driver.commands, [](const auto& c) { return c[0] == 0 && c[2] == 0; });
}

// The OS commands after the reads that validate makes.
std::vector<std::vector<uint8_t>> afterReads(const IcMuFakeDriver& driver) {
  auto first = std::ranges::find_if(driver.commands,
                                    [](const auto& c) { return !(c[0] == 0 && c[2] == 0); });
  return {first, driver.commands.end()};
}

Device makeIcMuDevice(IcMuFakeDriver& driver, std::string_view firmware, uint8_t sensorPort,
                      uint8_t singleturnBits, uint8_t multiturnBits) {
  driver.programIcMuDrive(firmware, sensorPort, singleturnBits, multiturnBits);
  Device device(1, driver);
  EXPECT_TRUE(device.initializeParameters().has_value());
  return device;
}

using Writes = std::vector<std::pair<uint8_t, uint8_t>>;

// The singleturn RAM configuration of a Circulo 7, as the old per-product table holds it.
const Writes kCirculo7SingleturnRam = {
    {0x00, 0x00}, {0x05, 0x88}, {0x06, 0x00}, {0x0B, 0x02}, {0x0C, 0x6C}, {0x0D, 0xC0},
    {0x0E, 0x04}, {0x0F, 0x05}, {0x10, 0x00}, {0x11, 0x05}, {0x12, 0x00}, {0x13, 0xFF},
    {0x14, 0x0F}, {0x15, 0x13}, {0x16, 0x10}, {0x17, 0x02}, {0x18, 0x00},
};

TEST(RunIcMuEncoderConfigurationProcedure, WritesTheSingleturnTableOfACirculo7) {
  IcMuFakeDriver driver;
  Device device = makeIcMuDevice(driver, "v5.6.11", 2, 19, 0);
  ProgressReporter reporter(mm::node::icMuEncoderConfigurationSteps());

  auto result = mm::node::runIcMuEncoderConfigurationProcedure(device, reporter, std::stop_token{},
                                                               icMuRequest());
  ASSERT_TRUE(result.has_value()) << result.error();

  Writes expected = kCirculo7SingleturnRam;
  expected.emplace_back(0x75, 0x01);  // WRITE_ALL
  expected.emplace_back(0x75, 0x03);  // ABS_RESET
  EXPECT_EQ(driver.registerWrites, expected);

  // OS command 14 ignores the status bits before the first write and restores them after the last.
  ASSERT_GE(driver.commandIds.size(), 2u);
  // Validate only reads (BANKSEL); the first command after it ignores the status bits.
  ASSERT_FALSE(afterReads(driver).empty());
  EXPECT_EQ(afterReads(driver).front()[0], 14);
  EXPECT_EQ(afterReads(driver).front()[1], 0x03);  // encoder 1, ignore
  EXPECT_EQ(driver.commandIds.back(), 14);
  EXPECT_EQ(driver.commands.back()[1], 0x02);  // encoder 1, stop ignoring

  // The restore file is named by the sensor port and holds 33 pairs.
  ASSERT_EQ(driver.fileWrites.size(), 1u);
  EXPECT_EQ(driver.fileWrites[0].first, "circulo_integrated_encoder_2_config.bin");
  ASSERT_EQ(driver.fileWrites[0].second.size(), 66u);
  EXPECT_EQ(driver.fileWrites[0].second[0x11 * 2], 0x11);
  EXPECT_EQ(driver.fileWrites[0].second[0x11 * 2 + 1], 0x05);

  const auto steps = reporter.steps();
  auto step = [&steps](std::string_view id) {
    return *std::ranges::find(steps, id, &mm::node::ProgressStep::id);
  };
  EXPECT_EQ(step(mm::node::kIcMuVerifyStep).value["state"], "configured");
  EXPECT_EQ(step(mm::node::kIcPvlRegistersStep).status, ProgressStatus::kIdle);
  EXPECT_EQ(step(mm::node::kIcMuSynchronisationStep).status, ProgressStatus::kIdle);
  EXPECT_EQ(step(mm::node::kIcMuRestoreStatusBitsStep).status, ProgressStatus::kSucceeded);
}

TEST(RunIcMuEncoderConfigurationProcedure, WritesTheMultiturnSequenceOfACirculo9) {
  IcMuFakeDriver driver;
  Device device = makeIcMuDevice(driver, "v5.6.11", 1, 20, 18);
  driver.icPvl[0x0D] = 0x02;  // SYNC 2, 90°
  ProgressReporter reporter(mm::node::icMuEncoderConfigurationSteps());

  auto result = mm::node::runIcMuEncoderConfigurationProcedure(device, reporter, std::stop_token{},
                                                               icMuRequest());
  ASSERT_TRUE(result.has_value()) << result.error();

  const std::array<uint8_t, 6> settings = {0x28, 0x09, 0x3F, 0x5D, 0x50, 0x30};
  const std::array<uint8_t, 5> reserved{};
  auto icPvlCommand = [](uint8_t command) {
    return Writes{{0x5E, 0xC2}, {0x5C, 0x60},    {0x5B, 0x11},
                  {0x5D, 0x60}, {0x60, command}, {0x75, 0x0A}};
  };
  auto icPvlWrite = [](uint8_t reg, uint8_t value) {
    return Writes{{0x5E, 0xC2}, {0x5C, 0x60},  {0x5B, reg},
                  {0x5D, 0x60}, {0x60, value}, {0x75, 0x0A}};
  };
  Writes expected = {
      {0x00, 0x00}, {0x05, 0x88},
      {0x06, 0x00}, {0x0B, 0x02},
      {0x0C, 0x6C}, {0x0D, 0xC0},
      {0x0E, 0x44}, {0x0F, 0x06},
      {0x10, 0x6F}, {0x11, 0x18},
      {0x12, 0x00}, {0x13, 0xFF},
      {0x14, 0x0F}, {0x15, 0x13},
      {0x16, 0x10}, {0x17, 0x02},
      {0x18, 0x00}, {0x5E, 0xA0},
      {0x5C, 0x60}, {0x5B, 0x40},
      {0x5D, 0x6C}, {0x60, 0x28},
      {0x61, 0x09}, {0x62, 0x3F},
      {0x63, 0x5D}, {0x64, 0x50},
      {0x65, 0x30}, {0x66, somanet::icPvlCrc8(settings)},
      {0x67, 0x00}, {0x68, 0x00},
      {0x69, 0x00}, {0x6A, 0x00},
      {0x6B, 0x00}, {0x6C, somanet::icPvlCrc8(reserved)},
      {0x75, 0x0A},
  };
  auto append = [&expected](const Writes& more) {
    expected.insert(expected.end(), more.begin(), more.end());
  };
  append(icPvlCommand(0x03));  // REBOOT
  append({{0x75, 0x01}});      // WRITE_ALL
  append(icPvlCommand(0x05));  // SCLR
  append({{0x75, 0x03}});      // ABS_RESET
  // The synchronisation: SPO_MT cleared, the position read, the SYNC bits read.
  append({{0x0F, 0x06}, {0x75, 0x03}});
  append(icPvlWrite(0x05, 0xB0));
  append({{0x5E, 0xC3}, {0x5C, 0x60}, {0x5B, 0x0D}, {0x5D, 0x60}, {0x75, 0x0A}});
  // A raw position of 0x40000 is a quarter turn of a 20-bit ring, 900, and SYNC 2 is 900 too.
  append({{0x0F, 0x06}});
  append(icPvlWrite(0x05, 0x30));
  append({{0x75, 0x01}});
  append(icPvlCommand(0x05));
  append({{0x75, 0x03}});
  EXPECT_EQ(driver.registerWrites, expected);

  const auto steps = reporter.steps();
  auto step = [&steps](std::string_view id) {
    return *std::ranges::find(steps, id, &mm::node::ProgressStep::id);
  };
  EXPECT_EQ(step(mm::node::kIcMuValidateStep).value["batteryMode"], 0x5D);
  EXPECT_EQ(step(mm::node::kIcMuFaultResetStep).value["faulted"], false);
  EXPECT_EQ(step(mm::node::kIcMuSynchronisationStep).value["icPvlSync"], 2);
  EXPECT_EQ(step(mm::node::kIcMuVerifyStep).value["expectedOutputRegister"], 0x18);
  EXPECT_EQ(driver.fileWrites.at(0).first, "circulo_integrated_encoder_1_config.bin");
}

TEST(RunIcMuEncoderConfigurationProcedure, ANamedBatteryModeReplacesTheDefault) {
  IcMuFakeDriver driver;
  Device device = makeIcMuDevice(driver, "v5.6.11", 1, 19, 18);
  ProgressReporter reporter(mm::node::icMuEncoderConfigurationSteps());
  auto request = icMuRequest();
  request.batteryMode = 0x50;

  auto result =
      mm::node::runIcMuEncoderConfigurationProcedure(device, reporter, std::stop_token{}, request);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_NE(std::ranges::find(driver.registerWrites, std::pair<uint8_t, uint8_t>{0x63, 0x50}),
            driver.registerWrites.end());
}

TEST(RunIcMuEncoderConfigurationProcedure, AWrongEchoFailsAndStillRestoresTheStatusBits) {
  IcMuFakeDriver driver;
  Device device = makeIcMuDevice(driver, "v5.6.11", 2, 19, 0);
  driver.wrongEcho = 0x01;  // the first write is 0x00 into register 0x00
  ProgressReporter reporter(mm::node::icMuEncoderConfigurationSteps());

  auto result = mm::node::runIcMuEncoderConfigurationProcedure(device, reporter, std::stop_token{},
                                                               icMuRequest());
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("register 0x00"), std::string::npos) << result.error();
  EXPECT_EQ(driver.registerWrites.size(), 1u);
  EXPECT_EQ(driver.commandIds.back(), 14);
  EXPECT_TRUE(driver.fileWrites.empty());
}

TEST(RunIcMuEncoderConfigurationProcedure, RefusesBeforeWritingAnything) {
  struct Case {
    const char* name;
    const char* firmware;
    uint8_t sensorPort;
    uint8_t singleturnBits;
    uint8_t multiturnBits;
    uint16_t statusword;
  };
  const std::array cases = {
      Case{"old firmware", "v4.4.1", 1, 19, 0, 0x0040},
      Case{"unknown ring", "v5.6.11", 1, 17, 0, 0x0040},
      Case{"no sensor port", "v5.6.11", 0, 19, 0, 0x0040},
      Case{"operation enabled", "v5.6.11", 1, 19, 0, 0x0027},
      Case{"Jasper multiturn", "v10.0.0-jasper.5", 1, 19, 18, 0x0040},
  };
  for (const auto& c : cases) {
    SCOPED_TRACE(c.name);
    IcMuFakeDriver driver;
    Device device =
        makeIcMuDevice(driver, c.firmware, c.sensorPort, c.singleturnBits, c.multiturnBits);
    driver.store[OsCommandFakeDriver::key(Object::kStatusword, 0)] = {
        static_cast<uint8_t>(c.statusword & 0xFF), static_cast<uint8_t>(c.statusword >> 8)};
    ProgressReporter reporter(mm::node::icMuEncoderConfigurationSteps());

    auto result = mm::node::runIcMuEncoderConfigurationProcedure(device, reporter,
                                                                 std::stop_token{}, icMuRequest());
    EXPECT_FALSE(result.has_value());
    EXPECT_TRUE(onlyRegisterReads(driver));
    EXPECT_EQ(reporter.steps()[0].status, ProgressStatus::kFailed);
  }
}

TEST(RunIcMuEncoderConfigurationProcedure, AnExternalIcMuIsConfiguredWithoutARestoreFile) {
  IcMuFakeDriver driver;
  // Multiturn bits on port 3 do not take the iC-PVL path: only port 1 has an iC-PVL.
  Device device = makeIcMuDevice(driver, "v5.6.11", 3, 19, 18);
  ProgressReporter reporter(mm::node::icMuEncoderConfigurationSteps());

  auto result = mm::node::runIcMuEncoderConfigurationProcedure(device, reporter, std::stop_token{},
                                                               icMuRequest());
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_TRUE(driver.fileWrites.empty());
  const auto steps = reporter.steps();
  auto step = [&steps](std::string_view id) {
    return *std::ranges::find(steps, id, &mm::node::ProgressStep::id);
  };
  EXPECT_EQ(step(mm::node::kIcMuValidateStep).value["path"], "singleturn");
  EXPECT_TRUE(step(mm::node::kIcMuRestoreFileStep).value["file"].is_null());
  EXPECT_EQ(step(mm::node::kIcPvlRegistersStep).status, ProgressStatus::kIdle);
}

TEST(RunIcMuEncoderConfigurationProcedure, RefusesAnotherChipOnAnExternalPort) {
  IcMuFakeDriver driver;
  Device device = makeIcMuDevice(driver, "v5.6.11", 3, 19, 0);
  driver.icMu[0x74] = 0x30;  // no iC-MU part code
  ProgressReporter reporter(mm::node::icMuEncoderConfigurationSteps());

  auto result = mm::node::runIcMuEncoderConfigurationProcedure(device, reporter, std::stop_token{},
                                                               icMuRequest());
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("names no iC-MU part"), std::string::npos) << result.error();
  EXPECT_TRUE(driver.registerWrites.empty());
  // Only the reads of BANKSEL and register 0x74 went out: nothing was written or ignored.
  EXPECT_TRUE(onlyRegisterReads(driver));
}

TEST(RunIcMuEncoderConfigurationProcedure, JasperTakesTheSingleturnPath) {
  IcMuFakeDriver driver;
  Device device = makeIcMuDevice(driver, "v10.0.0-jasper.5", 1, 19, 0);
  ProgressReporter reporter(mm::node::icMuEncoderConfigurationSteps());

  auto result = mm::node::runIcMuEncoderConfigurationProcedure(device, reporter, std::stop_token{},
                                                               icMuRequest());
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_NE(std::ranges::find(driver.registerWrites, std::pair<uint8_t, uint8_t>{0x0B, 0x02}),
            driver.registerWrites.end());
}

TEST(RunIcMuEncoderConfigurationProcedure, AnEncoderWithoutARoleNeedsABatteryMode) {
  IcMuFakeDriver driver;
  Device device = makeIcMuDevice(driver, "v5.6.11", 1, 19, 18);
  for (const auto& [index, subindex] :
       {std::pair<uint16_t, uint8_t>{0x2010, 12}, {0x2011, 5}, {0x2012, 9}}) {
    driver.store[OsCommandFakeDriver::key(index, subindex)] = {0};
  }
  ProgressReporter reporter(mm::node::icMuEncoderConfigurationSteps());

  auto result = mm::node::runIcMuEncoderConfigurationProcedure(device, reporter, std::stop_token{},
                                                               icMuRequest());
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("assign it"), std::string::npos) << result.error();
  EXPECT_TRUE(onlyRegisterReads(driver));
}

TEST(ParseIcMuEncoderConfigurationRequest, RequiresTheEncoderAndKnowsTheBatteryModes) {
  EXPECT_FALSE(mm::node::parseIcMuEncoderConfigurationRequest(osCommandBody("{}")));
  auto plain =
      mm::node::parseIcMuEncoderConfigurationRequest(osCommandBody(R"({"encoderOrdinal": 2})"));
  ASSERT_TRUE(plain) << plain.error();
  EXPECT_EQ(plain->encoderOrdinal, somanet::EncoderOrdinal::kEncoder2);
  EXPECT_EQ(plain->batteryMode, std::nullopt);
  auto named = mm::node::parseIcMuEncoderConfigurationRequest(
      osCommandBody(R"({"encoderOrdinal": 1, "batteryMode": "36-microamperes"})"));
  ASSERT_TRUE(named) << named.error();
  EXPECT_EQ(named->batteryMode, 0x48);
  auto fallback = mm::node::parseIcMuEncoderConfigurationRequest(
      osCommandBody(R"({"encoderOrdinal": 1, "batteryMode": "default"})"));
  ASSERT_TRUE(fallback) << fallback.error();
  EXPECT_EQ(fallback->batteryMode, std::nullopt);
  EXPECT_FALSE(mm::node::parseIcMuEncoderConfigurationRequest(
      osCommandBody(R"({"encoderOrdinal": 1, "batteryMode": "5g"})")));
}

// --- iC-MU calibration ---------------------------------------------------------------------------

TEST(ParseIcMuCalibrationRequest, ReadsEveryField) {
  EXPECT_FALSE(mm::node::parseIcMuCalibrationRequest(osCommandBody("{}")));
  auto request = mm::node::parseIcMuCalibrationRequest(osCommandBody(
      R"({"encoderOrdinal": 2, "ringPosition": "outer", "measurementOnly": true,
          "activateHealthMonitoring": true, "ringVelocity": 45})"));
  ASSERT_TRUE(request) << request.error();
  EXPECT_EQ(request->encoderOrdinal, somanet::EncoderOrdinal::kEncoder2);
  EXPECT_EQ(request->ringPosition, somanet::IcMuRingPosition::kOuter);
  EXPECT_TRUE(request->measurementOnly);
  EXPECT_TRUE(request->activateHealthMonitoring);
  EXPECT_EQ(request->ringVelocityRpm, 45);

  EXPECT_FALSE(mm::node::parseIcMuCalibrationRequest(
      osCommandBody(R"({"encoderOrdinal": 1, "ringPosition": "middle"})")));
  EXPECT_FALSE(mm::node::parseIcMuCalibrationRequest(
      osCommandBody(R"({"encoderOrdinal": 1, "ringVelocity": 0})")));
}

TEST(IcMuCalibrationParameters, OnlyTheEncoderOrdinalIsRequired) {
  for (const auto& parameter : mm::node::icMuCalibrationParameters()) {
    EXPECT_EQ(parameter.required(), parameter.name == "encoderOrdinal") << parameter.name;
  }
}

TEST(IcMuCalibrationSteps, ListEveryIterationThenTheValidationAndTheRestore) {
  const auto steps = mm::node::icMuCalibrationSteps();
  std::vector<std::string> ids;
  for (const auto& step : steps) {
    ids.push_back(step.id);
  }
  ASSERT_GE(ids.size(), 5u + 13u);
  EXPECT_EQ(ids[0], "validate");
  EXPECT_EQ(ids[5], "iteration-1");
  EXPECT_EQ(ids[5 + 12], "iteration-13");
  EXPECT_EQ(ids[5 + 13], "validation");
  EXPECT_EQ(ids.back(), "restore");
}

#ifdef MM_HAVE_MU_3SL

// The objects the calibration reads before the motor moves, on top of the iC-MU drive: a 19-bit
// position encoder, rpm, no gearbox, a feed constant of 1, and limits of ±10 turns.
void programCalibrationObjects(IcMuFakeDriver& driver) {
  auto u32 = [](uint32_t v) {
    return std::vector<uint8_t>{static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8),
                                static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
  };
  driver.programObject(0x6502, 0, ObjectDataType::UNSIGNED32, u32(0x0000006D));
  driver.programObject(0x6092, 0, ObjectDataType::UNSIGNED8, {2});
  driver.programObject(0x6092, 1, ObjectDataType::UNSIGNED32, u32(1));
  driver.programObject(0x6092, 2, ObjectDataType::UNSIGNED32, u32(1));
  driver.programObject(0x60A9, 0, ObjectDataType::UNSIGNED32, u32(0x00B44700));
  driver.programObject(0x607D, 0, ObjectDataType::UNSIGNED8, {2});
  driver.programObject(0x607D, 1, ObjectDataType::INTEGER32,
                       u32(static_cast<uint32_t>(-10 * 524288)));
  driver.programObject(0x607D, 2, ObjectDataType::INTEGER32, u32(10 * 524288));
  driver.programObject(0x6064, 0, ObjectDataType::INTEGER32, u32(0));
  driver.programObject(0x6080, 0, ObjectDataType::UNSIGNED32, u32(3000));
  driver.programObject(0x606B, 0, ObjectDataType::INTEGER32, u32(0));
  driver.programObject(0x6083, 0, ObjectDataType::UNSIGNED32, u32(1000));
  driver.programObject(0x6084, 0, ObjectDataType::UNSIGNED32, u32(1000));
  driver.programObject(0x60FF, 0, ObjectDataType::INTEGER32, u32(0));
}

Device makeCalibrationDevice(IcMuFakeDriver& driver, uint8_t sensorPort, uint8_t singleturnBits,
                             const std::function<void(IcMuFakeDriver&)>& change = {}) {
  driver.programIcMuDrive("v5.6.11", sensorPort, singleturnBits, 0);
  programCalibrationObjects(driver);
  if (change) {
    change(driver);
  }
  Device device(1, driver);
  EXPECT_TRUE(device.initializeParameters().has_value());
  return device;
}

mm::node::IcMuCalibrationRequest calibrationRequest() {
  mm::node::IcMuCalibrationRequest request;
  request.encoderOrdinal = somanet::EncoderOrdinal::kEncoder1;
  return request;
}

// Runs the calibration and expects validate to refuse with @p message before anything moves.
void expectRefusal(Device& device, IcMuFakeDriver& driver,
                   const mm::node::IcMuCalibrationRequest& request, std::string_view message) {
  ProgressReporter reporter(mm::node::icMuCalibrationSteps());
  auto result = mm::node::runIcMuCalibrationProcedure(device, reporter, std::stop_token{}, request);
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find(message), std::string::npos) << result.error();
  EXPECT_EQ(reporter.steps()[0].status, ProgressStatus::kFailed);
  EXPECT_TRUE(driver.registerWrites.empty());
  // No controlword was written, so the drive was never enabled.
  EXPECT_EQ(std::ranges::count_if(driver.writeLog,
                                  [](const auto& w) { return w.first == Object::kControlword; }),
            0);
}

TEST(RunIcMuCalibrationProcedure, RefusesAnExternalEncoderWithoutARingPosition) {
  IcMuFakeDriver driver;
  Device device = makeCalibrationDevice(driver, 3, 19);
  expectRefusal(device, driver, calibrationRequest(), "needs 'ringPosition'");
}

TEST(RunIcMuCalibrationProcedure, RefusesARingPositionOnAnIntegratedPort) {
  IcMuFakeDriver driver;
  Device device = makeCalibrationDevice(driver, 1, 19);
  auto request = calibrationRequest();
  request.ringPosition = somanet::IcMuRingPosition::kOuter;
  expectRefusal(device, driver, request, "always inner");
}

TEST(RunIcMuCalibrationProcedure, RefusesARingWithoutStartValues) {
  IcMuFakeDriver driver;
  // 16 master pole pairs on the outer ring: nobody measured that row, and no placeholder exists.
  Device device = makeCalibrationDevice(driver, 2, 18);
  expectRefusal(device, driver, calibrationRequest(), "no start values exist");
}

TEST(RunIcMuCalibrationProcedure, RefusesAnEncoderThatIsNeitherPositionNorVelocitySource) {
  IcMuFakeDriver driver;
  Device device = makeCalibrationDevice(driver, 1, 19, [](IcMuFakeDriver& d) {
    d.store[OsCommandFakeDriver::key(0x2012, 9)] = {2};
    d.store[OsCommandFakeDriver::key(0x2011, 5)] = {2};
  });
  expectRefusal(device, driver, calibrationRequest(), "neither the position source");
}

TEST(RunIcMuCalibrationProcedure, RefusesWithoutACommutationSource) {
  IcMuFakeDriver driver;
  Device device = makeCalibrationDevice(driver, 1, 19, [](IcMuFakeDriver& d) {
    d.store[OsCommandFakeDriver::key(0x2010, 12)] = {0};
  });
  expectRefusal(device, driver, calibrationRequest(), "commutation source");
}

TEST(RunIcMuCalibrationProcedure, RefusesAFeedConstantOfZero) {
  IcMuFakeDriver driver;
  Device device = makeCalibrationDevice(driver, 1, 19, [](IcMuFakeDriver& d) {
    d.store[OsCommandFakeDriver::key(0x6092, 1)] = {0, 0, 0, 0};
  });
  expectRefusal(device, driver, calibrationRequest(), "feed constant");
}

TEST(RunIcMuCalibrationProcedure, RefusesADriveInOperationEnabled) {
  IcMuFakeDriver driver;
  Device device = makeCalibrationDevice(driver, 1, 19, [](IcMuFakeDriver& d) {
    d.store[OsCommandFakeDriver::key(Object::kStatusword, 0)] = {0x27, 0x00};
  });
  expectRefusal(device, driver, calibrationRequest(), "Operation Enabled");
}

TEST(RunIcMuCalibrationProcedure, RefusesLimitsTooNarrowForAFullTurn) {
  IcMuFakeDriver driver;
  // 30 degrees between the limits: far less than a turn of the ring.
  Device device = makeCalibrationDevice(driver, 1, 19, [](IcMuFakeDriver& d) {
    const auto quarter = static_cast<uint32_t>(524288 / 24);
    const auto lower = static_cast<uint32_t>(-static_cast<int32_t>(quarter));
    d.store[OsCommandFakeDriver::key(0x607D, 1)] = {
        static_cast<uint8_t>(lower), static_cast<uint8_t>(lower >> 8),
        static_cast<uint8_t>(lower >> 16), static_cast<uint8_t>(lower >> 24)};
    d.store[OsCommandFakeDriver::key(0x607D, 2)] = {
        static_cast<uint8_t>(quarter), static_cast<uint8_t>(quarter >> 8),
        static_cast<uint8_t>(quarter >> 16), static_cast<uint8_t>(quarter >> 24)};
  });
  expectRefusal(device, driver, calibrationRequest(), "widen the range");
}

#endif  // MM_HAVE_MU_3SL

TEST(RunIcMuEncoderStatusProcedure, DecodesWhatTheDriveLatched) {
  IcMuFakeDriver driver;
  driver.programIcMuDrive("v5.6.11", 1, 20, 18);
  driver.programObject(0x2111, 4, ObjectDataType::UNSIGNED32, {0x00, 0x10, 0x00, 0x00});  // BAT_ERR
  Device device(1, driver);
  ASSERT_TRUE(device.initializeParameters().has_value());
  ProgressReporter reporter(mm::node::icMuEncoderStatusSteps());

  auto result = mm::node::runIcMuEncoderStatusProcedure(device, reporter, std::stop_token{});
  ASSERT_TRUE(result.has_value()) << result.error();

  const auto steps = reporter.steps();
  const auto& statuses = std::ranges::find(steps, mm::node::kIcMuEncoderStatusRegistersStep,
                                           &mm::node::ProgressStep::id)
                             ->value;
  ASSERT_EQ(statuses.size(), 1u);
  EXPECT_EQ(statuses[0]["encoderStatus"], 0x1000);
  ASSERT_EQ(statuses[0]["encoderStatusBits"].size(), 1u);
  EXPECT_EQ(statuses[0]["encoderStatusBits"][0]["name"], "BAT_ERR");
  EXPECT_EQ(statuses[0]["encoderStatusBits"][0]["chip"], "iC-PVL");
  // The status word is an SDO read: the chips are asked only for 0x74 and 0x11, never for STATUS1.
  EXPECT_TRUE(onlyRegisterReads(driver));
  for (const auto& command : driver.commands) {
    EXPECT_NE(command[3], 0x77);
  }
}

TEST(RunIcMuEncoderConfigurationProcedure, RefusesAnIcMuWithAnotherBankSelected) {
  IcMuFakeDriver driver;
  Device device = makeIcMuDevice(driver, "v5.6.11", 2, 19, 0);
  driver.icMu[0x40] = 0x01;  // BANKSEL: an EEPROM bank instead of the configuration
  ProgressReporter reporter(mm::node::icMuEncoderConfigurationSteps());

  auto result = mm::node::runIcMuEncoderConfigurationProcedure(device, reporter, std::stop_token{},
                                                               icMuRequest());
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("BANKSEL"), std::string::npos) << result.error();
  EXPECT_TRUE(onlyRegisterReads(driver));
}

TEST(RunIcMuEncoderConfigurationProcedure, FailsWhenWriteAllReportsAnI2cError) {
  IcMuFakeDriver driver;
  Device device = makeIcMuDevice(driver, "v5.6.11", 2, 19, 0);
  driver.icMu[0x77] = 0x40;  // STATUS1: EPR_ERR, the EEPROM did not answer
  ProgressReporter reporter(mm::node::icMuEncoderConfigurationSteps());

  auto result = mm::node::runIcMuEncoderConfigurationProcedure(device, reporter, std::stop_token{},
                                                               icMuRequest());
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("EPR_ERR"), std::string::npos) << result.error();
  const auto steps = reporter.steps();
  EXPECT_EQ(
      std::ranges::find(steps, mm::node::kIcMuEepromWriteStep, &mm::node::ProgressStep::id)->status,
      ProgressStatus::kFailed);
  // The status bits come back even though the save failed.
  EXPECT_EQ(driver.commandIds.back(), 14);
}

}  // namespace
