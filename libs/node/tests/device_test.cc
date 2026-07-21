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
#include "node/synapticon.h"

namespace {

using mm::comm::EtherCatState;
using mm::comm::FieldbusDriver;
using mm::comm::OdEntry;
using mm::comm::SlaveInfo;
using mm::node::Device;
using mm::node::DeviceParameter;
using mm::node::DeviceParameterValue;
using mm::node::packMappingEntry;
using mm::node::ParameterOrigin;
using mm::node::PdoMapping;
using mm::node::PdoMappingEntry;
using mm::node::PdoMappingObject;
using mm::node::reconcileDetectedModules;
using mm::node::SyncState;
using mm::node::unpackMappingEntry;

// ETG.1020 data type codes used by the parameter read/write tests.
constexpr uint16_t kU8 = 0x0005;
constexpr uint16_t kU32 = 0x0007;
// ETG.1000-6 object codes (only ARRAY/RECORD are Complete-Access candidates).
constexpr uint16_t kArray = 0x0008;
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
  /// Programmed Complete Access blobs, keyed by object index. An unprogrammed index reports
  /// complete access unsupported (an SDO abort in reality).
  std::map<uint16_t, std::expected<std::vector<uint8_t>, std::string>> completeReads;
  /// Object indices passed to readSdoComplete(), in call order.
  std::vector<uint16_t> completeReadIndices;
  /// Count of per-subindex readSdo() uploads, to assert Complete Access replaced them.
  int perSubReads = 0;
  /// Cached AL status returned by slaveState() (what mailboxActive()/exchangesProcessData read).
  uint16_t state = 0;
  /// Mailbox-protocol bits returned by mailboxProtocols() (drives supportsCoe()). Defaults to CoE
  /// so the CoE-path tests need no setup; a no-CoE slave test clears it to exercise the SII path.
  uint16_t protocols = mm::comm::MailboxConfig::kProtocolCoe;
  /// Raw SII image returned by readSii() (empty → error, as for a slave with no readable SII).
  std::vector<uint8_t> sii;

  static uint32_t key(uint16_t index, uint8_t subindex) {
    return (static_cast<uint32_t>(index) << 8) | subindex;
  }

  void programRead(uint16_t index, uint8_t subindex, std::vector<uint8_t> bytes) {
    reads[key(index, subindex)] = std::move(bytes);
  }

  void programOd(uint16_t index, uint8_t subindex, uint16_t dataType, uint16_t access = 0x3F,
                 uint16_t bitLength = 0, uint16_t objectCode = 0) {
    OdEntry e{};
    e.index = index;
    e.subindex = subindex;
    e.dataType = dataType;
    e.access = access;
    e.bitLength = bitLength;
    e.objectCode = objectCode;
    ods.push_back(e);
  }

  std::expected<std::vector<uint8_t>, std::string> readSdo(uint16_t, uint16_t index,
                                                           uint8_t subindex) override {
    ++perSubReads;
    auto it = reads.find(key(index, subindex));
    if (it == reads.end()) {
      return std::unexpected("no such object");
    }
    return it->second;
  }

  std::expected<std::vector<uint8_t>, std::string> readSdoComplete(uint16_t,
                                                                   uint16_t index) override {
    completeReadIndices.push_back(index);
    auto it = completeReads.find(index);
    if (it == completeReads.end()) {
      return std::unexpected("complete access not supported");
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
  SlaveInfo info;  // identity returned to the Device at construction
  SlaveInfo slaveInfo(uint16_t) const override { return info; }
  uint16_t slaveState(uint16_t) const override { return state; }
  uint16_t mailboxProtocols(uint16_t) const override { return protocols; }
  std::expected<std::vector<uint8_t>, std::string> readSii(uint16_t) override {
    if (sii.empty()) {
      return std::unexpected("no SII programmed");
    }
    return sii;
  }
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

// Builds the wire layout of a Complete Access upload: subindex 0 as a padded 16-bit value
// (count byte + alignment pad) followed by the subindex payloads concatenated.
std::vector<uint8_t> completeBlob(uint8_t count, std::vector<std::vector<uint8_t>> subs) {
  std::vector<uint8_t> blob{count, 0};
  for (const auto& s : subs) {
    blob.insert(blob.end(), s.begin(), s.end());
  }
  return blob;
}

TEST(DeviceInitParametersCompleteAccess, DecodesMultiSubObjectInOneUpload) {
  SdoFakeDriver driver;
  driver.programOd(0x1600, 0x00, kU8, 0x3F, 8, kArray);
  driver.programOd(0x1600, 0x01, kU32, 0x3F, 32, kArray);
  driver.programOd(0x1600, 0x02, kU32, 0x3F, 32, kArray);
  driver.state = kPreOp;
  driver.completeReads[0x1600] = completeBlob(2, {u32le(0x11111111), u32le(0x22222222)});
  Device device(1, driver);

  ASSERT_TRUE(
      device.initializeParameters(/*readValues=*/true, /*useCompleteAccess=*/true).has_value());

  // One Complete Access read replaced the three per-subindex uploads.
  EXPECT_EQ(driver.completeReadIndices, (std::vector<uint16_t>{0x1600}));
  EXPECT_EQ(driver.perSubReads, 0);
  EXPECT_EQ(device.parameter(0x1600, 0x00)->value, DeviceParameterValue{uint8_t{2}});
  EXPECT_EQ(device.parameter(0x1600, 0x01)->value, DeviceParameterValue{uint32_t{0x11111111}});
  EXPECT_EQ(device.parameter(0x1600, 0x02)->value, DeviceParameterValue{uint32_t{0x22222222}});
  EXPECT_EQ(device.parameter(0x1600, 0x02)->syncState, SyncState::Synced);
}

TEST(DeviceInitParametersCompleteAccess, FallsBackToPerSubindexWhenUnsupported) {
  SdoFakeDriver driver;
  driver.programOd(0x1600, 0x00, kU8, 0x3F, 8, kArray);
  driver.programOd(0x1600, 0x01, kU32, 0x3F, 32, kArray);
  driver.state = kPreOp;
  // completeReads left empty → the first (probe) CA read is rejected, disabling CA for the pass.
  driver.programRead(0x1600, 0x00, {2});
  driver.programRead(0x1600, 0x01, u32le(0x33333333));
  Device device(1, driver);

  ASSERT_TRUE(
      device.initializeParameters(/*readValues=*/true, /*useCompleteAccess=*/true).has_value());

  EXPECT_EQ(driver.completeReadIndices, (std::vector<uint16_t>{0x1600}));  // probed once
  EXPECT_GT(driver.perSubReads, 0);                                        // then fell back
  EXPECT_EQ(device.parameter(0x1600, 0x01)->value, DeviceParameterValue{uint32_t{0x33333333}});
  EXPECT_EQ(device.parameter(0x1600, 0x01)->syncState, SyncState::Synced);
}

TEST(DeviceInitParametersCompleteAccess, DisabledFlagForcesPerSubindexReads) {
  SdoFakeDriver driver;
  driver.programOd(0x1600, 0x00, kU8, 0x3F, 8, kArray);
  driver.programOd(0x1600, 0x01, kU32, 0x3F, 32, kArray);
  driver.state = kPreOp;
  driver.completeReads[0x1600] = completeBlob(1, {u32le(0x55555555)});  // would work if attempted
  driver.programRead(0x1600, 0x00, {1});
  driver.programRead(0x1600, 0x01, u32le(0x44444444));
  Device device(1, driver);

  ASSERT_TRUE(
      device.initializeParameters(/*readValues=*/true, /*useCompleteAccess=*/false).has_value());

  EXPECT_TRUE(driver.completeReadIndices.empty());  // CA never attempted
  EXPECT_GT(driver.perSubReads, 0);
  EXPECT_EQ(device.parameter(0x1600, 0x01)->value, DeviceParameterValue{uint32_t{0x44444444}});
}

TEST(DeviceInitParametersCompleteAccess, SingleSubindexVarSkipsCompleteAccess) {
  SdoFakeDriver driver;
  driver.programOd(0x6065, 0x00, kU32, 0x3F, 32, /*objectCode=*/0x0007);  // VAR
  driver.state = kPreOp;
  driver.programRead(0x6065, 0x00, u32le(16));
  Device device(1, driver);

  ASSERT_TRUE(
      device.initializeParameters(/*readValues=*/true, /*useCompleteAccess=*/true).has_value());

  EXPECT_TRUE(driver.completeReadIndices.empty());  // a lone subindex gains nothing from CA
  EXPECT_EQ(device.parameter(0x6065, 0x00)->value, DeviceParameterValue{uint32_t{16}});
}

TEST(DeviceReadAllParameters, UsesCompleteAccessForMultiSubObjects) {
  SdoFakeDriver driver;
  driver.programOd(0x1600, 0x00, kU8, 0x3F, 8, kArray);
  driver.programOd(0x1600, 0x01, kU32, 0x3F, 32, kArray);
  driver.programOd(0x1600, 0x02, kU32, 0x3F, 32, kArray);
  driver.programOd(0x6060, 0x00, kU32, 0x3F, 32, /*objectCode=*/0x0007);  // a VAR
  Device device(1, driver);
  ASSERT_TRUE(device.initializeParameters(/*readValues=*/false).has_value());
  driver.state = kPreOp;  // mailbox active, not exchanging → SDO/CA path
  driver.completeReads[0x1600] = completeBlob(2, {u32le(0xAAAA0001), u32le(0xAAAA0002)});
  driver.programRead(0x6060, 0x00, u32le(7));
  driver.perSubReads = 0;

  ASSERT_TRUE(device.readAllParameters(/*useCompleteAccess=*/true).has_value());

  // The record was read with one CA upload; only the VAR used a per-subindex read.
  EXPECT_EQ(driver.completeReadIndices, (std::vector<uint16_t>{0x1600}));
  EXPECT_EQ(driver.perSubReads, 1);
  EXPECT_EQ(device.parameter(0x1600, 0x01)->value, DeviceParameterValue{uint32_t{0xAAAA0001}});
  EXPECT_EQ(device.parameter(0x1600, 0x02)->value, DeviceParameterValue{uint32_t{0xAAAA0002}});
  EXPECT_EQ(device.parameter(0x6060, 0x00)->value, DeviceParameterValue{uint32_t{7}});
}

TEST(DeviceReadAllParameters, FallsBackToPerSubindexWhenCompleteAccessUnsupported) {
  SdoFakeDriver driver;
  driver.programOd(0x1600, 0x00, kU8, 0x3F, 8, kArray);
  driver.programOd(0x1600, 0x01, kU32, 0x3F, 32, kArray);
  Device device(1, driver);
  ASSERT_TRUE(device.initializeParameters(/*readValues=*/false).has_value());
  driver.state = kPreOp;
  // completeReads left empty → the probe CA read is rejected, disabling CA for the sweep.
  driver.programRead(0x1600, 0x00, {2});
  driver.programRead(0x1600, 0x01, u32le(0x1234));

  ASSERT_TRUE(device.readAllParameters(/*useCompleteAccess=*/true).has_value());

  EXPECT_EQ(driver.completeReadIndices, (std::vector<uint16_t>{0x1600}));  // probed once
  EXPECT_EQ(device.parameter(0x1600, 0x01)->value, DeviceParameterValue{uint32_t{0x1234}});
  EXPECT_EQ(device.parameter(0x1600, 0x01)->syncState, SyncState::Synced);
}

TEST(DeviceReadAllParameters, DisabledFlagForcesPerSubindexReads) {
  SdoFakeDriver driver;
  driver.programOd(0x1600, 0x00, kU8, 0x3F, 8, kArray);
  driver.programOd(0x1600, 0x01, kU32, 0x3F, 32, kArray);
  Device device(1, driver);
  ASSERT_TRUE(device.initializeParameters(/*readValues=*/false).has_value());
  driver.state = kPreOp;
  driver.completeReads[0x1600] = completeBlob(1, {u32le(0x9999)});  // would work if attempted
  driver.programRead(0x1600, 0x00, {1});
  driver.programRead(0x1600, 0x01, u32le(0x4444));

  ASSERT_TRUE(device.readAllParameters(/*useCompleteAccess=*/false).has_value());

  EXPECT_TRUE(driver.completeReadIndices.empty());  // CA never attempted
  EXPECT_EQ(device.parameter(0x1600, 0x01)->value, DeviceParameterValue{uint32_t{0x4444}});
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

// --- readFlatPdoMapping ---------------------------------------------------------

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

// A single-entry SII PDO record: the 8-byte PDO header (index, nEntry, syncM, sync, nameIdx,
// flags) followed by one 8-byte entry (index, subindex, entryNameIdx, dataType, bitLen, flags).
// This is the shape a simple I/O terminal uses — an EL2008 gives each channel its own 1-bit PDO.
std::vector<uint8_t> siiPdo1(uint16_t pdoIndex, uint16_t entryIndex, uint8_t subindex, uint8_t bits,
                             uint8_t dataType = 0) {
  std::vector<uint8_t> b;
  const auto push16 = [&](uint16_t v) {
    b.push_back(static_cast<uint8_t>(v));
    b.push_back(static_cast<uint8_t>(v >> 8));
  };
  push16(pdoIndex);
  b.push_back(1);  // nEntry
  b.push_back(0);  // syncM — irrelevant here: direction is split by category, not SM index
  b.push_back(0);  // synchronization
  b.push_back(0);  // nameIdx
  push16(0);       // flags
  push16(entryIndex);
  b.push_back(subindex);
  b.push_back(0);         // entryNameIdx
  b.push_back(dataType);  // dataType (ETG.1020 code)
  b.push_back(bits);      // bitLen
  push16(0);              // flags
  return b;
}

// Assembles a raw SII image: a zeroed 128-byte fixed header (content irrelevant to PDO decode),
// then each (categoryType, payload) record as [type:u16][wordSize:u16][payload], then the End
// (0xFFFF) marker. Category 51 = RxPDO (outputs), 50 = TxPDO (inputs).
std::vector<uint8_t> buildSii(
    const std::vector<std::pair<uint16_t, std::vector<uint8_t>>>& categories) {
  std::vector<uint8_t> img(128, 0);
  const auto push16 = [&](uint16_t v) {
    img.push_back(static_cast<uint8_t>(v));
    img.push_back(static_cast<uint8_t>(v >> 8));
  };
  for (const auto& [type, payload] : categories) {
    push16(type);
    push16(static_cast<uint16_t>(payload.size() / 2));  // size in words
    img.insert(img.end(), payload.begin(), payload.end());
  }
  push16(0xFFFF);
  return img;
}

TEST(DeviceReadFlatPdoMapping, BuildsEntriesWithAccumulatedBitOffsets) {
  SdoFakeDriver driver;
  programCia402Mapping(driver);
  Device device(1, driver);

  ASSERT_TRUE(device.readFlatPdoMapping().has_value());
  const auto& m = device.flatPdoMapping();

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

TEST(DeviceReadFlatPdoMapping, AbsentAssignmentObjectYieldsEmptyMapping) {
  SdoFakeDriver driver;  // nothing programmed — 0x1C12/0x1C13 reads fail
  Device device(1, driver);

  ASSERT_TRUE(device.readFlatPdoMapping().has_value());
  EXPECT_TRUE(device.flatPdoMapping().outputs.empty());
  EXPECT_TRUE(device.flatPdoMapping().inputs.empty());
  EXPECT_EQ(device.flatPdoMapping().outputBits, 0u);
  EXPECT_EQ(device.flatPdoMapping().inputBits, 0u);
}

TEST(DeviceReadFlatPdoMapping, UnreadableMappingObjectIsAnError) {
  SdoFakeDriver driver;
  // Assignment points at 0x1600 but its content is never programmed.
  driver.programRead(0x1C12, 0x00, u8le(1));
  driver.programRead(0x1C12, 0x01, u16le(0x1600));
  Device device(1, driver);

  auto result = device.readFlatPdoMapping();
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("1600"), std::string::npos);
}

TEST(DeviceReadFlatPdoMapping, TrailingAlignmentPaddingPdoIsSkipped) {
  // A TwinCAT-style alignment-padding PDO (0x1701) is assigned in the last slot but is not
  // implemented in the device's CoE dictionary, so uploading its subindex 0 aborts with
  // "object does not exist" (0x06020000). readFlatPdoMapping must treat it as padding and skip it,
  // not fail the whole map — the real 0x1600 entries must come through unchanged.
  SdoFakeDriver driver;
  programCia402Mapping(driver);
  // Re-point 0x1C12 to assign two RxPDOs: the real 0x1600 plus a trailing padding 0x1701.
  driver.programRead(0x1C12, 0x00, u8le(2));
  driver.programRead(0x1C12, 0x02, u16le(0x1701));
  // 0x1701 aborts exactly as SoemFieldbusDriver::readSdo formats an SDO abort.
  driver.reads[SdoFakeDriver::key(0x1701, 0x00)] =
      std::unexpected("SDOread slave 1 0x1701:00 failed (SDO abort 0x06020000)");
  Device device(1, driver);

  ASSERT_TRUE(device.readFlatPdoMapping().has_value());
  const auto& m = device.flatPdoMapping();
  ASSERT_EQ(m.outputs.size(), 3u);  // only 0x1600's entries; the pad contributes none
  EXPECT_EQ(m.outputs[0].index, 0x6040);
  EXPECT_EQ(m.outputs[2].index, 0x607A);
  EXPECT_EQ(m.outputBits, 56u);
}

TEST(DeviceReadFlatPdoMapping, MaxAssignmentCountTerminates) {
  // Regression: subindex 0 reporting 255 (the uint8_t max) must not wrap the loop counter.
  // With a `uint8_t i` the guard `i <= 255` is permanently true and i wraps 255->0; the
  // widened counter iterates exactly 255 (here all-unused) slots and then terminates.
  SdoFakeDriver driver;
  driver.programRead(0x1C12, 0x00, u8le(255));
  for (unsigned s = 1; s <= 255; ++s) {
    driver.programRead(0x1C12, static_cast<uint8_t>(s), u16le(0));  // unused assignment slot
  }
  Device device(1, driver);

  auto result = device.readFlatPdoMapping();
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(device.flatPdoMapping().outputs.empty());
}

// --- readFlatPdoMapping: SII fallback for mailbox-less slaves ----------------

TEST(DeviceReadFlatPdoMapping, NoCoeSlaveReadsOutputsFromSii) {
  // An EL2008-style digital-output terminal: no CoE mailbox, eight 1-bit RxPDOs (0x1600..0x1607
  // → 0x7000:01..0x7070:01) fixed in SII. supportsCoe() is false, so the whole mapping is read
  // from the SII EEPROM, and the eight concatenated single-entry PDOs flatten to eight 1-bit
  // outputs at consecutive offsets — 8 bits = the 1 output byte the driver's window reserves.
  SdoFakeDriver driver;
  driver.protocols = 0;  // no mailbox → no CoE
  std::vector<uint8_t> rxCat;
  for (uint16_t ch = 0; ch < 8; ++ch) {
    const auto pdo = siiPdo1(static_cast<uint16_t>(0x1600 + ch),
                             static_cast<uint16_t>(0x7000 + ch * 0x10), 0x01, 1);
    rxCat.insert(rxCat.end(), pdo.begin(), pdo.end());
  }
  driver.sii = buildSii({{51, rxCat}});  // category 51 = RxPDO (outputs)
  Device device(1, driver);

  ASSERT_TRUE(device.readFlatPdoMapping().has_value());
  const auto& m = device.flatPdoMapping();
  ASSERT_EQ(m.outputs.size(), 8u);
  EXPECT_EQ(m.outputs[0].index, 0x7000);
  EXPECT_EQ(m.outputs[0].subindex, 0x01);
  EXPECT_EQ(m.outputs[0].bitLength, 1u);
  EXPECT_EQ(m.outputs[0].bitOffset, 0u);
  EXPECT_EQ(m.outputs[7].index, 0x7070);
  EXPECT_EQ(m.outputs[7].bitOffset, 7u);
  EXPECT_EQ(m.outputBits, 8u);
  EXPECT_TRUE(m.inputs.empty());
}

TEST(DeviceReadFlatPdoMapping, SiiTxPdoCategoryMapsToInputs) {
  // Direction is split by category, independent of the SM index: category 51 → outputs,
  // category 50 → inputs, each accumulating its own bit offsets.
  SdoFakeDriver driver;
  driver.protocols = 0;
  driver.sii = buildSii({{51, siiPdo1(0x1600, 0x7000, 0x01, 8)},    // 8-bit output
                         {50, siiPdo1(0x1A00, 0x6000, 0x01, 8)}});  // 8-bit input
  Device device(1, driver);

  ASSERT_TRUE(device.readFlatPdoMapping().has_value());
  const auto& m = device.flatPdoMapping();
  ASSERT_EQ(m.outputs.size(), 1u);
  EXPECT_EQ(m.outputs[0].index, 0x7000);
  EXPECT_EQ(m.outputBits, 8u);
  ASSERT_EQ(m.inputs.size(), 1u);
  EXPECT_EQ(m.inputs[0].index, 0x6000);
  EXPECT_EQ(m.inputs[0].bitOffset, 0u);
  EXPECT_EQ(m.inputBits, 8u);
}

TEST(DeviceReadFlatPdoMapping, NoCoeSlaveWithoutPdoCategoriesYieldsEmptyMapping) {
  // An EK1100-style coupler: no CoE mailbox and no PDO categories in SII at all → no process data.
  // The SII path succeeds with an empty mapping, so buildProcessImage skips the device.
  SdoFakeDriver driver;
  driver.protocols = 0;
  driver.sii = buildSii({});  // fixed header + End marker only
  Device device(1, driver);

  ASSERT_TRUE(device.readFlatPdoMapping().has_value());
  EXPECT_TRUE(device.flatPdoMapping().outputs.empty());
  EXPECT_TRUE(device.flatPdoMapping().inputs.empty());
}

TEST(DeviceReadFlatPdoMapping, CoeSlaveWithEmptyAssignmentDoesNotFallBackToSii) {
  // A CoE drive that assigns no PDOs must report an empty mapping — never silently adopt its SII
  // defaults. The SII here *would* yield an output, but supportsCoe() being true must ignore it.
  SdoFakeDriver driver;  // protocols defaults to CoE
  driver.sii = buildSii({{51, siiPdo1(0x1600, 0x7000, 0x01, 8)}});
  Device device(1, driver);  // 0x1C12/0x1C13 unprogrammed → empty CoE assignment

  ASSERT_TRUE(device.readFlatPdoMapping().has_value());
  EXPECT_TRUE(device.flatPdoMapping().outputs.empty());
  EXPECT_TRUE(device.flatPdoMapping().inputs.empty());
}

// --- initializeParameters: SII fallback for mailbox-less slaves --------------

TEST(DeviceInitParameters, NoCoeDeviceBuildsParametersFromSii) {
  // A mailbox-less slave has no object dictionary to enumerate; its objects come from the SII PDO
  // categories instead — an RxPDO output (BOOLEAN, 0x01) and a TxPDO input (UNSIGNED16, 0x06).
  // Each parameter is flagged ParameterOrigin::Sii, and access reflects direction: RxPDO
  // read-write (0x3F), TxPDO read-only (0x07).
  SdoFakeDriver driver;
  driver.protocols = 0;  // no CoE mailbox
  driver.sii = buildSii({{51, siiPdo1(0x1600, 0x7000, 0x01, 1, /*dataType=*/0x01)},
                         {50, siiPdo1(0x1A00, 0x6000, 0x01, 16, /*dataType=*/0x06)}});
  Device device(1, driver);

  ASSERT_TRUE(device.initializeParameters(/*readValues=*/false).has_value());

  const DeviceParameter* out = device.parameter(0x7000, 0x01);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->dataType, 0x01);  // BOOLEAN
  EXPECT_EQ(out->bitLength, 1u);
  EXPECT_EQ(out->access, 0x3F);  // read + write
  EXPECT_EQ(out->origin, ParameterOrigin::Sii);

  const DeviceParameter* in = device.parameter(0x6000, 0x01);
  ASSERT_NE(in, nullptr);
  EXPECT_EQ(in->dataType, 0x06);  // UNSIGNED16
  EXPECT_EQ(in->access, 0x07);    // read-only
  EXPECT_EQ(in->origin, ParameterOrigin::Sii);
}

TEST(DeviceInitParameters, CoeDeviceParametersHaveObjectDictionaryOrigin) {
  // A CoE device's parameters are enumerated over the object dictionary, so they carry the default
  // ParameterOrigin::ObjectDictionary — never the SII flag.
  SdoFakeDriver driver;  // protocols defaults to CoE
  driver.programOd(0x6040, 0x00, kU32);
  Device device(1, driver);

  ASSERT_TRUE(device.initializeParameters(/*readValues=*/false).has_value());
  const DeviceParameter* p = device.parameter(0x6040, 0x00);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->origin, ParameterOrigin::ObjectDictionary);
}

// --- pack / unpack mapping entry --------------------------------------------

TEST(PdoMappingEntryCodec, PacksToCoeWord) {
  // index 0x607A, subindex 0x00, 32 bits → 0x607A0020 (ETG.1000.6 §5.6.7.4.7).
  EXPECT_EQ(packMappingEntry(PdoMappingEntry{.index = 0x607A, .subindex = 0, .bitLength = 32}),
            0x607A0020u);
  // A padding gap: index 0, 8 bits → 0x00000008.
  EXPECT_EQ(packMappingEntry(PdoMappingEntry{.index = 0, .subindex = 0, .bitLength = 8}), 0x08u);
}

TEST(PdoMappingEntryCodec, UnpacksFromCoeWord) {
  const PdoMappingEntry e = unpackMappingEntry(0x60410110);
  EXPECT_EQ(e.index, 0x6041);
  EXPECT_EQ(e.subindex, 0x01);
  EXPECT_EQ(e.bitLength, 0x10);
  EXPECT_EQ(e.bitOffset, 0u);  // not carried in the packed word; left at default
}

TEST(PdoMappingEntryCodec, RoundTripsIgnoringBitOffset) {
  const PdoMappingEntry in{.index = 0x1234, .subindex = 0x56, .bitLength = 0x78, .bitOffset = 999};
  const PdoMappingEntry out = unpackMappingEntry(packMappingEntry(in));
  EXPECT_EQ(out.index, in.index);
  EXPECT_EQ(out.subindex, in.subindex);
  EXPECT_EQ(out.bitLength, in.bitLength);
}

// --- writePdoMapping -------------------------------------------------------
//
// The fake's readSdo answers from the programmed map, independent of what writeSdo records, so a
// write test programs the read map to mirror the mapping it intends to write — that read-back is
// what writePdoMapping verifies against. The shared kPreOp constant (defined near the top) is the
// only state the write is legal in; the default 0 (INIT) and SAFE-OP exercise the guard.

// A simple one-RxPDO / one-TxPDO CiA402 request: controlword + target position out, statusword +
// position-actual in.
PdoMapping cia402Request() {
  PdoMapping m;
  m.outputs.push_back({0x1600,
                       {PdoMappingEntry{.index = 0x6040, .subindex = 0, .bitLength = 16},
                        PdoMappingEntry{.index = 0x607A, .subindex = 0, .bitLength = 32}}});
  m.inputs.push_back({0x1A00,
                      {PdoMappingEntry{.index = 0x6041, .subindex = 0, .bitLength = 16},
                       PdoMappingEntry{.index = 0x6064, .subindex = 0, .bitLength = 32}}});
  return m;
}

// Programs the read map so a read-back reproduces cia402Request() exactly (so verification passes).
void programCia402ReadBack(SdoFakeDriver& driver) {
  driver.programRead(0x1C12, 0x00, u8le(1));
  driver.programRead(0x1C12, 0x01, u16le(0x1600));
  driver.programRead(0x1600, 0x00, u8le(2));
  driver.programRead(0x1600, 0x01, pdoEntry(0x6040, 0x00, 16));
  driver.programRead(0x1600, 0x02, pdoEntry(0x607A, 0x00, 32));
  driver.programRead(0x1C13, 0x00, u8le(1));
  driver.programRead(0x1C13, 0x01, u16le(0x1A00));
  driver.programRead(0x1A00, 0x00, u8le(2));
  driver.programRead(0x1A00, 0x01, pdoEntry(0x6041, 0x00, 16));
  driver.programRead(0x1A00, 0x02, pdoEntry(0x6064, 0x00, 32));
}

// Matches one recorded write against (index, subindex, bytes).
void expectWrite(const SdoFakeDriver::Write& w, uint16_t index, uint8_t subindex,
                 const std::vector<uint8_t>& data) {
  EXPECT_EQ(w.index, index);
  EXPECT_EQ(w.subindex, subindex);
  EXPECT_EQ(w.data, data);
}

TEST(DeviceWriteFlatPdoMapping, EmitsCoeSequenceInOrder) {
  SdoFakeDriver driver;
  driver.state = kPreOp;
  programCia402ReadBack(driver);
  Device device(1, driver);

  ASSERT_TRUE(device.writePdoMapping(cia402Request()).has_value());

  // 14 writes: for each direction — clear assignment, clear/fill/count the mapping object, then
  // assign the object and write the assignment count. No retry, so exactly one pass.
  ASSERT_EQ(driver.writes.size(), 14u);
  size_t i = 0;
  // Outputs → 0x1C12 / 0x1600
  expectWrite(driver.writes[i++], 0x1C12, 0x00, u8le(0));            // clear SM2 assignment
  expectWrite(driver.writes[i++], 0x1600, 0x00, u8le(0));            // clear mapping count
  expectWrite(driver.writes[i++], 0x1600, 0x01, u32le(0x60400010));  // controlword, 16 bits
  expectWrite(driver.writes[i++], 0x1600, 0x02, u32le(0x607A0020));  // target pos, 32 bits
  expectWrite(driver.writes[i++], 0x1600, 0x00, u8le(2));            // restore mapping count
  expectWrite(driver.writes[i++], 0x1C12, 0x01, u16le(0x1600));      // assign 0x1600
  expectWrite(driver.writes[i++], 0x1C12, 0x00, u8le(1));            // SM2 assignment count
  // Inputs → 0x1C13 / 0x1A00
  expectWrite(driver.writes[i++], 0x1C13, 0x00, u8le(0));
  expectWrite(driver.writes[i++], 0x1A00, 0x00, u8le(0));
  expectWrite(driver.writes[i++], 0x1A00, 0x01, u32le(0x60410010));  // statusword, 16 bits
  expectWrite(driver.writes[i++], 0x1A00, 0x02, u32le(0x60640020));  // pos actual, 32 bits
  expectWrite(driver.writes[i++], 0x1A00, 0x00, u8le(2));
  expectWrite(driver.writes[i++], 0x1C13, 0x01, u16le(0x1A00));
  expectWrite(driver.writes[i++], 0x1C13, 0x00, u8le(1));

  // The read-back refreshed the cached mapping to match the request.
  ASSERT_EQ(device.flatPdoMapping().outputs.size(), 2u);
  EXPECT_EQ(device.flatPdoMapping().outputs[0].index, 0x6040);
  EXPECT_EQ(device.flatPdoMapping().inputs[1].index, 0x6064);
}

TEST(DeviceWriteFlatPdoMapping, RejectsWhenNotPreOp) {
  SdoFakeDriver driver;
  driver.state = 4;  // SAFE-OP: sync managers active, mapping objects not writable
  Device device(1, driver);

  auto result = device.writePdoMapping(cia402Request());
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("PRE-OP"), std::string::npos);
  EXPECT_TRUE(driver.writes.empty());  // guarded before any SDO write
}

TEST(DeviceWriteFlatPdoMapping, EmptyDirectionClearsAssignment) {
  // Outputs empty: the RxPDO sync manager is cleared (count 0) and nothing is assigned; only the
  // TxPDO is configured.
  SdoFakeDriver driver;
  driver.state = kPreOp;
  driver.programRead(0x1C12, 0x00, u8le(0));  // read-back: no outputs
  driver.programRead(0x1C13, 0x00, u8le(1));
  driver.programRead(0x1C13, 0x01, u16le(0x1A00));
  driver.programRead(0x1A00, 0x00, u8le(1));
  driver.programRead(0x1A00, 0x01, pdoEntry(0x6041, 0x00, 16));

  PdoMapping m;
  m.inputs.push_back({0x1A00, {PdoMappingEntry{.index = 0x6041, .subindex = 0, .bitLength = 16}}});
  Device device(1, driver);

  ASSERT_TRUE(device.writePdoMapping(m).has_value());
  // Outputs direction: clear assignment (0), then assignment count (0) — two writes, no objects.
  expectWrite(driver.writes[0], 0x1C12, 0x00, u8le(0));
  expectWrite(driver.writes[1], 0x1C12, 0x00, u8le(0));
  EXPECT_TRUE(device.flatPdoMapping().outputs.empty());
  ASSERT_EQ(device.flatPdoMapping().inputs.size(), 1u);
}

TEST(DeviceWriteFlatPdoMapping, RetriesThenFailsOnPersistentWriteError) {
  // A mapping-object write that always fails is retried the whole sequence up to 3 times before the
  // call gives up — turning a silent half-written mapping into a reported error.
  SdoFakeDriver driver;
  driver.state = kPreOp;
  programCia402ReadBack(driver);
  driver.failWrites.insert(SdoFakeDriver::key(0x1600, 0x01));  // controlword entry always aborts
  Device device(1, driver);

  auto result = device.writePdoMapping(cia402Request());
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("after 3 attempts"), std::string::npos);
  // Each attempt reaches the failing 0x1600:01 write: clear SM2, clear 0x1600 count, then the
  // entry.
  int failingWrites = 0;
  for (const auto& w : driver.writes) {
    if (w.index == 0x1600 && w.subindex == 0x01) {
      ++failingWrites;
    }
  }
  EXPECT_EQ(failingWrites, 3);
}

TEST(DeviceWriteFlatPdoMapping, ReadBackMismatchFails) {
  // Writes succeed but the read-back does not match the request (device reports a different entry),
  // so verification fails — caught, retried, and ultimately reported.
  SdoFakeDriver driver;
  driver.state = kPreOp;
  programCia402ReadBack(driver);
  // Corrupt the read-back: 0x1600:02 reports the wrong object.
  driver.programRead(0x1600, 0x02, pdoEntry(0x1234, 0x00, 32));
  Device device(1, driver);

  auto result = device.writePdoMapping(cia402Request());
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("output mapping entry 1"), std::string::npos);
}

TEST(DeviceWriteFlatPdoMapping, TooManyEntriesRejected) {
  SdoFakeDriver driver;
  driver.state = kPreOp;
  Device device(1, driver);

  PdoMapping m;
  PdoMappingObject obj{0x1600, {}};
  obj.entries.resize(256, PdoMappingEntry{.index = 0x6040, .subindex = 0, .bitLength = 8});
  m.outputs.push_back(std::move(obj));

  auto result = device.writePdoMapping(m);
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("too many"), std::string::npos);
  EXPECT_TRUE(driver.writes.empty());  // rejected before any write
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

// --- productName -----------------------------------------------------------

TEST(DeviceProductName, ResolvesKnownSomanetProduct) {
  SdoFakeDriver driver;
  driver.info.name = "SOMANET";  // the generic group name the SII reports for every SOMANET drive
  driver.info.vendorId = mm::node::kSynapticonVendorId;
  driver.info.productCode = 0x00000301;  // SOMANET Circulo
  Device device(1, driver);

  EXPECT_EQ(device.name(), "SOMANET");
  EXPECT_EQ(device.productName(), "SOMANET Circulo");
}

TEST(DeviceProductName, FallsBackToSiiNameForUnknownProductCode) {
  SdoFakeDriver driver;
  driver.info.name = "SOMANET";
  driver.info.vendorId = mm::node::kSynapticonVendorId;
  driver.info.productCode = 0x00009999;  // Synapticon vendor, but not a code we recognise
  Device device(1, driver);

  EXPECT_EQ(device.productName(), "SOMANET");
}

TEST(DeviceProductName, FallsBackToSiiNameForForeignVendor) {
  SdoFakeDriver driver;
  driver.info.name = "Some Other Drive";
  driver.info.vendorId = 0x00000539;     // not Synapticon
  driver.info.productCode = 0x00000301;  // a code that happens to match a SOMANET product
  Device device(1, driver);

  // Product codes are only unique within a vendor, so a foreign vendor never resolves to a
  // SOMANET name even if the code collides.
  EXPECT_EQ(device.productName(), "Some Other Drive");
}

}  // namespace
