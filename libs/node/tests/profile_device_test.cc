#include "node/profile_device.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "comm/fieldbus_driver.h"
#include "comm/object_data_types.h"
#include "node/device.h"

namespace {

using mm::comm::EtherCatState;
using mm::comm::FieldbusDriver;
using mm::comm::ObjectDataType;
using mm::comm::OdEntry;
using mm::comm::SlaveInfo;
using mm::node::createProfileDevice;
using mm::node::Device;
using mm::node::parseRestoreGroup;
using mm::node::RestoreGroup;

constexpr uint16_t kPreOp = static_cast<uint16_t>(EtherCatState::PreOp);

// Zero settle + retry delay so the tests exercise the retry/confirm logic without real waiting.
constexpr auto kNoDelay = std::chrono::milliseconds(0);

// Generic device-profile object indices used by the tests.
constexpr uint16_t kDeviceType = 0x1000;
constexpr uint16_t kStoreParameters = 0x1010;
constexpr uint16_t kRestoreDefaultParameters = 0x1011;

// ASCII (little-endian) signatures the command objects accept.
constexpr uint32_t kSaveSignature = 0x65766173;  // "save" → 0x1010:01
constexpr uint32_t kLoadSignature = 0x64616F6C;  // "load" → 0x1011:0x

std::vector<uint8_t> u32le(uint32_t v) {
  return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
          static_cast<uint8_t>(v >> 24)};
}

/// FieldbusDriver test double for the "write a signature, poll until it reads back 1" command
/// objects — store parameters (0x1010:01) and restore default parameters (0x1011:0x). It watches
/// one configurable command object and models its confirmation behaviour deterministically (no real
/// hardware); everything else is a small SDO object store.
class CommandFakeDriver : public FieldbusDriver {
 public:
  std::map<uint32_t, std::vector<uint8_t>> store;
  std::vector<OdEntry> ods;
  uint16_t alStatus = kPreOp;

  // The command object under test (set by programCommandObject).
  uint16_t cmdIndex = kStoreParameters;
  uint8_t cmdSubindex = 1;

  // --- command behaviour ---------------------------------------------------
  bool failWrite = false;          ///< writeSdo(command object) returns an SDO-abort error.
  int writes = 0;                  ///< Count of signature writes to the command object.
  uint32_t lastSignature = 0;      ///< Last value written to the command object.
  int statusReads = 0;             ///< Count of reads of the command object.
  int readErrorsBeforeAnswer = 0;  ///< First N reads fail (mailbox busy while the device works).
  int pollsBeforeConfirm = 0;      ///< Successful reads that return 0 before it reads 1.
  bool neverConfirm = false;       ///< When set, the object always reads 0 (never completes).

  static uint32_t key(uint16_t index, uint8_t subindex) {
    return (static_cast<uint32_t>(index) << 8) | subindex;
  }

  // OTYPE_VAR object with an initial value, readable + writable in every AL state.
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

  // Seeds the mandatory device type (0x1000) and the command object under test.
  void programCommandObject(uint16_t index, uint8_t subindex) {
    cmdIndex = index;
    cmdSubindex = subindex;
    programObject(kDeviceType, 0, ObjectDataType::UNSIGNED32, u32le(0x00020192));  // 402 profile
    programObject(index, subindex, ObjectDataType::UNSIGNED32, u32le(0));
  }

  std::expected<std::vector<uint8_t>, std::string> readSdo(uint16_t, uint16_t index,
                                                           uint8_t subindex) override {
    if (index == cmdIndex && subindex == cmdSubindex) {
      ++statusReads;
      if (statusReads <= readErrorsBeforeAnswer) {
        return std::unexpected("SDOread failed (no response — mailbox timeout)");
      }
      const bool confirmed =
          !neverConfirm && statusReads > readErrorsBeforeAnswer + pollsBeforeConfirm;
      return u32le(confirmed ? 1u : 0u);
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
      if (failWrite) {
        return std::unexpected("SDO abort 0x08000000: General error");
      }
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
  uint16_t slaveState(uint16_t) const override { return alStatus; }
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

// Builds a device whose object dictionary holds the generic device type (0x1000) and the command
// object at (index, subindex), enumerated and online (PRE-OP) for SDO access.
Device makeDevice(CommandFakeDriver& driver, uint16_t index, uint8_t subindex) {
  driver.programCommandObject(index, subindex);
  Device device(1, driver);
  auto initialized = device.initializeParameters();
  EXPECT_TRUE(initialized.has_value()) << initialized.error();
  return device;
}

TEST(CreateProfileDevice, RejectsDeviceWithoutGenericArea) {
  CommandFakeDriver driver;
  Device device(1, driver);  // no parameters enumerated — 0x1000 absent
  auto profile = createProfileDevice(device);
  EXPECT_FALSE(profile.has_value());
}

TEST(CreateProfileDevice, BindsDeviceWithGenericArea) {
  CommandFakeDriver driver;
  Device device = makeDevice(driver, kStoreParameters, 1);
  auto profile = createProfileDevice(device);
  ASSERT_TRUE(profile.has_value()) << profile.error();
}

// --- store parameters (0x1010) -------------------------------------------------------------------

TEST(RunStoreParameters, WritesSaveSignatureAndConfirms) {
  CommandFakeDriver driver;
  Device device = makeDevice(driver, kStoreParameters, 1);
  auto profile = createProfileDevice(device);
  ASSERT_TRUE(profile.has_value()) << profile.error();

  // 0x1010:01 reads back 1 on the first poll → confirmed immediately.
  auto result =
      profile->runStoreParameters({.retries = 5, .interval = kNoDelay, .settle = kNoDelay});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(driver.writes, 1);
  EXPECT_EQ(driver.lastSignature, kSaveSignature);
  EXPECT_EQ(driver.statusReads, 1);  // one confirming read, no retries
}

TEST(RunStoreParameters, RetriesUntilConfirmed) {
  CommandFakeDriver driver;
  Device device = makeDevice(driver, kStoreParameters, 1);
  auto profile = createProfileDevice(device);
  ASSERT_TRUE(profile.has_value()) << profile.error();

  driver.pollsBeforeConfirm = 2;  // first two polls read 0, the third reads 1
  auto result =
      profile->runStoreParameters({.retries = 5, .interval = kNoDelay, .settle = kNoDelay});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(driver.statusReads, 3);
}

TEST(RunStoreParameters, RetriesThroughTransientReadErrors) {
  CommandFakeDriver driver;
  Device device = makeDevice(driver, kStoreParameters, 1);
  auto profile = createProfileDevice(device);
  ASSERT_TRUE(profile.has_value()) << profile.error();

  // The mailbox is unresponsive for the first two polls (store in progress), then answers 1 — a
  // read error must be retried like a value mismatch, not fail the whole call.
  driver.readErrorsBeforeAnswer = 2;
  auto result =
      profile->runStoreParameters({.retries = 5, .interval = kNoDelay, .settle = kNoDelay});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(driver.statusReads, 3);
}

TEST(RunStoreParameters, TimesOutWhenNeverConfirmed) {
  CommandFakeDriver driver;
  Device device = makeDevice(driver, kStoreParameters, 1);
  auto profile = createProfileDevice(device);
  ASSERT_TRUE(profile.has_value()) << profile.error();

  driver.neverConfirm = true;
  auto result =
      profile->runStoreParameters({.retries = 3, .interval = kNoDelay, .settle = kNoDelay});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("not confirmed"), std::string::npos) << result.error();
  EXPECT_EQ(driver.statusReads, 4);  // 1 initial + 3 retries
}

TEST(RunStoreParameters, FailsWhenStoreCommandWriteFails) {
  CommandFakeDriver driver;
  Device device = makeDevice(driver, kStoreParameters, 1);
  auto profile = createProfileDevice(device);
  ASSERT_TRUE(profile.has_value()) << profile.error();

  driver.failWrite = true;
  auto result =
      profile->runStoreParameters({.retries = 5, .interval = kNoDelay, .settle = kNoDelay});
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(driver.writes, 0);
  EXPECT_EQ(driver.statusReads, 0);  // no settle, no polls — fails on the command write
}

// --- restore default parameters (0x1011) ---------------------------------------------------------
//
// The retry/confirm/timeout/read-error/write-fail behaviour is the shared
// runSignatureConfirmCommand path already covered by the store tests above, so the restore tests
// focus on what is restore-specific: it writes the "load" signature and targets the sub-entry the
// group selects.

TEST(RunRestoreDefaultParameters, WritesLoadSignatureAndConfirms) {
  CommandFakeDriver driver;
  Device device = makeDevice(driver, kRestoreDefaultParameters, 1);  // 0x1011:01 = all
  auto profile = createProfileDevice(device);
  ASSERT_TRUE(profile.has_value()) << profile.error();

  auto result = profile->runRestoreDefaultParameters(
      RestoreGroup::kAll, {.retries = 5, .interval = kNoDelay, .settle = kNoDelay});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(driver.writes, 1);
  EXPECT_EQ(driver.lastSignature, kLoadSignature);
  EXPECT_EQ(driver.statusReads, 1);
}

TEST(RunRestoreDefaultParameters, TargetsTheSelectedGroupSubindex) {
  // Each group's confirm object sits at the matching 0x1011 sub-entry; a run that reaches it
  // (writes + confirms) proves the group→subindex mapping. A wrong subindex would leave the watched
  // object untouched and time out instead.
  struct Case {
    RestoreGroup group;
    uint8_t subindex;
  };
  for (const Case c : {Case{RestoreGroup::kAll, 1}, Case{RestoreGroup::kCommunication, 2},
                       Case{RestoreGroup::kApplication, 3}, Case{RestoreGroup::kManufacturer, 4}}) {
    CommandFakeDriver driver;
    Device device = makeDevice(driver, kRestoreDefaultParameters, c.subindex);
    auto profile = createProfileDevice(device);
    ASSERT_TRUE(profile.has_value()) << profile.error();

    auto result = profile->runRestoreDefaultParameters(
        c.group, {.retries = 5, .interval = kNoDelay, .settle = kNoDelay});
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(driver.writes, 1) << "group subindex " << static_cast<int>(c.subindex);
    EXPECT_EQ(driver.lastSignature, kLoadSignature);
  }
}

TEST(ParseRestoreGroup, MapsKnownTokensAndRejectsOthers) {
  EXPECT_EQ(parseRestoreGroup("all"), RestoreGroup::kAll);
  EXPECT_EQ(parseRestoreGroup("communication"), RestoreGroup::kCommunication);
  EXPECT_EQ(parseRestoreGroup("application"), RestoreGroup::kApplication);
  EXPECT_EQ(parseRestoreGroup("manufacturer"), RestoreGroup::kManufacturer);
  EXPECT_EQ(parseRestoreGroup(""), std::nullopt);
  EXPECT_EQ(parseRestoreGroup("factory"), std::nullopt);
  EXPECT_EQ(parseRestoreGroup("All"), std::nullopt);  // case-sensitive
}

}  // namespace
