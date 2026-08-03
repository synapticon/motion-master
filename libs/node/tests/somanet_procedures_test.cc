#include "node/somanet_procedures.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
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
using mm::node::Device;
using mm::node::kOsCommand;
using mm::node::kOsCommandMode;
using mm::node::kOsCommandStep;
using mm::node::kSynapticonVendorId;
using mm::node::motorPhaseOrderDetectionSteps;
using mm::node::openPhaseDetectionSteps;
using mm::node::OsCommandRequest;
using mm::node::osCommandSteps;
using mm::node::phaseInductanceMeasurementSteps;
using mm::node::phaseResistanceMeasurementSteps;
using mm::node::polePairDetectionSteps;
using mm::node::ProgressReporter;
using mm::node::ProgressStatus;
using mm::node::runMotorPhaseOrderDetectionProcedure;
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
  uint32_t vendorId = kSynapticonVendorId;
  uint16_t statusword = 0x0040;  // SwitchOnDisabled
  std::vector<std::pair<uint16_t, uint8_t>> writeLog;

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
    store[key(Object::kStatusword, 0)] = {0x40, 0x00};
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
    writeLog.push_back({index, subindex});
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
const mm::node::ProgressStep* stepById(const std::vector<mm::node::ProgressStep>& steps,
                                       std::string_view id) {
  for (const auto& step : steps) {
    if (step.id == id) {
      return &step;
    }
  }
  return nullptr;
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
    const auto* step = stepById(steps, id);
    ASSERT_NE(step, nullptr) << id;
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
  const auto* detect = stepById(steps, "open-phase-detection");
  ASSERT_NE(detect, nullptr);
  EXPECT_EQ(detect->status, ProgressStatus::kFailed);
  ASSERT_TRUE(detect->error.has_value());
  EXPECT_NE(detect->error->find("terminal A of the drive is not connected"), std::string::npos);

  // The restore is not conditional on success — a failed run must not leave the drive in
  // diagnostics mode.
  const auto* restore = stepById(steps, "restore");
  ASSERT_NE(restore, nullptr);
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
  const auto* restore = stepById(steps, "restore");
  ASSERT_NE(restore, nullptr);
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
    const auto* step = stepById(steps, id);
    ASSERT_NE(step, nullptr) << id;
    EXPECT_EQ(step->status, ProgressStatus::kSucceeded) << id;
  }

  const auto* detected = stepById(steps, "pole-pair-detection");
  ASSERT_NE(detected, nullptr);
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

TEST(RunPolePairDetectionProcedure, AFailedDetectionStillRestoresTheBrake) {
  OsCommandFakeDriver driver;
  Device device = makeDevice(driver);
  ProgressReporter reporter(polePairDetectionSteps());

  driver.responses = {{3, 0, 0, 0, 0, 0, 0, 0}};  // command-specific code 0: current amplitude
  auto result = runPolePairDetectionProcedure(device, reporter, std::stop_token{});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("could not raise the motor phase currents"), std::string::npos)
      << result.error();

  const auto* restore = stepById(reporter.steps(), "restore");
  ASSERT_NE(restore, nullptr);
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
    const auto* step = stepById(steps, id);
    ASSERT_NE(step, nullptr) << id;
    EXPECT_EQ(step->status, ProgressStatus::kSucceeded) << id;
  }

  const auto* detected = stepById(steps, "motor-phase-order-detection");
  ASSERT_NE(detected, nullptr);
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

  const auto* restore = stepById(reporter.steps(), "restore");
  ASSERT_NE(restore, nullptr);
  EXPECT_EQ(restore->status, ProgressStatus::kSucceeded);
  EXPECT_EQ(
      driver.store.at(OsCommandFakeDriver::key(somanet::kBrakeOptions, somanet::kBrakeStatus)),
      std::vector<uint8_t>{static_cast<uint8_t>(somanet::BrakeStatus::kEngaged)});
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
    const auto* step = stepById(steps, id);
    ASSERT_NE(step, nullptr) << id;
    EXPECT_EQ(step->status, ProgressStatus::kSucceeded) << id;
  }

  const auto* measured = stepById(steps, "phase-resistance-measurement");
  ASSERT_NE(measured, nullptr);
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
  const auto* measured = stepById(steps, "phase-resistance-measurement");
  ASSERT_NE(measured, nullptr);
  EXPECT_EQ(measured->status, ProgressStatus::kFailed);

  const auto* restore = stepById(steps, "restore");
  ASSERT_NE(restore, nullptr);
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

  const auto* restore = stepById(reporter.steps(), "restore");
  ASSERT_NE(restore, nullptr);
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
    const auto* step = stepById(steps, id);
    ASSERT_NE(step, nullptr) << id;
    EXPECT_EQ(step->status, ProgressStatus::kSucceeded) << id;
  }

  const auto* measured = stepById(steps, "phase-inductance-measurement");
  ASSERT_NE(measured, nullptr);
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
