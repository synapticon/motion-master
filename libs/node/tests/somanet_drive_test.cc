#include "node/somanet_drive.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <map>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "comm/fieldbus_driver.h"
#include "comm/object_data_types.h"
#include "node/cia402.h"
#include "node/device.h"
#include "node/synapticon.h"

namespace {

using mm::comm::EtherCatState;
using mm::comm::FieldbusDriver;
using mm::comm::ObjectDataType;
using mm::comm::OdEntry;
using mm::comm::SlaveInfo;
using mm::node::createSomanetDrive;
namespace somanet = mm::node::somanet;
using mm::node::Device;
using mm::node::kOsCommand;
using mm::node::kOsCommandMode;
using mm::node::kSynapticonVendorId;
using mm::node::cia402::Object;

constexpr uint16_t kPreOp = static_cast<uint16_t>(EtherCatState::PreOp);

/// Minimal driver double: reports a configurable vendor ID and a configurable set of OD entries.
/// Enough to exercise createSomanetDrive's vendor + CiA402-object validation (no bus I/O needed).
class IdentityFakeDriver : public FieldbusDriver {
 public:
  uint32_t vendorId = kSynapticonVendorId;
  std::vector<OdEntry> ods;

  void programObject(uint16_t index, uint8_t subindex, ObjectDataType type) {
    OdEntry e{};
    e.index = index;
    e.subindex = subindex;
    e.dataType = static_cast<uint16_t>(type);
    ods.push_back(e);
  }

  void programCia402Objects() {
    programObject(Object::kControlword, 0, ObjectDataType::UNSIGNED16);
    programObject(Object::kStatusword, 0, ObjectDataType::UNSIGNED16);
  }

  SlaveInfo slaveInfo(uint16_t) const override {
    SlaveInfo info{};
    info.vendorId = vendorId;
    return info;
  }
  std::expected<std::vector<OdEntry>, std::string> readObjectDictionary(uint16_t) override {
    return ods;
  }
  uint16_t slaveState(uint16_t) const override { return kPreOp; }
  // CoE-capable stand-in: parameters are enumerated over the object dictionary, not SII.
  uint16_t mailboxProtocols(uint16_t) const override {
    return mm::comm::MailboxConfig::kProtocolCoe;
  }

  // --- unused stubs ---------------------------------------------------------
  std::expected<void, std::string> init() override { return {}; }
  std::expected<int, std::string> scan() override { return 0; }
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

TEST(CreateSomanetDrive, AcceptsSynapticonCia402Drive) {
  IdentityFakeDriver driver;
  driver.vendorId = kSynapticonVendorId;
  driver.programCia402Objects();
  Device device(1, driver);
  ASSERT_TRUE(device.initializeParameters().has_value());

  auto drive = createSomanetDrive(device);
  EXPECT_TRUE(drive.has_value());
}

TEST(CreateSomanetDrive, RejectsForeignVendor) {
  IdentityFakeDriver driver;
  driver.vendorId = 0x00000539;  // some other vendor
  driver.programCia402Objects();
  Device device(1, driver);
  ASSERT_TRUE(device.initializeParameters().has_value());

  auto drive = createSomanetDrive(device);
  EXPECT_FALSE(drive.has_value());
}

TEST(CreateSomanetDrive, RejectsSynapticonNonCia402Device) {
  IdentityFakeDriver driver;
  driver.vendorId = kSynapticonVendorId;
  // No CiA402 objects enumerated — a Synapticon I/O module, say.
  Device device(1, driver);
  ASSERT_TRUE(device.initializeParameters().has_value());

  auto drive = createSomanetDrive(device);
  EXPECT_FALSE(drive.has_value());
}

// --- OS command (0x1023 / 0x1024) ----------------------------------------------------------------

/// Driver double modelling the drive's OS command handshake: it records what the master writes to
/// the command (0x1023:01) and mode (0x1024) objects, and answers reads of the response
/// (0x1023:03) from a scripted sequence, repeating the last entry once exhausted. Reads of the
/// status mirror (0x1023:02) are counted so a test can assert the implementation never polls it —
/// reading it would not re-arm the command object on a real drive.
class OsCommandFakeDriver : public FieldbusDriver {
 public:
  std::vector<OdEntry> ods;
  std::map<uint32_t, std::vector<uint8_t>> store;

  std::vector<std::vector<uint8_t>> responses{{0, 0, 0, 0, 0, 0, 0, 0}};
  int responseReads = 0;
  int statusReads = 0;

  std::vector<uint8_t> lastCommand;
  int commandWrites = 0;
  bool failCommandWrite = false;
  std::vector<uint8_t> modeWrites;

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

  void programOsCommandObjects() {
    programObject(Object::kControlword, 0, ObjectDataType::UNSIGNED16, {0, 0});
    programObject(Object::kStatusword, 0, ObjectDataType::UNSIGNED16, {0, 0});
    programObject(kOsCommand, 1, ObjectDataType::OCTET_STRING, std::vector<uint8_t>(8, 0));
    programObject(kOsCommand, 2, ObjectDataType::UNSIGNED8, {0});
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
    if (index == kOsCommand && subindex == 2) {
      ++statusReads;
      return std::vector<uint8_t>{0};
    }
    auto it = store.find(key(index, subindex));
    if (it == store.end()) {
      return std::unexpected("no such object");
    }
    return it->second;
  }

  std::expected<void, std::string> writeSdo(uint16_t, uint16_t index, uint8_t subindex,
                                            std::span<const uint8_t> data) override {
    if (index == kOsCommand && subindex == 1) {
      if (failCommandWrite) {
        return std::unexpected(
            "SDO abort 0x08000021: Data cannot be transferred because of local control");
      }
      ++commandWrites;
      lastCommand.assign(data.begin(), data.end());
    }
    if (index == kOsCommandMode && subindex == 0 && !data.empty()) {
      modeWrites.push_back(data[0]);
    }
    store[key(index, subindex)] = std::vector<uint8_t>(data.begin(), data.end());
    return {};
  }

  std::expected<std::vector<OdEntry>, std::string> readObjectDictionary(uint16_t) override {
    return ods;
  }
  SlaveInfo slaveInfo(uint16_t) const override {
    SlaveInfo info{};
    info.vendorId = kSynapticonVendorId;
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

// Zero poll delay so the tests exercise the polling logic without real waiting.
constexpr auto kNoDelay = std::chrono::milliseconds(0);

// An arbitrary well-formed request: command ID 8 (phase resistance measurement) and no parameters.
const std::vector<uint8_t> kRequest{8, 0, 0, 0, 0, 0, 0, 0};

// Programs the 0x2004 sub-entries the brake operations touch. Voltages and pull time are arbitrary
// but distinct, so a test can tell which object a value came from.
void programBrakeObjects(OsCommandFakeDriver& driver, somanet::BrakeReleaseStrategy strategy,
                         somanet::BrakeStatus status, uint16_t pullTimeMs = 120) {
  driver.programObject(somanet::kBrakeOptions, somanet::kBrakePullVoltage,
                       ObjectDataType::UNSIGNED32, {0xE8, 0x03, 0, 0});  // 1000 mV
  driver.programObject(somanet::kBrakeOptions, somanet::kBrakeHoldVoltage,
                       ObjectDataType::UNSIGNED32, {0xF4, 0x01, 0, 0});  // 500 mV
  driver.programObject(
      somanet::kBrakeOptions, somanet::kBrakePullTime, ObjectDataType::UNSIGNED16,
      {static_cast<uint8_t>(pullTimeMs & 0xFF), static_cast<uint8_t>((pullTimeMs >> 8) & 0xFF)});
  driver.programObject(somanet::kBrakeOptions, somanet::kBrakeReleaseStrategy,
                       ObjectDataType::UNSIGNED8, {static_cast<uint8_t>(strategy)});
  driver.programObject(somanet::kBrakeOptions, somanet::kBrakeStatus, ObjectDataType::UNSIGNED8,
                       {static_cast<uint8_t>(status)});
}

// A device whose brake is programmed as given. Brake tests use a zero settle so they never really
// wait; the pull time is still read and added, so it is kept small.
Device makeBrakeDevice(OsCommandFakeDriver& driver, somanet::BrakeReleaseStrategy strategy,
                       somanet::BrakeStatus status) {
  driver.programObject(Object::kControlword, 0, ObjectDataType::UNSIGNED16, {0, 0});
  driver.programObject(Object::kStatusword, 0, ObjectDataType::UNSIGNED16, {0, 0});
  driver.programObject(Object::kModeOfOperation, 0, ObjectDataType::INTEGER8, {0});
  programBrakeObjects(driver, strategy, status, /*pullTimeMs=*/1);
  Device device(1, driver);
  EXPECT_TRUE(device.initializeParameters().has_value());
  return device;
}

uint8_t storedBrakeStatus(const OsCommandFakeDriver& driver) {
  return driver.store.at(
      OsCommandFakeDriver::key(somanet::kBrakeOptions, somanet::kBrakeStatus))[0];
}

Device makeOsCommandDevice(OsCommandFakeDriver& driver) {
  driver.programOsCommandObjects();
  Device device(1, driver);
  EXPECT_TRUE(device.initializeParameters().has_value());
  return device;
}

TEST(RunOsCommand, WritesModeThenCommandAndCompletes) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{0, 0, 0, 0, 0, 0, 0, 0}};  // completed, no error, no response data
  auto response = drive->runOsCommand(kRequest, {.pollInterval = kNoDelay});
  ASSERT_TRUE(response.has_value()) << response.error();
  EXPECT_EQ(response->status, mm::node::OsCommandStatus::kCompleted);
  EXPECT_FALSE(response->failed());
  EXPECT_TRUE(response->data.empty());
  EXPECT_FALSE(response->errorCode.has_value());

  // The mode is normalised to 0 before the command is written; nothing aborts, so it is the only
  // mode write.
  EXPECT_EQ(driver.modeWrites, std::vector<uint8_t>{0});
  EXPECT_EQ(driver.commandWrites, 1);
  EXPECT_EQ(driver.lastCommand, kRequest);
}

TEST(RunOsCommand, NeverReadsTheStatusMirror) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{255, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0}};
  ASSERT_TRUE(drive->runOsCommand(kRequest, {.pollInterval = kNoDelay}).has_value());

  // Only reading 0x1023:03 re-arms the command object, so 0x1023:02 must never be polled.
  EXPECT_EQ(driver.statusReads, 0);
  EXPECT_EQ(driver.responseReads, 2);
}

TEST(RunOsCommand, ReturnsResponseData) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  // Completed with response data: six payload bytes starting at byte 2 (byte 1 is unused).
  driver.responses = {{1, 0xFF, 1, 2, 3, 4, 5, 6}};
  auto response = drive->runOsCommand(kRequest, {.pollInterval = kNoDelay});
  ASSERT_TRUE(response.has_value()) << response.error();
  EXPECT_EQ(response->status, mm::node::OsCommandStatus::kCompletedWithData);
  EXPECT_FALSE(response->failed());
  EXPECT_FALSE(response->errorCode.has_value());
  EXPECT_EQ(response->data, (std::vector<uint8_t>{1, 2, 3, 4, 5, 6}));
}

TEST(RunOsCommand, ReturnsErrorCodeAndDataAsAValue) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  // Failed with response data: byte 2 is the OS error code, so only five payload bytes remain.
  driver.responses = {{3, 0, 254, 1, 2, 3, 4, 5}};
  auto response = drive->runOsCommand(kRequest, {.pollInterval = kNoDelay});
  ASSERT_TRUE(response.has_value()) << response.error();  // a verdict, not a transport failure
  EXPECT_EQ(response->status, mm::node::OsCommandStatus::kFailedWithData);
  EXPECT_TRUE(response->failed());
  ASSERT_TRUE(response->errorCode.has_value());
  EXPECT_EQ(*response->errorCode, 254);
  EXPECT_EQ(response->data, (std::vector<uint8_t>{1, 2, 3, 4, 5}));
}

TEST(RunOsCommand, PollsWhileTheDriveReportsProgress) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{255, 0, 0, 0, 0, 0, 0, 0},  // executing, no percentage
                      {100, 0, 0, 0, 0, 0, 0, 0},  // 0%
                      {150, 0, 0, 0, 0, 0, 0, 0},  // 50%
                      {200, 0, 0, 0, 0, 0, 0, 0},  // 100%
                      {2, 0, 0, 0, 0, 0, 0, 0}};   // completed with error, no response data
  auto response = drive->runOsCommand(kRequest, {.pollInterval = kNoDelay});
  ASSERT_TRUE(response.has_value()) << response.error();
  EXPECT_EQ(response->status, mm::node::OsCommandStatus::kFailed);
  EXPECT_TRUE(response->failed());
  EXPECT_FALSE(response->errorCode.has_value());
  EXPECT_EQ(driver.responseReads, 5);
}

TEST(RunOsCommand, AbortsAndReportsTimeout) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  // Still executing when the deadline passes, then the drive confirms the abort (error 252).
  driver.responses = {
      {255, 0, 0, 0, 0, 0, 0, 0}, {255, 0, 0, 0, 0, 0, 0, 0}, {3, 0, 252, 0, 0, 0, 0, 0}};
  auto response = drive->runOsCommand(kRequest, {.timeout = kNoDelay, .pollInterval = kNoDelay});
  ASSERT_FALSE(response.has_value());
  EXPECT_NE(response.error().find("timed out"), std::string::npos) << response.error();

  // Mode 0 to arm the command, 3 to abort it, 0 again so the next command is not discarded.
  EXPECT_EQ(driver.modeWrites, (std::vector<uint8_t>{0, 3, 0}));
}

TEST(RunOsCommand, AbortsOnCancellation) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  std::stop_source source;
  source.request_stop();
  driver.responses = {{255, 0, 0, 0, 0, 0, 0, 0}, {3, 0, 252, 0, 0, 0, 0, 0}};
  auto response = drive->runOsCommand(
      kRequest,
      {.timeout = std::chrono::minutes(1), .pollInterval = kNoDelay, .stop = source.get_token()});
  ASSERT_FALSE(response.has_value());
  EXPECT_NE(response.error().find("cancelled"), std::string::npos) << response.error();
  EXPECT_EQ(driver.modeWrites, (std::vector<uint8_t>{0, 3, 0}));
}

TEST(RunOsCommand, RestoresTheModeWhenTheAbortIsNeverConfirmed) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{255, 0, 0, 0, 0, 0, 0, 0}};  // never leaves the executing state
  auto response = drive->runOsCommand(
      kRequest, {.timeout = kNoDelay, .pollInterval = kNoDelay, .abortTimeout = kNoDelay});
  ASSERT_FALSE(response.has_value());
  EXPECT_NE(response.error().find("did not report the abort"), std::string::npos)
      << response.error();
  EXPECT_EQ(driver.modeWrites, (std::vector<uint8_t>{0, 3, 0}));
}

TEST(RunOsCommand, ForwardsTheCommandWriteErrorVerbatim) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  // The drive refuses the write while a command is still running; that abort code is the whole
  // diagnosis, so it must reach the caller unchanged.
  driver.failCommandWrite = true;
  auto response = drive->runOsCommand(kRequest, {.pollInterval = kNoDelay});
  ASSERT_FALSE(response.has_value());
  EXPECT_NE(response.error().find("0x08000021"), std::string::npos) << response.error();
  EXPECT_EQ(driver.responseReads, 0);
}

TEST(RunOsCommand, RejectsAWrongSizedCommand) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  auto response = drive->runOsCommand({8, 0, 0}, {.pollInterval = kNoDelay});
  ASSERT_FALSE(response.has_value());
  EXPECT_EQ(driver.commandWrites, 0);
  EXPECT_TRUE(driver.modeWrites.empty());
}

TEST(RunOsCommand, FailsOnAnUnknownStatus) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{42, 0, 0, 0, 0, 0, 0, 0}};  // reserved by the specification
  auto response = drive->runOsCommand(kRequest, {.pollInterval = kNoDelay});
  ASSERT_FALSE(response.has_value());
  EXPECT_NE(response.error().find("unknown OS command status 42"), std::string::npos)
      << response.error();
}

TEST(OsCommandErrorName, NamesGeneralCodesOnly) {
  EXPECT_EQ(mm::node::osCommandErrorName(251), "command not allowed");
  EXPECT_EQ(mm::node::osCommandErrorName(252), "command aborted");
  EXPECT_EQ(mm::node::osCommandErrorName(253), "command timeout");
  EXPECT_EQ(mm::node::osCommandErrorName(254), "unsupported command");
  // Codes counting up from 0 are command-specific — only the issuing command can name them.
  EXPECT_FALSE(mm::node::osCommandErrorName(0).has_value());
  EXPECT_FALSE(mm::node::osCommandErrorName(8).has_value());
}

// --- Brake (0x2004) ---------------------------------------------------------------------------

constexpr auto kNoSettle = std::chrono::milliseconds(0);

TEST(BrakeState, ReportsTheConfigurationAndDerivedFlags) {
  OsCommandFakeDriver driver;
  Device device =
      makeBrakeDevice(driver, somanet::BrakeReleaseStrategy::kPin, somanet::BrakeStatus::kEngaged);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  auto state = drive->brakeState();
  ASSERT_TRUE(state.has_value()) << state.error();
  EXPECT_EQ(state->status, somanet::BrakeStatus::kEngaged);
  EXPECT_EQ(state->releaseStrategy, somanet::BrakeReleaseStrategy::kPin);
  EXPECT_EQ(state->pullVoltageMv, 1000u);
  EXPECT_EQ(state->holdVoltageMv, 500u);
  EXPECT_TRUE(state->softwareControllable());
  // A pin brake is the one whose release turns the motor, which a client has to be told.
  EXPECT_TRUE(state->releaseMovesShaft());
}

TEST(ReleaseBrake, DisengagesAClutchBrakeAndReadsItBack) {
  OsCommandFakeDriver driver;
  Device device = makeBrakeDevice(driver, somanet::BrakeReleaseStrategy::kClutch,
                                  somanet::BrakeStatus::kEngaged);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  auto state = drive->releaseBrake(kNoSettle);
  ASSERT_TRUE(state.has_value()) << state.error();
  EXPECT_EQ(storedBrakeStatus(driver), static_cast<uint8_t>(somanet::BrakeStatus::kDisengaged));
  EXPECT_EQ(state->status, somanet::BrakeStatus::kDisengaged);
  EXPECT_FALSE(state->releaseMovesShaft());
}

TEST(ReleaseBrake, LeavesAManualBrakeAloneAndSaysWhy) {
  // Release strategy 0 means the brake is not the firmware's to drive, so commanding 0x2004:07
  // would do nothing. That is not a failure — and the returned state is how a caller tells, without
  // having to know that 0 means manual.
  OsCommandFakeDriver driver;
  Device device = makeBrakeDevice(driver, somanet::BrakeReleaseStrategy::kManualOutputVoltage,
                                  somanet::BrakeStatus::kEngaged);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  auto state = drive->releaseBrake(kNoSettle);
  ASSERT_TRUE(state.has_value()) << state.error();
  EXPECT_EQ(storedBrakeStatus(driver), static_cast<uint8_t>(somanet::BrakeStatus::kEngaged));
  EXPECT_FALSE(state->softwareControllable());
}

TEST(ReleaseBrake, DoesNotRewriteAnAlreadyDisengagedBrake) {
  // Skipping the write also skips a second pull-time wait, which is the point: releasing an already
  // released brake should not cost the caller another pull time.
  OsCommandFakeDriver driver;
  Device device = makeBrakeDevice(driver, somanet::BrakeReleaseStrategy::kClutch,
                                  somanet::BrakeStatus::kDisengaged);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  auto state = drive->releaseBrake(kNoSettle);
  ASSERT_TRUE(state.has_value()) << state.error();
  EXPECT_EQ(state->status, somanet::BrakeStatus::kDisengaged);
}

TEST(EngageBrake, EngagesAClutchBrake) {
  OsCommandFakeDriver driver;
  Device device = makeBrakeDevice(driver, somanet::BrakeReleaseStrategy::kClutch,
                                  somanet::BrakeStatus::kDisengaged);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  auto state = drive->engageBrake(kNoSettle);
  ASSERT_TRUE(state.has_value()) << state.error();
  EXPECT_EQ(storedBrakeStatus(driver), static_cast<uint8_t>(somanet::BrakeStatus::kEngaged));
  EXPECT_EQ(state->status, somanet::BrakeStatus::kEngaged);
}

TEST(EngageBrake, LeavesAManualBrakeAlone) {
  OsCommandFakeDriver driver;
  Device device = makeBrakeDevice(driver, somanet::BrakeReleaseStrategy::kManualOutputVoltage,
                                  somanet::BrakeStatus::kDisengaged);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  auto state = drive->engageBrake(kNoSettle);
  ASSERT_TRUE(state.has_value()) << state.error();
  EXPECT_EQ(storedBrakeStatus(driver), static_cast<uint8_t>(somanet::BrakeStatus::kDisengaged));
}

TEST(SetBrakeStatus, WritesTheRawValueForRestoring) {
  // The raw write exists so a guard can put back exactly what it read, with no timing and no
  // checks.
  OsCommandFakeDriver driver;
  Device device = makeBrakeDevice(driver, somanet::BrakeReleaseStrategy::kClutch,
                                  somanet::BrakeStatus::kDisengaged);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  ASSERT_TRUE(drive->setBrakeStatus(somanet::BrakeStatus::kEngaged).has_value());
  EXPECT_EQ(storedBrakeStatus(driver), static_cast<uint8_t>(somanet::BrakeStatus::kEngaged));
}

TEST(OperationModeValue, RoundTripsAVendorModeTheCia402EnumCannotName) {
  // The reason the raw pair exists: diagnostics is -2, which is not a CiA402 mode, and save/restore
  // must preserve whatever mode it finds without deciding which enum names it.
  OsCommandFakeDriver driver;
  Device device = makeBrakeDevice(driver, somanet::BrakeReleaseStrategy::kClutch,
                                  somanet::BrakeStatus::kEngaged);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  ASSERT_TRUE(drive->setOperationMode(somanet::OperationMode::kDiagnostics).has_value());
  auto mode = drive->operationModeValue();
  ASSERT_TRUE(mode.has_value()) << mode.error();
  EXPECT_EQ(*mode, -2);

  // And the inherited standard-mode setter is still reachable — declaring the vendor overload would
  // otherwise have hidden it.
  ASSERT_TRUE(
      drive->setOperationMode(mm::node::cia402::OperationMode::kCyclicSyncPosition).has_value());
  EXPECT_EQ(drive->operationModeValue().value(), 8);
}

// --- Open phase detection (OS command 6) --------------------------------------------------------

TEST(RunOpenPhaseDetection, ASuccessfulCommandMeansNoPhaseIsOpen) {
  // The command's verdict is inverted: the drive answers success when it found nothing wrong.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{0, 0, 0, 0, 0, 0, 0, 0}};  // completed, no error
  auto result = drive->runOpenPhaseDetection({.pollInterval = kNoDelay});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_FALSE(result->phaseOpened);
  EXPECT_FALSE(result->fault.has_value());
  EXPECT_EQ(result->describe(), "no open phase detected");
}

TEST(RunOpenPhaseDetection, IssuesCommandSix) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  ASSERT_TRUE(drive->runOpenPhaseDetection({.pollInterval = kNoDelay}).has_value());
  ASSERT_EQ(driver.lastCommand.size(), 8u);
  EXPECT_EQ(driver.lastCommand[0], 6);
  // No parameters: bytes 1-7 are zero.
  EXPECT_EQ(std::vector<uint8_t>(driver.lastCommand.begin() + 1, driver.lastCommand.end()),
            std::vector<uint8_t>(7, 0));
}

TEST(RunOpenPhaseDetection, AFailedCommandIsTheFindingAndNamesTheFault) {
  // A *failed* OS command with a command-specific code is a completed measurement reporting which
  // terminal or FET is open — a value, emphatically not a transport error.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  // Status 3 (failed with data), OS error code 1 in byte 2 → open terminal B.
  driver.responses = {{3, 0, 1, 0, 0, 0, 0, 0}};
  auto result = drive->runOpenPhaseDetection({.pollInterval = kNoDelay});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_TRUE(result->phaseOpened);
  ASSERT_TRUE(result->fault.has_value());
  EXPECT_EQ(*result->fault, somanet::OpenPhaseFault::kOpenTerminalB);
  EXPECT_EQ(result->faultCode, 1);
  EXPECT_EQ(result->describe(), "open terminal B — terminal B of the drive is not connected");
}

TEST(RunOpenPhaseDetection, ReportsAFaultCodeItCannotName) {
  // A firmware that grows the fault table must stay actionable: the finding stands and the raw code
  // is reported rather than swallowed.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{3, 0, 42, 0, 0, 0, 0, 0}};
  auto result = drive->runOpenPhaseDetection({.pollInterval = kNoDelay});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_TRUE(result->phaseOpened);
  EXPECT_FALSE(result->fault.has_value());
  EXPECT_EQ(result->faultCode, 42);
  EXPECT_EQ(result->describe(), "an open phase was detected (fault code 42)");
}

TEST(RunOpenPhaseDetection, AGeneralOsErrorIsAnErrorNotAFinding) {
  // 251 "command not allowed" means the check never ran — the drive was in the wrong mode or state.
  // Reporting that as "a phase is open" would invent a hardware fault, so it must be an error.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{3, 0, 251, 0, 0, 0, 0, 0}};
  auto result = drive->runOpenPhaseDetection({.pollInterval = kNoDelay});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("not performed"), std::string::npos) << result.error();
  EXPECT_NE(result.error().find("command not allowed"), std::string::npos) << result.error();
}

TEST(RunOpenPhaseDetection, AFailureWithoutACodeStillReportsAnOpenPhase) {
  // Status 2 is "failed, no response data": the drive gave a verdict but no code.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{2, 0, 0, 0, 0, 0, 0, 0}};
  auto result = drive->runOpenPhaseDetection({.pollInterval = kNoDelay});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_TRUE(result->phaseOpened);
  EXPECT_FALSE(result->faultCode.has_value());
}

// --- Pole pair detection (command 7) -------------------------------------------------------------

TEST(RunPolePairDetection, DecodesTheCountFromTheFirstPayloadByte) {
  // The specification's example: 3 pole pairs, reported in response byte 2.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{1, 0, 3, 0, 0, 0, 0, 0}};
  auto result = drive->runPolePairDetection({.pollInterval = kNoDelay});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(result->polePairs, 3);
  EXPECT_EQ(result->describe(), "3 pole pairs");
  EXPECT_EQ(driver.store.at(OsCommandFakeDriver::key(kOsCommand, 1))[0], 7);
}

TEST(RunPolePairDetection, SaysPolePairSingularForOne) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{1, 0, 1, 0, 0, 0, 0, 0}};
  auto result = drive->runPolePairDetection({.pollInterval = kNoDelay});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(result->describe(), "1 pole pair");
}

TEST(RunPolePairDetection, NamesTheCurrentAmplitudeFaultAndWhatCausesIt) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{3, 0, 0, 0, 0, 0, 0, 0}};
  auto result = drive->runPolePairDetection({.pollInterval = kNoDelay});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("pole pair detection"), std::string::npos) << result.error();
  EXPECT_NE(result.error().find("DC-link voltage"), std::string::npos) << result.error();
}

// --- Phase resistance measurement (command 8) ----------------------------------------------------

TEST(RunPhaseResistanceMeasurement, DecodesTheBigEndianMilliohmValue) {
  // The firmware specification's own example: 100000 mΩ arrives as 0x000186A0 in response bytes
  // 2-5, most significant byte first — the opposite order to every SDO value in the dictionary, so
  // decoding it little-endian would read 2691137536 and look plausible.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{1, 0, 0x00, 0x01, 0x86, 0xA0, 0, 0}};
  auto result = drive->runPhaseResistanceMeasurement({.pollInterval = kNoDelay});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(result->milliohms, 100000u);
  EXPECT_EQ(result->describe(), "phase resistance 100000 mΩ");
}

TEST(RunPhaseResistanceMeasurement, IssuesCommandEight) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{1, 0, 0, 0, 0, 1, 0, 0}};
  ASSERT_TRUE(drive->runPhaseResistanceMeasurement({.pollInterval = kNoDelay}).has_value());

  const auto written = driver.store.at(OsCommandFakeDriver::key(kOsCommand, 1));
  ASSERT_EQ(written.size(), 8u);
  EXPECT_EQ(written[0], 8);
}

TEST(RunPhaseResistanceMeasurement, NamesTheCommandSpecificCurrentAmplitudeFault) {
  // Code 0 is command-specific here, and means something entirely different from open phase
  // detection's code 0 ("open terminal A") — which is why each command decodes its own table.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{3, 0, 0, 0, 0, 0, 0, 0}};
  auto result = drive->runPhaseResistanceMeasurement({.pollInterval = kNoDelay});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("could not raise the motor phase currents"), std::string::npos)
      << result.error();
}

TEST(RunPhaseResistanceMeasurement, AGeneralOsErrorNamesItself) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{3, 0, 251, 0, 0, 0, 0, 0}};
  auto result = drive->runPhaseResistanceMeasurement({.pollInterval = kNoDelay});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("was not performed"), std::string::npos) << result.error();
  EXPECT_NE(result.error().find("command not allowed"), std::string::npos) << result.error();
}

TEST(RunPhaseResistanceMeasurement, ATooShortPayloadIsAnErrorRatherThanAZero) {
  // Status 0 is "completed, no response data". A measurement command that answers that way has
  // produced no value, and reporting 0 mΩ would be a plausible-looking lie about a real motor.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{0, 0, 0, 0, 0, 0, 0, 0}};
  auto result = drive->runPhaseResistanceMeasurement({.pollInterval = kNoDelay});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("payload bytes"), std::string::npos) << result.error();
}

// --- Phase inductance measurement (command 9) ----------------------------------------------------

TEST(RunPhaseInductanceMeasurement, DecodesTheBigEndianMicrohenryValue) {
  // The specification's example: 4244 µH as 0x00001094.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{1, 0, 0x00, 0x00, 0x10, 0x94, 0, 0}};
  auto result = drive->runPhaseInductanceMeasurement({.pollInterval = kNoDelay});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(result->microhenries, 4244u);
  EXPECT_EQ(result->describe(), "phase inductance 4244 µH");
}

TEST(RunPhaseInductanceMeasurement, IssuesCommandNine) {
  // The two winding measurements share a decoder, so the command byte is what distinguishes them.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{1, 0, 0, 0, 0, 1, 0, 0}};
  ASSERT_TRUE(drive->runPhaseInductanceMeasurement({.pollInterval = kNoDelay}).has_value());
  EXPECT_EQ(driver.store.at(OsCommandFakeDriver::key(kOsCommand, 1))[0], 9);
}

TEST(RunPhaseInductanceMeasurement, SharesTheCurrentAmplitudeFaultTable) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{3, 0, 0, 0, 0, 0, 0, 0}};
  auto result = drive->runPhaseInductanceMeasurement({.pollInterval = kNoDelay});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("phase inductance measurement"), std::string::npos)
      << result.error();
  EXPECT_NE(result.error().find("could not raise the motor phase currents"), std::string::npos)
      << result.error();
}

}  // namespace
