#include "node/device.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "comm/fieldbus_driver.h"

namespace {

using mm::comm::EtherCatState;
using mm::comm::FieldbusDriver;
using mm::comm::OdEntry;
using mm::comm::SlaveInfo;
using mm::node::Device;
using mm::node::DeviceParameterValue;
using mm::node::reconcileDetectedModules;
using mm::node::SyncState;

// ETG.1020 UNSIGNED32 data type code, used by the parameter read/write tests.
constexpr uint16_t kU32 = 0x0007;
// PRE-OP AL state — used to bring the fake "online" (mailbox available) for SDO tests.
constexpr uint16_t kPreOp = static_cast<uint16_t>(EtherCatState::PreOp);

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
  /// Object dictionary entries returned by readObjectDictionary().
  std::vector<OdEntry> ods;
  /// Cached AL status returned by slaveState() (what mailboxActive()/exchangesProcessData read).
  uint16_t state = 0;

  static uint32_t key(uint16_t index, uint8_t subindex) {
    return (static_cast<uint32_t>(index) << 8) | subindex;
  }

  void programRead(uint16_t index, uint8_t subindex, std::vector<uint8_t> bytes) {
    reads[key(index, subindex)] = std::move(bytes);
  }

  void programOd(uint16_t index, uint8_t subindex, uint16_t dataType, uint16_t access = 0x3F) {
    OdEntry e{};
    e.index = index;
    e.subindex = subindex;
    e.dataType = dataType;
    e.access = access;
    ods.push_back(e);
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
  uint16_t slaveState(uint16_t) const override { return state; }
  std::expected<void, std::string> configureProcessData() override { return {}; }
  mm::comm::PdoLayout processDataLayout() override { return {}; }
  int exchangeProcessData(std::span<const uint8_t>, std::span<uint8_t>) override { return 0; }
  void stop() override {}

  std::expected<std::vector<SlaveStateRaw>, std::string> readStates(
      const std::vector<uint16_t>& positions) override {
    return std::vector<SlaveStateRaw>(positions.size(), SlaveStateRaw{});
  }

  std::expected<std::vector<OdEntry>, std::string> readObjectDictionary(uint16_t) override {
    return ods;
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

// Little-endian 4-byte encoding of a 32-bit value.
std::vector<uint8_t> u32le(uint32_t v) {
  return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
          static_cast<uint8_t>(v >> 24)};
}

// Builds a device with a single UNSIGNED32 parameter at 0x6065:00 (Unknown sync state).
Device deviceWithU32Param(SdoFakeDriver& driver) {
  driver.programOd(0x6065, 0x00, kU32);
  Device device(1, driver);
  EXPECT_TRUE(device.initializeParameters(/*readValues=*/false).has_value());
  return device;
}

TEST(DeviceReadParameter, OnlineUpdatesCacheAndMarksSynced) {
  SdoFakeDriver driver;
  Device device = deviceWithU32Param(driver);
  driver.state = kPreOp;
  driver.programRead(0x6065, 0x00, u32le(16));

  auto v = device.readParameter(0x6065, 0x00);
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(*v, DeviceParameterValue{uint32_t{16}});

  const auto* p = device.parameter(0x6065, 0x00);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->value, DeviceParameterValue{uint32_t{16}});
  EXPECT_EQ(p->syncState, SyncState::Synced);
}

TEST(DeviceReadParameter, OfflineServesCacheWithoutBusAccess) {
  SdoFakeDriver driver;
  Device device = deviceWithU32Param(driver);
  // Device stays offline (default). A device-side value of 16 is programmed but must
  // NOT be returned — the cached default (0) is served instead.
  driver.programRead(0x6065, 0x00, u32le(16));

  auto v = device.readParameter(0x6065, 0x00);
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(*v, DeviceParameterValue{uint32_t{0}});

  const auto* p = device.parameter(0x6065, 0x00);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->syncState, SyncState::Unknown);
}

TEST(DeviceReadParameter, UnknownParameterErrors) {
  SdoFakeDriver driver;
  Device device = deviceWithU32Param(driver);
  driver.state = kPreOp;
  EXPECT_FALSE(device.readParameter(0x1234, 0x00).has_value());
}

TEST(DeviceReadAllParameters, RefreshesEveryReadableValueInPlace) {
  SdoFakeDriver driver;
  driver.programOd(0x6065, 0x00, kU32);  // readable+writable (default access)
  driver.programOd(0x6066, 0x00, kU32);
  Device device(1, driver);
  ASSERT_TRUE(device.initializeParameters(/*readValues=*/false).has_value());
  driver.state = kPreOp;
  driver.programRead(0x6065, 0x00, u32le(16));
  driver.programRead(0x6066, 0x00, u32le(32));

  ASSERT_TRUE(device.readAllParameters().has_value());

  const auto* a = device.parameter(0x6065, 0x00);
  const auto* b = device.parameter(0x6066, 0x00);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(a->value, DeviceParameterValue{uint32_t{16}});
  EXPECT_EQ(a->syncState, SyncState::Synced);
  EXPECT_EQ(b->value, DeviceParameterValue{uint32_t{32}});
  EXPECT_EQ(b->syncState, SyncState::Synced);
}

TEST(DeviceReadAllParameters, SkipsWriteOnlyObjects) {
  SdoFakeDriver driver;
  driver.programOd(0x6040, 0x00, kU32, /*access=*/0x38);  // write-only: no read bits
  Device device(1, driver);
  ASSERT_TRUE(device.initializeParameters(/*readValues=*/false).has_value());
  driver.state = kPreOp;
  // An SDO upload would be answered, but the object is write-only — readAllParameters must not
  // attempt it. (No programRead, so any upload would also error; the skip keeps the call clean.)

  ASSERT_TRUE(device.readAllParameters().has_value());

  const auto* p = device.parameter(0x6040, 0x00);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->syncState, SyncState::Unknown);  // never read
}

TEST(DeviceReadAllParameters, SucceedsDespiteAPerEntryReadFailure) {
  SdoFakeDriver driver;
  driver.programOd(0x6065, 0x00, kU32);
  driver.programOd(0x6066, 0x00, kU32);
  Device device(1, driver);
  ASSERT_TRUE(device.initializeParameters(/*readValues=*/false).has_value());
  driver.state = kPreOp;
  driver.programRead(0x6065, 0x00, u32le(16));
  // 0x6066 has no programmed response → its upload errors. Best-effort: the sweep still succeeds
  // and the readable sibling is refreshed.

  ASSERT_TRUE(device.readAllParameters().has_value());

  const auto* a = device.parameter(0x6065, 0x00);
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->syncState, SyncState::Synced);
}

TEST(DeviceReadAllParameters, ErrorsWhenNoParametersLoaded) {
  SdoFakeDriver driver;
  Device device(1, driver);
  EXPECT_FALSE(device.readAllParameters().has_value());
}

TEST(DeviceParameterCopy, ReturnsFullStructFromCacheWithoutBusAccess) {
  SdoFakeDriver driver;
  Device device = deviceWithU32Param(driver);
  driver.state = kPreOp;
  driver.programRead(0x6065, 0x00, u32le(16));
  ASSERT_TRUE(device.readParameter(0x6065, 0x00).has_value());  // sync the cache to 16

  const size_t writesBefore = driver.writes.size();
  auto copy = device.parameterCopy(0x6065, 0x00);
  ASSERT_TRUE(copy.has_value());
  EXPECT_EQ(copy->index, 0x6065);
  EXPECT_EQ(copy->subindex, 0x00);
  EXPECT_EQ(copy->dataType, kU32);
  EXPECT_EQ(copy->value, DeviceParameterValue{uint32_t{16}});
  EXPECT_EQ(copy->syncState, SyncState::Synced);
  EXPECT_EQ(driver.writes.size(), writesBefore);  // a copy never touches the bus
}

TEST(DeviceParameterCopy, UnknownParameterReturnsNullopt) {
  SdoFakeDriver driver;
  Device device = deviceWithU32Param(driver);
  EXPECT_FALSE(device.parameterCopy(0x1234, 0x00).has_value());
}

TEST(DeviceWriteParameter, OnlineDownloadsAndMarksSynced) {
  SdoFakeDriver driver;
  Device device = deviceWithU32Param(driver);
  driver.state = kPreOp;

  auto w = device.writeParameter(0x6065, 0x00, DeviceParameterValue{uint32_t{123}});
  ASSERT_TRUE(w.has_value());

  ASSERT_EQ(driver.writes.size(), 1u);
  EXPECT_EQ(driver.writes[0].index, 0x6065);
  EXPECT_EQ(driver.writes[0].subindex, 0x00);
  EXPECT_EQ(driver.writes[0].data, u32le(123));

  const auto* p = device.parameter(0x6065, 0x00);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->value, DeviceParameterValue{uint32_t{123}});
  EXPECT_EQ(p->syncState, SyncState::Synced);
}

TEST(DeviceWriteParameter, OfflineCachesAsPendingAndSucceeds) {
  SdoFakeDriver driver;
  Device device = deviceWithU32Param(driver);
  // Offline (default): the write succeeds, updates the cache, and touches no hardware.

  auto w = device.writeParameter(0x6065, 0x00, DeviceParameterValue{uint32_t{55}});
  ASSERT_TRUE(w.has_value());
  EXPECT_TRUE(driver.writes.empty());

  const auto* p = device.parameter(0x6065, 0x00);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->value, DeviceParameterValue{uint32_t{55}});
  EXPECT_EQ(p->syncState, SyncState::Pending);
}

TEST(DeviceWriteParameter, OnlineDownloadFailureMarksPendingAndReturnsError) {
  SdoFakeDriver driver;
  Device device = deviceWithU32Param(driver);
  driver.state = kPreOp;
  driver.failWrites.insert(SdoFakeDriver::key(0x6065, 0x00));

  auto w = device.writeParameter(0x6065, 0x00, DeviceParameterValue{uint32_t{77}});
  EXPECT_FALSE(w.has_value());

  const auto* p = device.parameter(0x6065, 0x00);
  ASSERT_NE(p, nullptr);
  // Cache still reflects the attempted value; flagged Pending for a later re-write.
  EXPECT_EQ(p->value, DeviceParameterValue{uint32_t{77}});
  EXPECT_EQ(p->syncState, SyncState::Pending);
}

TEST(DeviceWriteParameter, CoercesValueIntoDeclaredType) {
  SdoFakeDriver driver;
  Device device = deviceWithU32Param(driver);
  driver.state = kPreOp;

  // Plain int literal — must be coerced to UNSIGNED32 and encoded as 4 bytes.
  auto w = device.writeParameter(0x6065, 0x00, DeviceParameterValue{9});
  ASSERT_TRUE(w.has_value());
  ASSERT_EQ(driver.writes.size(), 1u);
  EXPECT_EQ(driver.writes[0].data, u32le(9));

  const auto* p = device.parameter(0x6065, 0x00);
  ASSERT_NE(p, nullptr);
  EXPECT_TRUE(std::holds_alternative<uint32_t>(p->value));
}

TEST(DeviceWriteParameter, UnknownParameterErrors) {
  SdoFakeDriver driver;
  Device device = deviceWithU32Param(driver);
  driver.state = kPreOp;
  EXPECT_FALSE(device.writeParameter(0x1234, 0x00, DeviceParameterValue{uint32_t{1}}).has_value());
  EXPECT_TRUE(driver.writes.empty());
}

TEST(DeviceTypedHelpers, WriteValueAndReadValueRoundTrip) {
  SdoFakeDriver driver;
  Device device = deviceWithU32Param(driver);
  driver.state = kPreOp;

  // Terse write: a bare int literal, coerced to the declared UNSIGNED32 and encoded.
  ASSERT_TRUE(device.writeValue(0x6065, 0x00, 42).has_value());
  ASSERT_EQ(driver.writes.size(), 1u);
  EXPECT_EQ(driver.writes[0].data, u32le(42));

  // Terse typed read (device echoes 42 back).
  driver.programRead(0x6065, 0x00, u32le(42));
  auto v = device.readValue<uint32_t>(0x6065, 0x00);
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(*v, 42u);

  // A type-mismatched read is rejected.
  EXPECT_FALSE(device.readValue<int32_t>(0x6065, 0x00).has_value());
}

// --- readPdoMappings ---------------------------------------------------------

std::vector<uint8_t> u8le(uint8_t v) { return {v}; }
std::vector<uint8_t> u16le(uint16_t v) {
  return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
}
// A packed PDO mapping entry: index in bits 31..16, subindex 15..8, bit length 7..0.
std::vector<uint8_t> pdoEntry(uint16_t index, uint8_t subindex, uint8_t bits) {
  return u32le((static_cast<uint32_t>(index) << 16) | (static_cast<uint32_t>(subindex) << 8) |
               bits);
}

// Programs a CiA402-style mapping: one RxPDO (controlword + modes + target position) and one
// TxPDO (statusword + actual position + an alignment gap).
void programCia402Mapping(SdoFakeDriver& driver) {
  // 0x1C12 → 0x1600 : outputs (RxPDO)
  driver.programRead(0x1C12, 0x00, u8le(1));
  driver.programRead(0x1C12, 0x01, u16le(0x1600));
  driver.programRead(0x1600, 0x00, u8le(3));
  driver.programRead(0x1600, 0x01, pdoEntry(0x6040, 0x00, 16));  // controlword
  driver.programRead(0x1600, 0x02, pdoEntry(0x6060, 0x00, 8));   // modes of operation
  driver.programRead(0x1600, 0x03, pdoEntry(0x607A, 0x00, 32));  // target position
  // 0x1C13 → 0x1A00 : inputs (TxPDO)
  driver.programRead(0x1C13, 0x00, u8le(1));
  driver.programRead(0x1C13, 0x01, u16le(0x1A00));
  driver.programRead(0x1A00, 0x00, u8le(3));
  driver.programRead(0x1A00, 0x01, pdoEntry(0x6041, 0x00, 16));  // statusword
  driver.programRead(0x1A00, 0x02, pdoEntry(0x6064, 0x00, 32));  // position actual value
  driver.programRead(0x1A00, 0x03, pdoEntry(0x0000, 0x00, 8));   // alignment gap
}

TEST(DeviceReadPdoMappings, BuildsEntriesWithAccumulatedBitOffsets) {
  SdoFakeDriver driver;
  programCia402Mapping(driver);
  Device device(1, driver);

  ASSERT_TRUE(device.readPdoMappings().has_value());
  const auto& m = device.pdoMappings();

  ASSERT_EQ(m.outputs.size(), 3u);
  EXPECT_EQ(m.outputs[0].index, 0x6040);
  EXPECT_EQ(m.outputs[0].bitLength, 16u);
  EXPECT_EQ(m.outputs[0].bitOffset, 0u);
  EXPECT_EQ(m.outputs[1].index, 0x6060);
  EXPECT_EQ(m.outputs[1].bitOffset, 16u);
  EXPECT_EQ(m.outputs[2].index, 0x607A);
  EXPECT_EQ(m.outputs[2].bitOffset, 24u);
  EXPECT_EQ(m.outputBits, 56u);

  ASSERT_EQ(m.inputs.size(), 3u);
  EXPECT_EQ(m.inputs[0].index, 0x6041);
  EXPECT_EQ(m.inputs[0].bitOffset, 0u);
  EXPECT_EQ(m.inputs[1].index, 0x6064);
  EXPECT_EQ(m.inputs[1].bitOffset, 16u);
  // Alignment gap is kept (index 0) so the offset math stays self-contained.
  EXPECT_EQ(m.inputs[2].index, 0x0000);
  EXPECT_EQ(m.inputs[2].bitLength, 8u);
  EXPECT_EQ(m.inputs[2].bitOffset, 48u);
  EXPECT_EQ(m.inputBits, 56u);
}

TEST(DeviceReadPdoMappings, AbsentAssignmentObjectYieldsEmptyMapping) {
  SdoFakeDriver driver;  // nothing programmed — 0x1C12/0x1C13 reads fail
  Device device(1, driver);

  ASSERT_TRUE(device.readPdoMappings().has_value());
  EXPECT_TRUE(device.pdoMappings().outputs.empty());
  EXPECT_TRUE(device.pdoMappings().inputs.empty());
  EXPECT_EQ(device.pdoMappings().outputBits, 0u);
  EXPECT_EQ(device.pdoMappings().inputBits, 0u);
}

TEST(DeviceReadPdoMappings, UnreadableMappingObjectIsAnError) {
  SdoFakeDriver driver;
  // Assignment points at 0x1600 but its content is never programmed.
  driver.programRead(0x1C12, 0x00, u8le(1));
  driver.programRead(0x1C12, 0x01, u16le(0x1600));
  Device device(1, driver);

  auto result = device.readPdoMappings();
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("1600"), std::string::npos);
}

TEST(DeviceReadPdoMappings, MaxAssignmentCountTerminates) {
  // Regression: subindex 0 reporting 255 (the uint8_t max) must not wrap the loop counter.
  // With a `uint8_t i` the guard `i <= 255` is permanently true and i wraps 255->0; the
  // widened counter iterates exactly 255 (here all-unused) slots and then terminates.
  SdoFakeDriver driver;
  driver.programRead(0x1C12, 0x00, u8le(255));
  for (unsigned s = 1; s <= 255; ++s) {
    driver.programRead(0x1C12, static_cast<uint8_t>(s), u16le(0));  // unused assignment slot
  }
  Device device(1, driver);

  auto result = device.readPdoMappings();
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(device.pdoMappings().outputs.empty());
}

// --- cache thread-safety -----------------------------------------------------

// Hammers one parameter's cache from several threads at once: SDO refreshes
// (readParameter, online), raw-byte decodes (setValueFromBytes, the process-image path), and
// ordered snapshots (parametersOrdered). Each operation must stay internally consistent and
// the final cached value must be one a writer stored — never torn. Most valuable under a
// thread sanitizer, but also a plain deadlock/corruption smoke test.
TEST(DeviceCacheConcurrency, ConcurrentReadsAndCacheUpdatesAreSafe) {
  SdoFakeDriver driver;
  Device device = deviceWithU32Param(driver);
  driver.state = kPreOp;                        // online, so readParameter uploads
  driver.programRead(0x6065, 0x00, u32le(16));  // value an SDO refresh caches

  constexpr int kThreads = 4;
  constexpr int kIters = 2000;
  std::atomic<bool> go{false};
  std::atomic<int> failures{0};
  std::vector<std::thread> workers;

  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&] {
      while (!go.load()) {
        std::this_thread::yield();
      }
      for (int i = 0; i < kIters; ++i) {
        auto v = device.readParameter(0x6065, 0x00);
        if (!v || *v != DeviceParameterValue{uint32_t{16}}) {
          failures.fetch_add(1);
        }
      }
    });
  }
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&] {
      const auto bytes = u32le(99);
      while (!go.load()) {
        std::this_thread::yield();
      }
      for (int i = 0; i < kIters; ++i) {
        auto v = device.setValueFromBytes(0x6065, 0x00, bytes);
        if (!v || *v != DeviceParameterValue{uint32_t{99}}) {
          failures.fetch_add(1);
        }
      }
    });
  }
  for (int t = 0; t < 2; ++t) {
    workers.emplace_back([&] {
      while (!go.load()) {
        std::this_thread::yield();
      }
      for (int i = 0; i < kIters; ++i) {
        if (device.parametersOrdered().size() != 1) {
          failures.fetch_add(1);
        }
      }
    });
  }

  go.store(true);
  for (auto& w : workers) {
    w.join();
  }

  EXPECT_EQ(failures.load(), 0);
  const auto* p = device.parameter(0x6065, 0x00);
  ASSERT_NE(p, nullptr);
  EXPECT_TRUE(p->value == DeviceParameterValue{uint32_t{16}} ||
              p->value == DeviceParameterValue{uint32_t{99}});
}

}  // namespace
