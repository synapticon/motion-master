#include "node/profile_device.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <map>
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

constexpr uint16_t kPreOp = static_cast<uint16_t>(EtherCatState::PreOp);

// Zero settle + retry delay so the tests exercise the retry/confirm logic without real waiting.
constexpr auto kNoDelay = std::chrono::milliseconds(0);

// Generic device-profile object indices used by the tests.
constexpr uint16_t kDeviceType = 0x1000;
constexpr uint16_t kStoreParameters = 0x1010;

// ASCII "save" (little-endian) — the value 0x1010:01 accepts to trigger a store.
constexpr uint32_t kSaveSignature = 0x65766173;

std::vector<uint8_t> u32le(uint32_t v) {
  return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
          static_cast<uint8_t>(v >> 24)};
}

/// FieldbusDriver test double that models the generic "store parameters" object (0x1010:01): a
/// small SDO object store plus configurable behaviour for the store command and its confirmation
/// read-back, so the retry/confirm walk of ProfileDevice::runStoreParameters can be exercised
/// deterministically without real hardware.
class StoreFakeDriver : public FieldbusDriver {
 public:
  std::map<uint32_t, std::vector<uint8_t>> store;
  std::vector<OdEntry> ods;
  uint16_t alStatus = kPreOp;

  // --- store-parameters behaviour ------------------------------------------
  bool failStoreWrite = false;      ///< writeSdo(0x1010:01) returns an SDO-abort error.
  int storeWrites = 0;              ///< Count of signature writes to 0x1010:01.
  uint32_t lastStoreSignature = 0;  ///< Last value written to 0x1010:01.
  int storeStatusReads = 0;         ///< Count of reads of 0x1010:01.
  int readErrorsBeforeAnswer = 0;  ///< First N reads of 0x1010:01 fail (mailbox busy while saving).
  int pollsBeforeConfirm = 0;      ///< Successful reads that return 0 before 0x1010:01 reads 1.
  bool neverConfirm = false;       ///< When set, 0x1010:01 always reads 0 (store never completes).

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

  // Seeds the mandatory device type (0x1000) and the store-parameters sub-entry (0x1010:01).
  void programStoreObjects() {
    programObject(kDeviceType, 0, ObjectDataType::UNSIGNED32, u32le(0x00020192));  // 402 profile
    programObject(kStoreParameters, 1, ObjectDataType::UNSIGNED32, u32le(0));
  }

  std::expected<std::vector<uint8_t>, std::string> readSdo(uint16_t, uint16_t index,
                                                           uint8_t subindex) override {
    if (index == kStoreParameters && subindex == 1) {
      ++storeStatusReads;
      if (storeStatusReads <= readErrorsBeforeAnswer) {
        return std::unexpected("SDOread failed (no response — mailbox timeout)");
      }
      const bool confirmed =
          !neverConfirm && storeStatusReads > readErrorsBeforeAnswer + pollsBeforeConfirm;
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
    if (index == kStoreParameters && subindex == 1) {
      if (failStoreWrite) {
        return std::unexpected("SDO abort 0x08000000: General error");
      }
      ++storeWrites;
      if (data.size() >= 4) {
        lastStoreSignature =
            static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
            (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
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

// Builds a device with the store-parameters objects enumerated, online (PRE-OP) for SDO access.
Device makeStoreDevice(StoreFakeDriver& driver) {
  driver.programStoreObjects();
  Device device(1, driver);
  auto initialized = device.initializeParameters();
  EXPECT_TRUE(initialized.has_value()) << initialized.error();
  return device;
}

TEST(CreateProfileDevice, RejectsDeviceWithoutGenericArea) {
  StoreFakeDriver driver;
  Device device(1, driver);  // no parameters enumerated — 0x1000 absent
  auto profile = createProfileDevice(device);
  EXPECT_FALSE(profile.has_value());
}

TEST(CreateProfileDevice, BindsDeviceWithGenericArea) {
  StoreFakeDriver driver;
  Device device = makeStoreDevice(driver);
  auto profile = createProfileDevice(device);
  ASSERT_TRUE(profile.has_value()) << profile.error();
}

TEST(RunStoreParameters, WritesSaveSignatureAndConfirms) {
  StoreFakeDriver driver;
  Device device = makeStoreDevice(driver);
  auto profile = createProfileDevice(device);
  ASSERT_TRUE(profile.has_value()) << profile.error();

  // 0x1010:01 reads back 1 on the first poll → confirmed immediately.
  auto result =
      profile->runStoreParameters({.retries = 5, .interval = kNoDelay, .settle = kNoDelay});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(driver.storeWrites, 1);
  EXPECT_EQ(driver.lastStoreSignature, kSaveSignature);
  EXPECT_EQ(driver.storeStatusReads, 1);  // one confirming read, no retries
}

TEST(RunStoreParameters, RetriesUntilConfirmed) {
  StoreFakeDriver driver;
  Device device = makeStoreDevice(driver);
  auto profile = createProfileDevice(device);
  ASSERT_TRUE(profile.has_value()) << profile.error();

  driver.pollsBeforeConfirm = 2;  // first two polls read 0, the third reads 1
  auto result =
      profile->runStoreParameters({.retries = 5, .interval = kNoDelay, .settle = kNoDelay});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(driver.storeStatusReads, 3);
}

TEST(RunStoreParameters, RetriesThroughTransientReadErrors) {
  StoreFakeDriver driver;
  Device device = makeStoreDevice(driver);
  auto profile = createProfileDevice(device);
  ASSERT_TRUE(profile.has_value()) << profile.error();

  // The mailbox is unresponsive for the first two polls (store in progress), then answers 1 — a
  // read error must be retried like a value mismatch, not fail the whole call.
  driver.readErrorsBeforeAnswer = 2;
  auto result =
      profile->runStoreParameters({.retries = 5, .interval = kNoDelay, .settle = kNoDelay});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(driver.storeStatusReads, 3);
}

TEST(RunStoreParameters, TimesOutWhenNeverConfirmed) {
  StoreFakeDriver driver;
  Device device = makeStoreDevice(driver);
  auto profile = createProfileDevice(device);
  ASSERT_TRUE(profile.has_value()) << profile.error();

  driver.neverConfirm = true;
  auto result =
      profile->runStoreParameters({.retries = 3, .interval = kNoDelay, .settle = kNoDelay});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("not confirmed"), std::string::npos) << result.error();
  EXPECT_EQ(driver.storeStatusReads, 4);  // 1 initial + 3 retries
}

TEST(RunStoreParameters, FailsWhenStoreCommandWriteFails) {
  StoreFakeDriver driver;
  Device device = makeStoreDevice(driver);
  auto profile = createProfileDevice(device);
  ASSERT_TRUE(profile.has_value()) << profile.error();

  driver.failStoreWrite = true;
  auto result =
      profile->runStoreParameters({.retries = 5, .interval = kNoDelay, .settle = kNoDelay});
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(driver.storeWrites, 0);
  EXPECT_EQ(driver.storeStatusReads, 0);  // no settle, no polls — fails on the command write
}

}  // namespace
