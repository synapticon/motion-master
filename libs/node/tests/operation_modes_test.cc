#include "node/operation_modes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
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
using mm::node::Device;
using mm::node::deviceOperationModes;
using mm::node::kSynapticonVendorId;
using mm::node::OperationModeInfo;
using mm::node::OperationModeKind;
using mm::node::OperationModes;
using mm::node::cia402::Object;

constexpr uint16_t kPreOp = static_cast<uint16_t>(EtherCatState::PreOp);

// The value a SOMANET Integro actually reports (its EDS default, 197549): pp, pv, tq, hm, csp, csv
// and cst supported; vl, ip and cstca not; manufacturer bits 16 and 17 set.
constexpr uint32_t kIntegroSupportedDriveModes = 0x000303AD;

std::vector<uint8_t> u16le(uint16_t v) {
  return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
}
std::vector<uint8_t> u32le(uint32_t v) {
  return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
          static_cast<uint8_t>(v >> 24)};
}

// Enough of a drive to build a CiA402 view over and read 0x6502 from.
class ModesFakeDriver : public FieldbusDriver {
 public:
  std::map<uint32_t, std::vector<uint8_t>> store;
  std::vector<OdEntry> ods;
  uint32_t vendorId = kSynapticonVendorId;

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
    e.access = 0x003F;
    ods.push_back(e);
    store[key(index, subindex)] = std::move(initial);
  }

  // The controlword and statusword are what makes the device a CiA402 drive; 0x6502 is what this
  // module reads.
  void programDrive(uint32_t supportedDriveModes) {
    programObject(Object::kControlword, 0, ObjectDataType::UNSIGNED16, u16le(0));
    programObject(Object::kStatusword, 0, ObjectDataType::UNSIGNED16, u16le(0x0040));
    programObject(Object::kSupportedDriveModes, 0, ObjectDataType::UNSIGNED32,
                  u32le(supportedDriveModes));
  }

  std::expected<std::vector<uint8_t>, std::string> readSdo(uint16_t, uint16_t index,
                                                           uint8_t subindex) override {
    auto it = store.find(key(index, subindex));
    if (it == store.end()) {
      return std::unexpected("no such object");
    }
    return it->second;
  }
  std::expected<std::vector<uint8_t>, std::string> readSdoComplete(uint16_t, uint16_t) override {
    return std::unexpected("SDO abort 0x06010000: Unsupported access to an object");
  }
  std::expected<void, std::string> writeSdo(uint16_t, uint16_t index, uint8_t subindex,
                                            std::span<const uint8_t> data) override {
    store[key(index, subindex)] = std::vector<uint8_t>(data.begin(), data.end());
    return {};
  }
  std::expected<std::vector<OdEntry>, std::string> readObjectDictionary(uint16_t) override {
    return ods;
  }

  // --- unused stubs ---------------------------------------------------------
  std::expected<void, std::string> init() override { return {}; }
  std::expected<int, std::string> scan() override { return 0; }
  SlaveInfo slaveInfo(uint16_t) const override {
    SlaveInfo info{};
    info.vendorId = vendorId;
    return info;
  }
  uint16_t slaveState(uint16_t) const override { return kPreOp; }
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

Device makeDrive(ModesFakeDriver& driver, uint32_t supportedDriveModes) {
  driver.programDrive(supportedDriveModes);
  Device device(1, driver);
  auto initialized = device.initializeParameters();
  EXPECT_TRUE(initialized.has_value()) << initialized.error();
  return device;
}

const OperationModeInfo* modeByValue(const OperationModes& modes, int value) {
  auto it = std::ranges::find(modes.modes, value, &OperationModeInfo::value);
  return it == modes.modes.end() ? nullptr : &*it;
}

TEST(DeviceOperationModes, DecodesEveryStandardBitOfSupportedDriveModes) {
  // The whole ETG.6010 Figure 15 table against a real drive's value, so a mis-numbered bit shows up
  // as the wrong mode rather than as a plausible-looking list.
  ModesFakeDriver driver;
  Device device = makeDrive(driver, kIntegroSupportedDriveModes);

  auto modes = deviceOperationModes(device);
  ASSERT_TRUE(modes.has_value()) << modes.error();

  const std::vector<std::pair<int, bool>> expected = {
      {1, true},   // pp
      {2, false},  // vl
      {3, true},   // pv
      {4, true},   // tq
      {6, true},   // hm
      {7, false},  // ip
      {8, true},   // csp
      {9, true},   // csv
      {10, true},  // cst
      {11, false}  // cstca
  };
  for (const auto& [value, supported] : expected) {
    const auto* mode = modeByValue(*modes, value);
    ASSERT_NE(mode, nullptr) << "mode " << value << " is missing";
    EXPECT_EQ(mode->kind, OperationModeKind::kStandard) << value;
    ASSERT_TRUE(mode->supported.has_value()) << value;
    EXPECT_EQ(*mode->supported, supported) << "mode " << value << " (" << mode->name << ")";
  }

  // Mode 5 does not exist — the profile reserves bit 4 — so nothing should claim it does.
  EXPECT_EQ(modeByValue(*modes, 5), nullptr);
}

TEST(DeviceOperationModes, NoModeIsSupportedWithoutABit) {
  // 0x6502 has no bit for mode 0, and reading that absence as "unsupported" would be wrong about
  // the one mode every drive accepts.
  ModesFakeDriver driver;
  Device device = makeDrive(driver, 0);

  auto modes = deviceOperationModes(device);
  ASSERT_TRUE(modes.has_value()) << modes.error();

  const auto* noMode = modeByValue(*modes, 0);
  ASSERT_NE(noMode, nullptr);
  EXPECT_FALSE(noMode->bit.has_value());
  ASSERT_TRUE(noMode->supported.has_value());
  EXPECT_TRUE(*noMode->supported);
}

TEST(DeviceOperationModes, ReportsManufacturerBitsWithoutDecodingThem) {
  ModesFakeDriver driver;
  Device device = makeDrive(driver, kIntegroSupportedDriveModes);

  auto modes = deviceOperationModes(device);
  ASSERT_TRUE(modes.has_value()) << modes.error();
  EXPECT_EQ(modes->supportedDriveModes, kIntegroSupportedDriveModes);
  EXPECT_EQ(modes->manufacturerBits, (std::vector<int>{16, 17}));
}

TEST(DeviceOperationModes, ManufacturerModesSayNothingAboutSupport) {
  // Null, not false. 0x6502 reserves bits 16-31 for the vendor without defining them, so a drive
  // cannot advertise a vendor mode in a way a master could read — and reporting "not supported"
  // for a mode this very codebase commands (diagnostics, in every measurement procedure) would be
  // a plain lie.
  ModesFakeDriver driver;
  Device device = makeDrive(driver, kIntegroSupportedDriveModes);

  auto modes = deviceOperationModes(device);
  ASSERT_TRUE(modes.has_value()) << modes.error();

  const auto* diagnostics = modeByValue(*modes, -2);
  ASSERT_NE(diagnostics, nullptr);
  EXPECT_EQ(diagnostics->name, "Diagnostics");
  EXPECT_EQ(diagnostics->kind, OperationModeKind::kManufacturer);
  EXPECT_FALSE(diagnostics->supported.has_value());
  EXPECT_FALSE(diagnostics->bit.has_value());
}

TEST(DeviceOperationModes, MarksTheDeprecatedManufacturerMode) {
  ModesFakeDriver driver;
  Device device = makeDrive(driver, kIntegroSupportedDriveModes);

  auto modes = deviceOperationModes(device);
  ASSERT_TRUE(modes.has_value()) << modes.error();

  const auto* systemIdentification = modeByValue(*modes, -4);
  ASSERT_NE(systemIdentification, nullptr);
  EXPECT_TRUE(systemIdentification->deprecated);
  EXPECT_FALSE(modeByValue(*modes, -2)->deprecated);
}

TEST(DeviceOperationModes, ListsManufacturerModesOnlyForTheVendorThatDefinesThem) {
  // A vendor's modes mean nothing on another vendor's drive: -2 is diagnostics on a SOMANET and
  // whatever that manufacturer chose on anything else.
  ModesFakeDriver driver;
  driver.vendorId = 0x00000002;
  Device device = makeDrive(driver, kIntegroSupportedDriveModes);

  auto modes = deviceOperationModes(device);
  ASSERT_TRUE(modes.has_value()) << modes.error();

  EXPECT_EQ(modeByValue(*modes, -2), nullptr);
  for (const auto& mode : modes->modes) {
    EXPECT_EQ(mode.kind, OperationModeKind::kStandard) << mode.value;
  }
  // The manufacturer bits are still reported: the drive claims something even where this code
  // cannot name the modes.
  EXPECT_EQ(modes->manufacturerBits, (std::vector<int>{16, 17}));
}

TEST(DeviceOperationModes, ModesAreAscendingByValue) {
  // The order is the contract a client renders in, so it is pinned rather than assumed from the
  // order the two halves happen to be appended in.
  ModesFakeDriver driver;
  Device device = makeDrive(driver, kIntegroSupportedDriveModes);

  auto modes = deviceOperationModes(device);
  ASSERT_TRUE(modes.has_value()) << modes.error();
  EXPECT_TRUE(std::ranges::is_sorted(modes->modes, {}, &OperationModeInfo::value));
  EXPECT_EQ(modes->modes.front().value, -6);
  EXPECT_EQ(modes->modes.back().value, 11);
}

TEST(DeviceOperationModes, FailsWhenTheDeviceIsNotACia402Drive) {
  ModesFakeDriver driver;
  Device device(1, driver);  // nothing enumerated: no controlword, no statusword
  EXPECT_FALSE(deviceOperationModes(device).has_value());
}

TEST(OperationModesToJson, CarriesNullsRatherThanOmittingKeys) {
  // A client renders this as a table, so every row needs the same keys — an absent "supported" and
  // a null one are different things to read.
  ModesFakeDriver driver;
  Device device = makeDrive(driver, kIntegroSupportedDriveModes);
  auto modes = deviceOperationModes(device);
  ASSERT_TRUE(modes.has_value()) << modes.error();

  nlohmann::json j = *modes;
  EXPECT_EQ(j.at("supportedDriveModes").get<uint32_t>(), kIntegroSupportedDriveModes);
  EXPECT_EQ(j.at("manufacturerBits"), nlohmann::json::array({16, 17}));

  const auto& rows = j.at("modes");
  const auto diagnostics = std::ranges::find_if(
      rows, [](const nlohmann::json& row) { return row.at("value").get<int>() == -2; });
  ASSERT_NE(diagnostics, rows.end());
  EXPECT_TRUE(diagnostics->at("supported").is_null());
  EXPECT_TRUE(diagnostics->at("bit").is_null());
  EXPECT_TRUE(diagnostics->at("abbreviation").is_null());
  EXPECT_EQ(diagnostics->at("kind").get<std::string>(), "manufacturer");
  EXPECT_EQ(diagnostics->at("label").get<std::string>(), "Diagnostics mode");
  // deprecated is present only where it is true, so a row that carries it means something.
  EXPECT_FALSE(diagnostics->contains("deprecated"));

  const auto csp = std::ranges::find_if(
      rows, [](const nlohmann::json& row) { return row.at("value").get<int>() == 8; });
  ASSERT_NE(csp, rows.end());
  EXPECT_TRUE(csp->at("supported").get<bool>());
  EXPECT_EQ(csp->at("bit").get<int>(), 7);
  EXPECT_EQ(csp->at("abbreviation").get<std::string>(), "csp");
}

}  // namespace
