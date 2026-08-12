#include "node/somanet_drive.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
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
  int drainReads = 0;
  bool awaitingCommand = true;
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
      awaitingCommand = false;
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

  // Files the device "holds", keyed by FoE name — including the fs-getlist pseudo-file, which the
  // firmware serves like any other file and which a test therefore programs like any other.
  std::map<std::string, std::vector<uint8_t>> files;

  std::expected<std::vector<uint8_t>, mm::comm::FoeError> readFile(
      uint16_t, const std::string& name) override {
    auto it = files.find(name);
    if (it == files.end()) {
      return std::unexpected(
          mm::comm::makeFoeError(mm::comm::FoeErrorKind::FileNotFound, "FOEread", 1, name));
    }
    return it->second;
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

// Zero poll delay so the tests exercise the polling logic without real waiting.
constexpr auto kNoDelay = std::chrono::milliseconds(0);

// An arbitrary well-formed request: command ID 8 (phase resistance measurement) and no parameters.
// A test fixture's eight-byte vector; the only throw is bad_alloc before main, which would fail
// the run either way.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
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

TEST(RunOsCommand, ReadsTheResponseBeforeIssuingSoAStaleOneCannotBeReported) {
  // The drive returns to idle only once 0x1023:03 has been read, and one still holding an unread
  // response ignores the write to 0x1023:01 as silently as one whose mode is not 0. Without this
  // read the first poll would return the *previous* command's terminal status and it would be
  // reported as this command's verdict — a failure this command never produced.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{1, 0, 7, 0, 0, 0, 0, 0}};
  ASSERT_TRUE(drive->runOsCommand(kRequest, {.pollInterval = kNoDelay}).has_value());

  // drainReads only counts reads taken while no command had been written yet, so a non-zero count
  // *is* the ordering assertion: the response was read before the command was issued.
  EXPECT_EQ(driver.drainReads, 1);
  EXPECT_EQ(driver.commandWrites, 1);
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

// --- Encoder register communication (OS command 0) ----------------------------------------------

TEST(ReadEncoderRegister, IssuesCommandZeroWithTheAddressAndTheReadDirection) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{1, 0, 0x08, 0, 0, 0, 0, 0}};
  ASSERT_TRUE(drive
                  ->readEncoderRegister(somanet::EncoderOrdinal::kEncoder1, 0x11,
                                        {.pollInterval = kNoDelay})
                  .has_value());
  // Byte 1 the ordinal, byte 2 the slave address (always 0) above the direction bit, byte 3 the
  // register. Byte 4 stays 0 on a read whatever a caller passed.
  EXPECT_EQ(driver.lastCommand, (std::vector<uint8_t>{0, 1, 0, 0x11, 0, 0, 0, 0}));
}

TEST(ReadEncoderRegister, DecodesTheRegisterValue) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{1, 0, 0x08, 0, 0, 0, 0, 0}};  // completed with data
  auto result = drive->readEncoderRegister(somanet::EncoderOrdinal::kEncoder1, 0x11,
                                           {.pollInterval = kNoDelay});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(result->value, 0x08);
  EXPECT_FALSE(result->wrote);
  EXPECT_EQ(result->registerAddress, 0x11);
  EXPECT_EQ(result->describe(), "encoder 1 register 0x11 = 0x08 (8)");
}

TEST(WriteEncoderRegister, PacksTheSpecificationsWorkedExample) {
  // The firmware specification's own example: write 0x08 into register 0x11 of encoder 2.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{1, 0, 0x08, 0, 0, 0, 0, 0}};
  auto result = drive->writeEncoderRegister(somanet::EncoderOrdinal::kEncoder2, 0x11, 0x08,
                                            {.pollInterval = kNoDelay});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(driver.lastCommand, (std::vector<uint8_t>{0, 2, 1, 0x11, 0x08, 0, 0, 0}));
  // The drive answers a write the way it answers a read, so the write confirms itself.
  EXPECT_TRUE(result->wrote);
  EXPECT_EQ(result->value, 0x08);
}

TEST(WriteEncoderRegister, ASoftResetIsAcknowledgedWithoutAValue) {
  // Status 0 — completed with no response at all. The one access the firmware documents as
  // answering this way is the iC-MU soft reset, which restarts the chip.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{0, 0, 0, 0, 0, 0, 0, 0}};
  auto result = drive->writeEncoderRegister(somanet::EncoderOrdinal::kEncoder1, 0x75, 0x07,
                                            {.pollInterval = kNoDelay});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_FALSE(result->value.has_value());
  EXPECT_EQ(result->describe(), "encoder 1 register 0x75 write acknowledged without a response");
}

TEST(ReadEncoderRegister, NamesACommandSpecificFault) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{3, 0, 1, 0, 0, 0, 0, 0}};  // command-specific code 1
  auto result = drive->readEncoderRegister(somanet::EncoderOrdinal::kEncoder1, 0x11,
                                           {.pollInterval = kNoDelay});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("register not allowed"), std::string::npos) << result.error();
}

TEST(ReadEncoderRegister, AGeneralOsErrorNamesItself) {
  // What a non-BiSS or unconfigured encoder produces: the drive refuses the command outright.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{3, 0, 251, 0, 0, 0, 0, 0}};
  auto result = drive->readEncoderRegister(somanet::EncoderOrdinal::kEncoder2, 0x11,
                                           {.pollInterval = kNoDelay});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("command not allowed"), std::string::npos) << result.error();
  EXPECT_NE(result.error().find("encoder 2"), std::string::npos) << result.error();
}

TEST(ReadEncoderRegister, AFailureWithoutACodeNamesWhatCanCauseIt) {
  // Status 2: the BiSS transaction failed and the drive sent no code. The causes are all things a
  // user can act on, so the message names them rather than reporting a bare failure.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{2, 0, 0, 0, 0, 0, 0, 0}};
  auto result = drive->readEncoderRegister(somanet::EncoderOrdinal::kEncoder1, 0x11,
                                           {.pollInterval = kNoDelay});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("BiSS clock frequency"), std::string::npos) << result.error();
}

// --- iC-MU calibration mode (OS command 1) ------------------------------------------------------

TEST(SetIcMuCalibrationMode, PacksTheModeAboveTheEncoderOrdinal) {
  // The specification's own example: encoder 1 into raw mode is byte 1 = 9 — the mode (1) shifted
  // up three bits over the ordinal (1). Packing them into one byte is what makes this worth
  // pinning: a mode written beside the ordinal instead of above it would silently select
  // encoder 9.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{0, 0, 0, 0, 0, 0, 0, 0}};
  ASSERT_TRUE(drive
                  ->setIcMuCalibrationMode(somanet::EncoderOrdinal::kEncoder1,
                                           somanet::IcMuCalibrationMode::kRaw,
                                           {.pollInterval = kNoDelay})
                  .has_value());
  EXPECT_EQ(driver.lastCommand, (std::vector<uint8_t>{1, 9, 0, 0, 0, 0, 0, 0}));
}

TEST(SetIcMuCalibrationMode, PacksConfigurationModeOnEncoderOne) {
  // The other half of the specification's example: configuration mode is 0, so byte 1 is the bare
  // ordinal.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{0, 0, 0, 0, 0, 0, 0, 0}};
  ASSERT_TRUE(drive
                  ->setIcMuCalibrationMode(somanet::EncoderOrdinal::kEncoder1,
                                           somanet::IcMuCalibrationMode::kConfiguration,
                                           {.pollInterval = kNoDelay})
                  .has_value());
  EXPECT_EQ(driver.lastCommand, (std::vector<uint8_t>{1, 1, 0, 0, 0, 0, 0, 0}));
}

TEST(SetIcMuCalibrationMode, PacksStandardModeOnEncoderTwo) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{0, 0, 0, 0, 0, 0, 0, 0}};
  ASSERT_TRUE(drive
                  ->setIcMuCalibrationMode(somanet::EncoderOrdinal::kEncoder2,
                                           somanet::IcMuCalibrationMode::kStandard,
                                           {.pollInterval = kNoDelay})
                  .has_value());
  // Mode 2 shifted up three bits is 16, plus ordinal 2.
  EXPECT_EQ(driver.lastCommand, (std::vector<uint8_t>{1, 18, 0, 0, 0, 0, 0, 0}));
}

TEST(SetIcMuCalibrationMode, AGeneralOsErrorNamesItself) {
  // What a non-Circulo or unconfigured encoder produces — this command's only failures are general
  // codes, since the specification gives it none of its own.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{3, 0, 251, 0, 0, 0, 0, 0}};
  auto result =
      drive->setIcMuCalibrationMode(somanet::EncoderOrdinal::kEncoder1,
                                    somanet::IcMuCalibrationMode::kRaw, {.pollInterval = kNoDelay});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("command not allowed"), std::string::npos) << result.error();
  EXPECT_NE(result.error().find("raw"), std::string::npos) << result.error();
}

TEST(SetIcMuCalibrationMode, AFailureWithoutACodeNamesTheSensorService) {
  // Status 2 is this command's own failure: the sensor service did not accept the mode value.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{2, 0, 0, 0, 0, 0, 0, 0}};
  auto result =
      drive->setIcMuCalibrationMode(somanet::EncoderOrdinal::kEncoder1,
                                    somanet::IcMuCalibrationMode::kRaw, {.pollInterval = kNoDelay});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("sensor service"), std::string::npos) << result.error();
}

TEST(ParseIcMuCalibrationMode, RoundTripsEveryMode) {
  for (auto mode : {somanet::IcMuCalibrationMode::kConfiguration,
                    somanet::IcMuCalibrationMode::kRaw, somanet::IcMuCalibrationMode::kStandard}) {
    auto parsed = somanet::parseIcMuCalibrationMode(somanet::toString(mode));
    ASSERT_TRUE(parsed.has_value()) << somanet::toString(mode);
    EXPECT_EQ(*parsed, mode);
  }
  EXPECT_FALSE(somanet::parseIcMuCalibrationMode("calibration").has_value());
}

// --- HRD streaming (OS command 3) ---------------------------------------------------------------

TEST(ConfigureHrdStream, PacksTheDurationBigEndian) {
  // The specification's own example: 5 s of encoder raw data is 5000 = 0x1388, MSB in byte 3. The
  // OS command payload convention is big-endian even though the recorded *files* are little-endian,
  // so this is the pair of bytes most likely to be written the wrong way round.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{0, 0, 0, 0, 0, 0, 0, 0}};
  ASSERT_TRUE(drive
                  ->configureHrdStream(somanet::HrdData::kEncoderRawData,
                                       std::chrono::milliseconds(5000), {.pollInterval = kNoDelay})
                  .has_value());
  EXPECT_EQ(driver.lastCommand, (std::vector<uint8_t>{3, 0, 0, 0x13, 0x88, 0, 0, 0}));
}

TEST(ConfigureHrdStream, CarriesTheDataIndex) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{0, 0, 0, 0, 0, 0, 0, 0}};
  ASSERT_TRUE(drive
                  ->configureHrdStream(somanet::HrdData::kSystemIdentificationData,
                                       std::chrono::milliseconds(256), {.pollInterval = kNoDelay})
                  .has_value());
  EXPECT_EQ(driver.lastCommand, (std::vector<uint8_t>{3, 0, 1, 0x01, 0x00, 0, 0, 0}));
}

TEST(ConfigureHrdStream, RefusesAnOverLongSystemIdentificationRecordingWithoutAsking) {
  // 7000 ms of 6-byte samples does not fit the drive's five files. The drive refuses it too, so
  // what this pins is that the refusal costs no round trip — hence the assertion that no command
  // was written at all — and that the limit checked is the one for the chosen data rather than the
  // wider ceiling the other format gets.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  auto result =
      drive->configureHrdStream(somanet::HrdData::kSystemIdentificationData,
                                std::chrono::milliseconds(7000), {.pollInterval = kNoDelay});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("6000"), std::string::npos) << result.error();
  EXPECT_EQ(driver.commandWrites, 0);
}

TEST(ConfigureHrdStream, AcceptsTheSameDurationForEncoderRawData) {
  // The same 7000 ms is fine for 4-byte samples: the limit is what fills the files, not a figure
  // the two formats share.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{0, 0, 0, 0, 0, 0, 0, 0}};
  EXPECT_TRUE(drive
                  ->configureHrdStream(somanet::HrdData::kEncoderRawData,
                                       std::chrono::milliseconds(7000), {.pollInterval = kNoDelay})
                  .has_value());
}

TEST(ConfigureHrdStream, ADurationFaultNamesItself) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{3, 0, 2, 0, 0, 0, 0, 0}};
  auto result =
      drive->configureHrdStream(somanet::HrdData::kEncoderRawData, std::chrono::milliseconds(1000),
                                {.pollInterval = kNoDelay});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("duration value"), std::string::npos) << result.error();
}

TEST(StartHrdStream, SendsTheActionAlone) {
  // A start request carries no data index and no duration — the drive already has both. Sending
  // them again would be harmless but would hide which command actually configures a recording.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{0, 0, 0, 0, 0, 0, 0, 0}};
  ASSERT_TRUE(drive->startHrdStream({.pollInterval = kNoDelay}).has_value());
  EXPECT_EQ(driver.lastCommand, (std::vector<uint8_t>{3, 1, 0, 0, 0, 0, 0, 0}));
}

TEST(StartHrdStream, AGeneralOsErrorNamesItself) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{3, 0, 251, 0, 0, 0, 0, 0}};
  auto result = drive->startHrdStream({.pollInterval = kNoDelay});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("command not allowed"), std::string::npos) << result.error();
}

TEST(ParseHrdData, RoundTripsBothSelections) {
  for (auto data :
       {somanet::HrdData::kEncoderRawData, somanet::HrdData::kSystemIdentificationData}) {
    auto parsed = somanet::parseHrdData(somanet::toString(data));
    ASSERT_TRUE(parsed.has_value()) << somanet::toString(data);
    EXPECT_EQ(*parsed, data);
  }
  EXPECT_FALSE(somanet::parseHrdData("encoder").has_value());
}

// --- Device file list ---------------------------------------------------------------------------

TEST(ParseDeviceFileList, ReadsNameAndSize) {
  const auto files =
      mm::node::parseDeviceFileList("hr_data0.bin, size: 8032\r\nhr_data1.bin, size: 1968\r\n");
  ASSERT_EQ(files.size(), 2u);
  EXPECT_EQ(files[0].name, "hr_data0.bin");
  ASSERT_TRUE(files[0].byteCount.has_value());
  EXPECT_EQ(*files[0].byteCount, 8032u);
  EXPECT_EQ(files[1].name, "hr_data1.bin");
  EXPECT_EQ(*files[1].byteCount, 1968u);
}

TEST(ParseDeviceFileList, KeepsALineThatCarriesNoSize) {
  // A name is what a caller needs; a line formatted unexpectedly should not make the file
  // unreachable, and should not make the files around it unreachable either.
  const auto files = mm::node::parseDeviceFileList("config.csv\nhr_data0.bin, size: 12\n");
  ASSERT_EQ(files.size(), 2u);
  EXPECT_EQ(files[0].name, "config.csv");
  EXPECT_FALSE(files[0].byteCount.has_value());
  EXPECT_EQ(files[1].name, "hr_data0.bin");
  EXPECT_TRUE(files[1].byteCount.has_value());
}

TEST(ParseDeviceFileList, SkipsBlankLinesAndTrimsSpace) {
  const auto files = mm::node::parseDeviceFileList("\n  hr_data0.bin ,  size:  40  \n\n");
  ASSERT_EQ(files.size(), 1u);
  EXPECT_EQ(files[0].name, "hr_data0.bin");
  EXPECT_EQ(*files[0].byteCount, 40u);
}

TEST(ParseDeviceFileList, TakesTheCommaThatActuallySeparatesTheSize) {
  // A filename may contain a comma, so the separator is the first comma whose *tail* is a size —
  // splitting on the first comma outright would truncate the name.
  const auto files = mm::node::parseDeviceFileList("odd,name.bin, size: 7\n");
  ASSERT_EQ(files.size(), 1u);
  EXPECT_EQ(files[0].name, "odd,name.bin");
  EXPECT_EQ(*files[0].byteCount, 7u);
}

TEST(ReadFileList, ReadsTheListingPseudoFile) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  const std::string listing = "config.csv, size: 10\nhr_data0.bin, size: 4\n";
  driver.files["fs-getlist"] = std::vector<uint8_t>(listing.begin(), listing.end());

  auto files = drive->readFileList();
  ASSERT_TRUE(files.has_value()) << files.error();
  ASSERT_EQ(files->size(), 2u);
  EXPECT_EQ((*files)[0].name, "config.csv");
  EXPECT_EQ((*files)[1].name, "hr_data0.bin");
}

TEST(ReadFileList, ReportsAFailedListing) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  auto files = drive->readFileList();
  ASSERT_FALSE(files.has_value());
  EXPECT_NE(files.error().find("fs-getlist"), std::string::npos) << files.error();
}

// --- HRD recording readback ---------------------------------------------------------------------

TEST(DecodeHrdSamples, ReadsEncoderRawDataLittleEndianAndSplitsTheTracks) {
  // 0x0AC01234: master is the low 14 bits (0x1234 & 0x3FFF = 0x1234), nonius the next 14
  // (0x0AC01234 >> 14 & 0x3FFF = 0x2B00). Little-endian in the file, unlike the OS command payload
  // that configured the recording — this is the byte order the firmware writes, not the one it
  // parses.
  const std::vector<uint8_t> bytes{0x34, 0x12, 0xC0, 0x0A};
  const auto samples = mm::node::decodeHrdSamples(bytes, somanet::HrdData::kEncoderRawData);
  const auto& encoder = std::get<std::vector<mm::node::HrdEncoderSample>>(samples);
  ASSERT_EQ(encoder.size(), 1u);
  EXPECT_EQ(encoder[0].raw, 0x0AC01234u);
  EXPECT_EQ(encoder[0].masterCount, 0x1234u);
  EXPECT_EQ(encoder[0].noniusCount, 0x2B00u);
}

TEST(DecodeHrdSamples, ReadsSystemIdentificationVelocityOutOfQ15) {
  // 32768 in Q15 is 1 RPM; the torque is a plain little-endian int16. Both signed, so the sample
  // below is also the check that a negative velocity does not read as a huge positive one.
  const std::vector<uint8_t> bytes{0x00, 0x80, 0xFF, 0xFF, 0xF6, 0xFF};
  const auto samples =
      mm::node::decodeHrdSamples(bytes, somanet::HrdData::kSystemIdentificationData);
  const auto& system = std::get<std::vector<mm::node::HrdSystemIdentificationSample>>(samples);
  ASSERT_EQ(system.size(), 1u);
  EXPECT_DOUBLE_EQ(system[0].velocityRpm, -1.0);
  EXPECT_EQ(system[0].torquePermil, -10);
}

TEST(DecodeHrdSamples, IgnoresATrailingPartialSample) {
  // The drive allocates its files in fixed-size blocks, so a recording that does not fill the last
  // one ends in padding. Decoding it as a sample would put invented data at the end of every
  // recording.
  const std::vector<uint8_t> bytes{1, 0, 0, 0, 2, 0};
  const auto samples = mm::node::decodeHrdSamples(bytes, somanet::HrdData::kEncoderRawData);
  EXPECT_EQ(std::get<std::vector<mm::node::HrdEncoderSample>>(samples).size(), 1u);
}

TEST(HrdRecordingCsv, StartsWithTheColumnNamesAndEndsEveryRow) {
  mm::node::HrdRecording recording;
  recording.data = somanet::HrdData::kEncoderRawData;
  recording.samples = std::vector<mm::node::HrdEncoderSample>{
      {.raw = 7, .masterCount = 7, .noniusCount = 0},
      {.raw = 0x4001, .masterCount = 1, .noniusCount = 1},
  };
  // The header is the same list the JSON rendering publishes as `columns`, so a consumer reading
  // either format is reading the same field order.
  EXPECT_EQ(mm::node::toCsv(recording), "raw,masterCount,noniusCount\n7,7,0\n16385,1,1\n");
}

TEST(HrdRecordingCsv, WritesVelocityAsADecimal) {
  mm::node::HrdRecording recording;
  recording.data = somanet::HrdData::kSystemIdentificationData;
  recording.samples = std::vector<mm::node::HrdSystemIdentificationSample>{
      {.velocityRpm = -1.5, .torquePermil = -10},
  };
  EXPECT_EQ(mm::node::toCsv(recording), "velocityRpm,torquePermil\n-1.5,-10\n");
}

TEST(HrdRecordingCsv, IsAHeaderAloneWhenNothingWasRecorded) {
  // Not an empty file: a consumer that reads the header to learn the columns must still get them.
  mm::node::HrdRecording recording;
  recording.data = somanet::HrdData::kSystemIdentificationData;
  recording.samples = std::vector<mm::node::HrdSystemIdentificationSample>{};
  EXPECT_EQ(mm::node::toCsv(recording), "velocityRpm,torquePermil\n");
}

TEST(ReadHrdRecording, ConcatenatesTheFilesInNumericOrder) {
  // The listing is deliberately out of order, and a sample deliberately straddles the boundary
  // between the two files: the firmware chunks one byte stream at a fixed size rather than padding
  // each file to whole samples, so reassembling in the wrong order does not lose samples — it
  // silently corrupts every one after the seam.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  const std::string listing = "hr_data1.bin, size: 4\nhr_data0.bin, size: 4\n";
  driver.files["fs-getlist"] = std::vector<uint8_t>(listing.begin(), listing.end());
  driver.files["hr_data0.bin"] = {0x01, 0x00, 0x00, 0x00, 0x02, 0x00};
  driver.files["hr_data1.bin"] = {0x00, 0x00, 0x03, 0x00, 0x00, 0x00};

  auto recording = drive->readHrdRecording(somanet::HrdData::kEncoderRawData);
  ASSERT_TRUE(recording.has_value()) << recording.error();
  ASSERT_EQ(recording->files.size(), 2u);
  EXPECT_EQ(recording->files[0].name, "hr_data0.bin");
  EXPECT_EQ(recording->files[1].name, "hr_data1.bin");
  EXPECT_EQ(recording->byteCount, 12u);
  EXPECT_EQ(recording->trailingBytes, 0u);

  const auto& samples = std::get<std::vector<mm::node::HrdEncoderSample>>(recording->samples);
  ASSERT_EQ(samples.size(), 3u);
  EXPECT_EQ(samples[0].raw, 1u);
  EXPECT_EQ(samples[1].raw, 2u);  // straddles the file boundary
  EXPECT_EQ(samples[2].raw, 3u);
}

TEST(ReadHrdRecording, ReportsThePaddingItDidNotDecode) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  const std::string listing = "hr_data0.bin, size: 6\n";
  driver.files["fs-getlist"] = std::vector<uint8_t>(listing.begin(), listing.end());
  driver.files["hr_data0.bin"] = {1, 0, 0, 0, 0xFF, 0xFF};

  auto recording = drive->readHrdRecording(somanet::HrdData::kEncoderRawData);
  ASSERT_TRUE(recording.has_value()) << recording.error();
  EXPECT_EQ(recording->sampleCount(), 1u);
  EXPECT_EQ(recording->trailingBytes, 2u);
}

TEST(ReadHrdRecording, SaysWhenTheDeviceHoldsNoRecording) {
  // A device that has never recorded lists no hr_data file at all, which is a different thing from
  // a read that failed — and the message has to say which, because the fix is to record one.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  const std::string listing = "config.csv, size: 10\n";
  driver.files["fs-getlist"] = std::vector<uint8_t>(listing.begin(), listing.end());

  auto recording = drive->readHrdRecording(somanet::HrdData::kEncoderRawData);
  ASSERT_FALSE(recording.has_value());
  EXPECT_NE(recording.error().find("no high resolution data recording"), std::string::npos)
      << recording.error();
}

TEST(ReadHrdRecording, FailsRatherThanSkipAFileItCannotRead) {
  // Skipping a file would misalign every sample after it and still return "success", which is the
  // one outcome worse than an error: a graph of plausible nonsense.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  const std::string listing = "hr_data0.bin, size: 4\nhr_data1.bin, size: 4\n";
  driver.files["fs-getlist"] = std::vector<uint8_t>(listing.begin(), listing.end());
  driver.files["hr_data0.bin"] = {1, 0, 0, 0};

  auto recording = drive->readHrdRecording(somanet::HrdData::kEncoderRawData);
  ASSERT_FALSE(recording.has_value());
  EXPECT_NE(recording.error().find("hr_data1.bin"), std::string::npos) << recording.error();
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

// --- Motor phase order detection (command 4) -----------------------------------------------------

TEST(RunMotorPhaseOrderDetection, DecodesNormalOrder) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{1, 0, 0, 0, 0, 0, 0, 0}};
  auto result = drive->runMotorPhaseOrderDetection({.pollInterval = kNoDelay});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(result->order, somanet::MotorPhaseOrder::kNormal);
  EXPECT_FALSE(result->inverted());
  EXPECT_EQ(result->describe(), "motor phase order is normal");
  EXPECT_EQ(driver.store.at(OsCommandFakeDriver::key(kOsCommand, 1))[0], 4);
}

TEST(RunMotorPhaseOrderDetection, DecodesInvertedOrder) {
  // The specification's example: 0x0000000000010001 — status 1, inverted.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{1, 0, 1, 0, 0, 0, 0, 0}};
  auto result = drive->runMotorPhaseOrderDetection({.pollInterval = kNoDelay});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_TRUE(result->inverted());
  EXPECT_EQ(result->describe(), "motor phase order is inverted");
}

TEST(RunMotorPhaseOrderDetection, RejectsAnOrderValueItCannotInterpret) {
  // Only 0 and 1 are defined. Guessing at anything else would misreport how a motor is wired, which
  // commutation offset measurement then builds on.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{1, 0, 7, 0, 0, 0, 0, 0}};
  auto result = drive->runMotorPhaseOrderDetection({.pollInterval = kNoDelay});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("neither normal (0) nor inverted (1)"), std::string::npos)
      << result.error();
}

TEST(RunMotorPhaseOrderDetection, DoesNotReadCodeZeroAsACurrentAmplitudeFault) {
  // This command has no command-specific codes at all, so code 0 means nothing here — while for
  // pole pair, resistance and inductance it means "current amplitude error". Naming it would invent
  // a motor diagnosis, so the raw number is reported instead.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{3, 0, 0, 0, 0, 0, 0, 0}};
  auto result = drive->runMotorPhaseOrderDetection({.pollInterval = kNoDelay});
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().find("current"), std::string::npos) << result.error();
  EXPECT_NE(result.error().find("OS error 0"), std::string::npos) << result.error();
}

TEST(RunMotorPhaseOrderDetection, AGeneralOsErrorNamesItself) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{3, 0, 251, 0, 0, 0, 0, 0}};
  auto result = drive->runMotorPhaseOrderDetection({.pollInterval = kNoDelay});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("command not allowed"), std::string::npos) << result.error();
}

// --- Commutation offset measurement (command 5) --------------------------------------------------

TEST(RunCommutationOffsetMeasurement, DecodesTheTwoByteBigEndianOffset) {
  // The specification's example: 2035 (0x07F3) in response bytes 2-3.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{1, 0, 0x07, 0xF3, 0, 0, 0, 0}};
  auto result = drive->runCommutationOffsetMeasurement(somanet::CommutationOffsetMethod::kRotating,
                                                       {.pollInterval = kNoDelay});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(result->angleOffset, 2035);
  EXPECT_EQ(result->method, somanet::CommutationOffsetMethod::kRotating);
  EXPECT_EQ(result->describe(), "commutation angle offset 2035 (rotating method)");
  EXPECT_EQ(driver.store.at(OsCommandFakeDriver::key(kOsCommand, 1))[0], 5);
}

TEST(RunCommutationOffsetMeasurement, RecordsTheMethodItRanUnder) {
  // The offset alone does not say whether the rotor turned to produce it, so the method travels
  // with the value rather than being left for a reader to look up separately.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{1, 0, 0x00, 0x10, 0, 0, 0, 0}};
  auto result = drive->runCommutationOffsetMeasurement(
      somanet::CommutationOffsetMethod::kStationary, {.pollInterval = kNoDelay});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(result->method, somanet::CommutationOffsetMethod::kStationary);
  EXPECT_NE(result->describe().find("stationary"), std::string::npos) << result->describe();
}

TEST(RunCommutationOffsetMeasurement, AGeneralOsErrorNamesItself) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{3, 0, 251, 0, 0, 0, 0, 0}};
  auto result = drive->runCommutationOffsetMeasurement(somanet::CommutationOffsetMethod::kRotating,
                                                       {.pollInterval = kNoDelay});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("command not allowed"), std::string::npos) << result.error();
}

TEST(RequiresBrakeReleased, IsTrueForTheRotatingMethodsOnly) {
  // The stationary method wants the brake *engaged*, so this is not a "needs no brake handling"
  // flag.
  EXPECT_TRUE(somanet::requiresBrakeReleased(somanet::CommutationOffsetMethod::kRotating));
  EXPECT_TRUE(somanet::requiresBrakeReleased(somanet::CommutationOffsetMethod::kRotatingTuned));
  EXPECT_FALSE(somanet::requiresBrakeReleased(somanet::CommutationOffsetMethod::kStationary));
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

// --- Torque constant measurement (command 10) ----------------------------------------------------

TEST(RunTorqueConstantMeasurement, DecodesTheBigEndianValue) {
  // The specification's example value, 1203210 mNm/A_rms — written here as the firmware packs it,
  // most significant byte at byte 2, which is *not* how that document's worked example renders it.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{1, 0, 0x00, 0x12, 0x5C, 0x0A, 0, 0}};
  auto result = drive->runTorqueConstantMeasurement({.pollInterval = kNoDelay});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(result->milliNewtonMetresPerAmpere, 1203210);
  EXPECT_EQ(result->describe(), "torque constant 1203210 mNm/A_rms");
}

TEST(RunTorqueConstantMeasurement, IssuesCommandTen) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{1, 0, 0, 0, 0, 1, 0, 0}};
  ASSERT_TRUE(drive->runTorqueConstantMeasurement({.pollInterval = kNoDelay}).has_value());
  EXPECT_EQ(driver.store.at(OsCommandFakeDriver::key(kOsCommand, 1))[0], 10);
}

TEST(RunTorqueConstantMeasurement, ReportsANegativeValueRatherThanAHugeUnsignedOne) {
  // The one measurement here whose value is signed. The drive subtracts the configured winding
  // impedance from the applied voltage to find the back-EMF, so a resistance or inductance
  // configured far above the motor's real one drives the result below zero — and that is the
  // signal the caller needs. Read unsigned it would come back as roughly 4.29 billion.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{1, 0, 0xFF, 0xFF, 0xFF, 0xFE, 0, 0}};
  auto result = drive->runTorqueConstantMeasurement({.pollInterval = kNoDelay});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(result->milliNewtonMetresPerAmpere, -2);
}

TEST(RunTorqueConstantMeasurement, DoesNotReadCodeZeroAsACurrentAmplitudeFault) {
  // Like motor phase order detection, this command defines no command-specific codes, so code 0
  // means nothing here even though its siblings read it as "current amplitude error".
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{3, 0, 0, 0, 0, 0, 0, 0}};
  auto result = drive->runTorqueConstantMeasurement({.pollInterval = kNoDelay});
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().find("current"), std::string::npos) << result.error();
  EXPECT_NE(result.error().find("OS error 0"), std::string::npos) << result.error();
}

TEST(RunTorqueConstantMeasurement, AGeneralOsErrorNamesItself) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{3, 0, 251, 0, 0, 0, 0, 0}};
  auto result = drive->runTorqueConstantMeasurement({.pollInterval = kNoDelay});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("torque constant measurement"), std::string::npos)
      << result.error();
  EXPECT_NE(result.error().find("command not allowed"), std::string::npos) << result.error();
}

// --- Velocity source (command 18) ----------------------------------------------------------------

TEST(SetVelocitySource, PutsTheSourceInBitZeroOfByteOne) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{0, 0, 0, 0, 0, 0, 0, 0}};
  ASSERT_TRUE(
      drive->setVelocitySource(somanet::VelocitySource::kEncoder, {.pollInterval = kNoDelay})
          .has_value());
  const auto written = driver.store.at(OsCommandFakeDriver::key(kOsCommand, 1));
  ASSERT_EQ(written.size(), 8u);
  EXPECT_EQ(written[0], 18);
  EXPECT_EQ(written[1], 1);
}

TEST(SetVelocitySource, ClearsTheBitForTheFirmwareValue) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{0, 0, 0, 0, 0, 0, 0, 0}};
  ASSERT_TRUE(
      drive->setVelocitySource(somanet::VelocitySource::kFirmware, {.pollInterval = kNoDelay})
          .has_value());
  EXPECT_EQ(driver.store.at(OsCommandFakeDriver::key(kOsCommand, 1))[1], 0);
}

TEST(SetVelocitySource, AGeneralOsErrorNamesItself) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{3, 0, 254, 0, 0, 0, 0, 0}};
  auto result =
      drive->setVelocitySource(somanet::VelocitySource::kEncoder, {.pollInterval = kNoDelay});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("unsupported command"), std::string::npos) << result.error();
  EXPECT_NE(result.error().find("velocity from the encoder"), std::string::npos) << result.error();
}

TEST(ParseVelocitySource, RoundTripsItsOwnTokens) {
  EXPECT_EQ(somanet::parseVelocitySource("firmware"), somanet::VelocitySource::kFirmware);
  EXPECT_EQ(somanet::parseVelocitySource("encoder"), somanet::VelocitySource::kEncoder);
  EXPECT_FALSE(somanet::parseVelocitySource("").has_value());
  EXPECT_FALSE(somanet::parseVelocitySource("kuebler").has_value());
}

// --- Trigger error (command 16) ------------------------------------------------------------------

TEST(FirmwareErrorEffect, MatchesWhatTheFirmwareActuallyDoes) {
  // Taken from the firmware's own case bodies, not from the type names — seven of the twelve are
  // named after an exception it never raises, and the specification marks those "(disabled)".
  using somanet::effectOf;
  using somanet::FirmwareErrorEffect;
  using somanet::FirmwareErrorType;
  for (const auto type : {FirmwareErrorType::kLinkError, FirmwareErrorType::kIllegalPc,
                          FirmwareErrorType::kIllegalInstruction,
                          FirmwareErrorType::kIllegalResource, FirmwareErrorType::kIllegalPs,
                          FirmwareErrorType::kResourceDependency, FirmwareErrorType::kKcall}) {
    EXPECT_EQ(effectOf(type), FirmwareErrorEffect::kNotImplemented) << somanet::toString(type);
  }
  for (const auto type : {FirmwareErrorType::kLoadStore, FirmwareErrorType::kArithmetic,
                          FirmwareErrorType::kEcall, FirmwareErrorType::kEndlessLoop}) {
    EXPECT_EQ(effectOf(type), FirmwareErrorEffect::kStopsService) << somanet::toString(type);
  }
  EXPECT_EQ(effectOf(FirmwareErrorType::kResettableFirmwareError),
            FirmwareErrorEffect::kRaisesResettableError);
}

TEST(ParseFirmwareErrorType, RoundTripsEveryType) {
  for (const auto type : somanet::kFirmwareErrorTypes) {
    EXPECT_EQ(somanet::parseFirmwareErrorType(somanet::toString(type)), type)
        << somanet::toString(type);
  }
  EXPECT_FALSE(somanet::parseFirmwareErrorType("").has_value());
  EXPECT_FALSE(somanet::parseFirmwareErrorType("ET_ECALL").has_value());
}

TEST(TriggerError, PutsTheServiceAndTypeInBytesOneAndTwo) {
  // The specification's example: ECALL on motion control is bytes 16, 1, 7.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{2, 0, 0, 0, 0, 0, 0, 0}};
  auto result = drive->triggerError(somanet::FirmwareService::kMotionControl,
                                    somanet::FirmwareErrorType::kEcall, {.pollInterval = kNoDelay});
  ASSERT_TRUE(result.has_value()) << result.error();
  const auto written = driver.store.at(OsCommandFakeDriver::key(kOsCommand, 1));
  ASSERT_EQ(written.size(), 8u);
  EXPECT_EQ(written[0], 16);
  EXPECT_EQ(written[1], 1);
  EXPECT_EQ(written[2], 7);
}

TEST(TriggerError, AServiceThatAnswersDidNotStop) {
  // For a type meant to stop the service, an answer is the interesting negative result: the drive
  // is still running, so the error was not triggered.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{2, 0, 0, 0, 0, 0, 0, 0}};
  auto result =
      drive->triggerError(somanet::FirmwareService::kDriveControl,
                          somanet::FirmwareErrorType::kEndlessLoop, {.pollInterval = kNoDelay});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_FALSE(result->serviceStopped);
  EXPECT_NE(result->describe().find("did not stop it"), std::string::npos) << result->describe();
}

TEST(TriggerError, NoAnswerIsTheIntendedOutcomeForAStoppingType) {
  // A service that executes an unaligned store never answers again. Reporting that timeout as a
  // failure would call the command's success a fault — which is the whole reason this command
  // needs a result type rather than a bare expected<void>.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{255, 0, 0, 0, 0, 0, 0, 0}};  // "in progress", for ever
  auto result = drive->triggerError(somanet::FirmwareService::kDriveControl,
                                    somanet::FirmwareErrorType::kLoadStore,
                                    {.timeout = std::chrono::milliseconds(20),
                                     .pollInterval = kNoDelay,
                                     .abortTimeout = std::chrono::milliseconds(20)});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_TRUE(result->serviceStopped);
  EXPECT_NE(result->describe().find("needs a power cycle"), std::string::npos)
      << result->describe();
}

TEST(TriggerError, ANotImplementedTypeSaysNothingHappened) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{2, 0, 0, 0, 0, 0, 0, 0}};
  auto result =
      drive->triggerError(somanet::FirmwareService::kDriveControl,
                          somanet::FirmwareErrorType::kIllegalPc, {.pollInterval = kNoDelay});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_NE(result->describe().find("not implemented"), std::string::npos) << result->describe();
}

TEST(TriggerError, TheResettableTypeIsReportedTheSameFromEitherService) {
  // The two services disagree about the status byte for this type — motion control answers success,
  // drive control answers failure — because of where each sets its default, and the error is raised
  // either way. So the status must not be the verdict.
  for (const auto service :
       {somanet::FirmwareService::kDriveControl, somanet::FirmwareService::kMotionControl}) {
    for (const uint8_t status : {uint8_t{0}, uint8_t{2}}) {
      OsCommandFakeDriver driver;
      Device device = makeOsCommandDevice(driver);
      auto drive = createSomanetDrive(device);
      ASSERT_TRUE(drive.has_value()) << drive.error();

      driver.responses = {{status, 0, 0, 0, 0, 0, 0, 0}};
      auto result =
          drive->triggerError(service, somanet::FirmwareErrorType::kResettableFirmwareError,
                              {.pollInterval = kNoDelay});
      ASSERT_TRUE(result.has_value()) << result.error();
      EXPECT_NE(result->describe().find("resettable DiagErr"), std::string::npos)
          << result->describe();
    }
  }
}

// --- System identification (command 15) ----------------------------------------------------------

TEST(SetSystemIdentificationParameter, PutsTheIndexInByteOneAndTheValueBigEndian) {
  // The specification's worked example: transition time (index 3) = 10 ms.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{0, 0, 0, 0, 0, 0, 0, 0}};
  ASSERT_TRUE(drive
                  ->setSystemIdentificationParameter(
                      somanet::SystemIdentificationParameter::kTransitionTime, 10,
                      {.pollInterval = kNoDelay})
                  .has_value());
  const auto written = driver.store.at(OsCommandFakeDriver::key(kOsCommand, 1));
  ASSERT_EQ(written.size(), 8u);
  EXPECT_EQ(written[0], 15);
  EXPECT_EQ(written[1], 3);
  EXPECT_EQ(written[2], 0x00);
  EXPECT_EQ(written[3], 0x00);
  EXPECT_EQ(written[4], 0x00);
  EXPECT_EQ(written[5], 0x0A);
}

TEST(SetSystemIdentificationParameter, SpansAllFourValueBytes) {
  // A frequency at the firmware's ceiling needs three of them, so a value truncated to one byte
  // would pass the example above and fail here.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{0, 0, 0, 0, 0, 0, 0, 0}};
  ASSERT_TRUE(drive
                  ->setSystemIdentificationParameter(
                      somanet::SystemIdentificationParameter::kTargetFrequency,
                      somanet::kMaxChirpFrequencyMilliHz, {.pollInterval = kNoDelay})
                  .has_value());
  const auto written = driver.store.at(OsCommandFakeDriver::key(kOsCommand, 1));
  EXPECT_EQ(written[1], 1);
  // 1000000 = 0x000F4240.
  EXPECT_EQ(written[2], 0x00);
  EXPECT_EQ(written[3], 0x0F);
  EXPECT_EQ(written[4], 0x42);
  EXPECT_EQ(written[5], 0x40);
}

TEST(SetSystemIdentificationParameter, StatusTwoNamesTheUnknownParameterIndex) {
  // This command's own failure, and its only one: the motion control service did not recognise the
  // index. It carries no error code, so a bare "failed" would say nothing.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{2, 0, 0, 0, 0, 0, 0, 0}};
  auto result = drive->setSystemIdentificationParameter(
      somanet::SystemIdentificationParameter::kSignalType, 1, {.pollInterval = kNoDelay});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("does not recognise parameter index"), std::string::npos)
      << result.error();
}

TEST(ParseChirpVocabulary, RoundTripsItsOwnTokens) {
  EXPECT_EQ(somanet::parseChirpSignalType("logarithmic"), somanet::ChirpSignalType::kLogarithmic);
  EXPECT_EQ(somanet::parseChirpSignalType("linear"), somanet::ChirpSignalType::kLinear);
  EXPECT_FALSE(somanet::parseChirpSignalType("log").has_value());

  EXPECT_EQ(somanet::parseSystemIdentificationStart("none"),
            somanet::SystemIdentificationStart::kNone);
  EXPECT_EQ(somanet::parseSystemIdentificationStart("immediately"),
            somanet::SystemIdentificationStart::kImmediately);
  EXPECT_EQ(somanet::parseSystemIdentificationStart("after-hrd-stream-start"),
            somanet::SystemIdentificationStart::kAfterHrdStreamStart);
  EXPECT_FALSE(somanet::parseSystemIdentificationStart("hrd").has_value());
}

// --- Ignore BiSS status bits (command 14) --------------------------------------------------------

TEST(SetIgnoreBissStatusBits, PacksTheEncoderAboveTheTriggerBit) {
  // The specification's own example: encoder 1 with ignoring on is byte 1 = 3 (0b11). This byte is
  // packed differently from commands 0 and 1, which put the same ordinal in the low bits — so it is
  // pinned against that worked example rather than against the shape of its siblings.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{0, 0, 0, 0, 0, 0, 0, 0}};
  ASSERT_TRUE(drive
                  ->setIgnoreBissStatusBits(somanet::EncoderOrdinal::kEncoder1, true,
                                            {.pollInterval = kNoDelay})
                  .has_value());
  const auto written = driver.store.at(OsCommandFakeDriver::key(kOsCommand, 1));
  ASSERT_EQ(written.size(), 8u);
  EXPECT_EQ(written[0], 14);
  EXPECT_EQ(written[1], 0b011);
}

TEST(SetIgnoreBissStatusBits, ClearsTheTriggerToStopIgnoring) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{0, 0, 0, 0, 0, 0, 0, 0}};
  ASSERT_TRUE(drive
                  ->setIgnoreBissStatusBits(somanet::EncoderOrdinal::kEncoder2, false,
                                            {.pollInterval = kNoDelay})
                  .has_value());
  // Encoder 2, trigger clear: 0b100. The encoder must survive the trigger going to zero.
  EXPECT_EQ(driver.store.at(OsCommandFakeDriver::key(kOsCommand, 1))[1], 0b100);
}

TEST(SetIgnoreBissStatusBits, ATimeoutSaysTheEncoderIsNotBiss) {
  // Only the BiSS service instance owning the addressed encoder answers, so nothing answering is
  // the precondition failing — not a slow drive. The bare "command timeout" would hide a
  // misconfigured encoder behind a bus-shaped symptom.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{3, 0, 253, 0, 0, 0, 0, 0}};
  auto result = drive->setIgnoreBissStatusBits(somanet::EncoderOrdinal::kEncoder2, true,
                                               {.pollInterval = kNoDelay});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("not configured or not a BiSS encoder"), std::string::npos)
      << result.error();
  EXPECT_NE(result.error().find("encoder 2"), std::string::npos) << result.error();
}

TEST(SetIgnoreBissStatusBits, AnotherGeneralErrorIsReportedAsItself) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{3, 0, 254, 0, 0, 0, 0, 0}};
  auto result = drive->setIgnoreBissStatusBits(somanet::EncoderOrdinal::kEncoder1, true,
                                               {.pollInterval = kNoDelay});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("unsupported command"), std::string::npos) << result.error();
  EXPECT_EQ(result.error().find("not a BiSS encoder"), std::string::npos) << result.error();
}

// --- Skipped cycles counter (command 13) ---------------------------------------------------------

TEST(ReadSkippedCycles, DecodesTheBigEndianCounterAndEchoesTheService) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{1, 0, 0x00, 0x00, 0x01, 0x2C, 0, 0}};  // 300
  auto result = drive->readSkippedCycles(somanet::FirmwareService::kMotionControl,
                                         {.pollInterval = kNoDelay});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(result->skippedCycles, 300u);
  // Echoed rather than assumed: two readings are only comparable when they addressed the same loop,
  // and the drive's reply does not say which one answered.
  EXPECT_EQ(result->service, somanet::FirmwareService::kMotionControl);
  EXPECT_EQ(result->describe(), "motion-control skipped 300 cycles since it started");
}

TEST(ReadSkippedCycles, PutsTheServiceInByteOne) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{1, 0, 0, 0, 0, 0, 0, 0}};
  ASSERT_TRUE(
      drive->readSkippedCycles(somanet::FirmwareService::kMotionControl, {.pollInterval = kNoDelay})
          .has_value());
  const auto written = driver.store.at(OsCommandFakeDriver::key(kOsCommand, 1));
  ASSERT_EQ(written.size(), 8u);
  EXPECT_EQ(written[0], 13);
  EXPECT_EQ(written[1], 1);
}

TEST(ReadSkippedCycles, AZeroCounterIsAReadingRatherThanAMissingPayload) {
  // Unlike a measurement command, 0 here is the answer a healthy drive gives — so the four payload
  // bytes must still be required (a short reply is an error) while their value may be zero.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{1, 0, 0, 0, 0, 0, 0, 0}};
  auto result =
      drive->readSkippedCycles(somanet::FirmwareService::kDriveControl, {.pollInterval = kNoDelay});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(result->skippedCycles, 0u);
  EXPECT_EQ(result->describe(), "drive-control skipped 0 cycles since it started");
}

TEST(ReadSkippedCycles, ATimeoutSaysTheServiceIsNotRunning) {
  // Each firmware service answers only requests naming itself, so nothing answering at all is the
  // drive saying it has no such service — not that it was slow. A bare "command timeout" would send
  // a user looking at their bus.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{3, 0, 253, 0, 0, 0, 0, 0}};
  auto result = drive->readSkippedCycles(somanet::FirmwareService::kMotionControl,
                                         {.pollInterval = kNoDelay});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("no motion-control service running"), std::string::npos)
      << result.error();
}

TEST(ReadSkippedCycles, AnotherGeneralErrorIsReportedAsItself) {
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{3, 0, 254, 0, 0, 0, 0, 0}};
  auto result =
      drive->readSkippedCycles(somanet::FirmwareService::kDriveControl, {.pollInterval = kNoDelay});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("unsupported command"), std::string::npos) << result.error();
  EXPECT_EQ(result.error().find("not running"), std::string::npos) << result.error();
}

TEST(ParseFirmwareService, RoundTripsItsOwnTokens) {
  EXPECT_EQ(somanet::parseFirmwareService("drive-control"),
            somanet::FirmwareService::kDriveControl);
  EXPECT_EQ(somanet::parseFirmwareService("motion-control"),
            somanet::FirmwareService::kMotionControl);
  EXPECT_FALSE(somanet::parseFirmwareService("").has_value());
  EXPECT_FALSE(somanet::parseFirmwareService("drive").has_value());
  EXPECT_FALSE(somanet::parseFirmwareService("0").has_value());
}

// --- What a general OS error says about the drive
// -------------------------------------------------

TEST(GeneralOsError, AnAbortNamesTheFaultTheDriveIsIn) {
  // 0x0238 and "PuUv" are what a SOMANET Integro actually reported after a torque constant
  // measurement tripped its DC-link under-voltage protection: the fault cleared Operation Enabled,
  // the firmware re-checked the command's preconditions on the next cycle and aborted it, and the
  // resulting 252 said only "command aborted". The fault is the answer, and it has to be in the
  // message — it is two object reads away and nobody thinks to make them.
  OsCommandFakeDriver driver;
  driver.programObject(somanet::kErrorReport, somanet::kErrorReportDescription,
                       ObjectDataType::VISIBLE_STRING, {'P', 'u', 'U', 'v', ' ', ' ', ' ', ' '});
  Device device = makeOsCommandDevice(driver);
  driver.store[OsCommandFakeDriver::key(Object::kStatusword, 0)] = {0x38, 0x02};
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{3, 0, 252, 0, 0, 0, 0, 0}};
  auto result = drive->runTorqueConstantMeasurement({.pollInterval = kNoDelay});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("command aborted"), std::string::npos) << result.error();
  EXPECT_NE(result.error().find("the drive is in Fault"), std::string::npos) << result.error();
  EXPECT_NE(result.error().find("PuUv"), std::string::npos) << result.error();
}

TEST(GeneralOsError, AddsNothingWhenTheDriveIsStillEnabled) {
  // The state is appended to explain the refusal, not to decorate it. A drive still in Operation
  // Enabled rules nothing out — the cause is then the operation mode, a limit switch or the brake —
  // so the message is left as it was.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  driver.store[OsCommandFakeDriver::key(Object::kStatusword, 0)] = {0x37, 0x02};
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{3, 0, 251, 0, 0, 0, 0, 0}};
  auto result = drive->runTorqueConstantMeasurement({.pollInterval = kNoDelay});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("command not allowed"), std::string::npos) << result.error();
  EXPECT_EQ(result.error().find("the drive is in"), std::string::npos) << result.error();
}

TEST(GeneralOsError, ACommandSpecificCodeIsLeftAlone) {
  // Only 251 and 252 name a precondition. A command-specific code describes what the command found,
  // so the drive's state explains nothing about it — and reading one would put a CiA402 state in
  // front of a user who was told about a motor fault.
  OsCommandFakeDriver driver;
  Device device = makeOsCommandDevice(driver);
  driver.store[OsCommandFakeDriver::key(Object::kStatusword, 0)] = {0x38, 0x02};
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{3, 0, 0, 0, 0, 0, 0, 0}};  // current amplitude error
  auto result = drive->runPhaseResistanceMeasurement({.pollInterval = kNoDelay});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("could not raise the motor phase currents"), std::string::npos)
      << result.error();
  EXPECT_EQ(result.error().find("the drive is in"), std::string::npos) << result.error();
}

TEST(GeneralOsError, OpenPhaseDetectionExplainsItsRefusalToo) {
  // The one command whose failure is normally a *finding* rather than an error — which is exactly
  // why a general code here needs explaining: it means the check never ran at all.
  OsCommandFakeDriver driver;
  driver.programObject(somanet::kErrorReport, somanet::kErrorReportDescription,
                       ObjectDataType::VISIBLE_STRING, {'P', 'u', 'U', 'v', ' ', ' ', ' ', ' '});
  Device device = makeOsCommandDevice(driver);
  driver.store[OsCommandFakeDriver::key(Object::kStatusword, 0)] = {0x38, 0x02};
  auto drive = createSomanetDrive(device);
  ASSERT_TRUE(drive.has_value()) << drive.error();

  driver.responses = {{3, 0, 251, 0, 0, 0, 0, 0}};
  auto result = drive->runOpenPhaseDetection({.pollInterval = kNoDelay});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("the drive is in Fault"), std::string::npos) << result.error();
  EXPECT_NE(result.error().find("PuUv"), std::string::npos) << result.error();
}

}  // namespace
