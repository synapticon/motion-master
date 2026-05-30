#include "node/device.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "comm/fieldbus_driver.h"

namespace {

using mm::comm::EtherCatState;
using mm::comm::FieldbusDriver;
using mm::comm::OdEntry;
using mm::comm::SlaveInfo;
using mm::node::Device;
using mm::node::reconcileDetectedModules;

/// FieldbusDriver test double for SDO exchange: readSdo answers from a programmed
/// map keyed by (index, subindex); writeSdo records every download and can be told
/// to fail for specific (index, subindex) pairs. All other methods are stubs.
class SdoFakeDriver : public FieldbusDriver {
 public:
  struct Write {
    uint16_t index;
    uint8_t subindex;
    std::vector<uint8_t> data;
  };

  /// Programmed SDO upload responses, keyed by @c key(index, subindex).
  std::map<uint32_t, std::expected<std::vector<uint8_t>, std::string>> reads;
  /// Recorded SDO downloads, in call order.
  std::vector<Write> writes;
  /// (index, subindex) pairs whose download must fail.
  std::set<uint32_t> failWrites;

  static uint32_t key(uint16_t index, uint8_t subindex) {
    return (static_cast<uint32_t>(index) << 8) | subindex;
  }

  void programRead(uint16_t index, uint8_t subindex, std::vector<uint8_t> bytes) {
    reads[key(index, subindex)] = std::move(bytes);
  }

  std::expected<std::vector<uint8_t>, std::string> readSdo(uint16_t, uint16_t index,
                                                           uint8_t subindex) override {
    auto it = reads.find(key(index, subindex));
    if (it == reads.end()) {
      return std::unexpected("no such object");
    }
    return it->second;
  }

  std::expected<void, std::string> writeSdo(uint16_t, uint16_t index, uint8_t subindex,
                                            std::span<const uint8_t> data) override {
    writes.push_back({index, subindex, std::vector<uint8_t>(data.begin(), data.end())});
    if (failWrites.contains(key(index, subindex))) {
      return std::unexpected("simulated write failure");
    }
    return {};
  }

  // --- unused stubs ---------------------------------------------------------
  std::expected<void, std::string> init() override { return {}; }
  std::expected<int, std::string> scan() override { return 0; }
  SlaveInfo slaveInfo(uint16_t) const override { return {}; }
  void exchangeProcessData() override {}
  void stop() override {}

  std::expected<std::vector<SlaveStateRaw>, std::string> readStates(
      const std::vector<uint16_t>& positions) override {
    return std::vector<SlaveStateRaw>(positions.size(), SlaveStateRaw{});
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

  void transitionToState(const std::vector<uint16_t>&, std::optional<EtherCatState>, EtherCatState,
                         std::chrono::steady_clock::duration, std::chrono::steady_clock::duration,
                         std::function<void()>, std::function<bool()>) override {}
};

constexpr uint16_t kDetected = 0xF050;
constexpr uint16_t kConfigured = 0xF030;

// Little-endian 4-byte encoding of a 32-bit module ident.
std::vector<uint8_t> ident(uint32_t v) {
  return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
          static_cast<uint8_t>(v >> 24)};
}

TEST(ReconcileDetectedModules, NonModularDeviceIsNoOp) {
  // No 0xF050 list at all — readSdo returns an error for the count subindex.
  SdoFakeDriver driver;
  Device device(1, driver);

  auto result = reconcileDetectedModules(device);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 0);
  EXPECT_TRUE(driver.writes.empty());
}

TEST(ReconcileDetectedModules, WritesDetectedIdentIntoConfiguredList) {
  SdoFakeDriver driver;
  driver.programRead(kDetected, 0x00, {1});  // one slot (UNSIGNED8 count)
  driver.programRead(kDetected, 0x01, ident(0xAABBCCDD));
  driver.programRead(kConfigured, 0x01, ident(0x00000000));  // mismatch
  Device device(1, driver);

  auto result = reconcileDetectedModules(device);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 1);

  ASSERT_EQ(driver.writes.size(), 1u);
  EXPECT_EQ(driver.writes[0].index, kConfigured);
  EXPECT_EQ(driver.writes[0].subindex, 0x01);
  EXPECT_EQ(driver.writes[0].data, ident(0xAABBCCDD));
}

TEST(ReconcileDetectedModules, SkipsSlotsThatAlreadyMatch) {
  SdoFakeDriver driver;
  driver.programRead(kDetected, 0x00, {1});
  driver.programRead(kDetected, 0x01, ident(0x12345678));
  driver.programRead(kConfigured, 0x01, ident(0x12345678));  // already matches
  Device device(1, driver);

  auto result = reconcileDetectedModules(device);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 0);
  EXPECT_TRUE(driver.writes.empty());
}

TEST(ReconcileDetectedModules, SkipsEmptySlots) {
  SdoFakeDriver driver;
  driver.programRead(kDetected, 0x00, {1});
  driver.programRead(kDetected, 0x01, ident(0x00000000));  // empty slot
  Device device(1, driver);

  auto result = reconcileDetectedModules(device);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 0);
  EXPECT_TRUE(driver.writes.empty());
}

TEST(ReconcileDetectedModules, WritesWhenConfiguredEntryIsUnreadable) {
  // 0xF030:01 read fails (not programmed) — the configured value is unknown, so we
  // still write the detected ident rather than skipping.
  SdoFakeDriver driver;
  driver.programRead(kDetected, 0x00, {1});
  driver.programRead(kDetected, 0x01, ident(0xCAFEBABE));
  Device device(1, driver);

  auto result = reconcileDetectedModules(device);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 1);
  ASSERT_EQ(driver.writes.size(), 1u);
  EXPECT_EQ(driver.writes[0].data, ident(0xCAFEBABE));
}

TEST(ReconcileDetectedModules, HandlesMultipleSlotsAndU16Count) {
  // Count encoded as UNSIGNED16 (2 bytes). Slot 1 needs a write, slot 2 is empty,
  // slot 3 already matches — only slot 1 should be written.
  SdoFakeDriver driver;
  driver.programRead(kDetected, 0x00, {3, 0});  // three slots, little-endian u16
  driver.programRead(kDetected, 0x01, ident(0x11112222));
  driver.programRead(kConfigured, 0x01, ident(0x00000000));
  driver.programRead(kDetected, 0x02, ident(0x00000000));  // empty
  driver.programRead(kDetected, 0x03, ident(0x33334444));
  driver.programRead(kConfigured, 0x03, ident(0x33334444));  // matches
  Device device(1, driver);

  auto result = reconcileDetectedModules(device);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 1);
  ASSERT_EQ(driver.writes.size(), 1u);
  EXPECT_EQ(driver.writes[0].subindex, 0x01);
  EXPECT_EQ(driver.writes[0].data, ident(0x11112222));
}

TEST(ReconcileDetectedModules, SkipsSlotWithUnexpectedSize) {
  // A detected entry that is not 4 bytes is malformed and skipped, not written.
  SdoFakeDriver driver;
  driver.programRead(kDetected, 0x00, {1});
  driver.programRead(kDetected, 0x01, {0xDE, 0xAD, 0xBE});  // 3 bytes
  Device device(1, driver);

  auto result = reconcileDetectedModules(device);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 0);
  EXPECT_TRUE(driver.writes.empty());
}

TEST(ReconcileDetectedModules, ReportsWriteFailureNamingTheSlot) {
  SdoFakeDriver driver;
  driver.programRead(kDetected, 0x00, {1});
  driver.programRead(kDetected, 0x01, ident(0xAABBCCDD));
  driver.programRead(kConfigured, 0x01, ident(0x00000000));
  driver.failWrites.insert(SdoFakeDriver::key(kConfigured, 0x01));
  Device device(1, driver);

  auto result = reconcileDetectedModules(device);
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("slot 1"), std::string::npos);
}

}  // namespace
