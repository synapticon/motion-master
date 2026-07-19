#include "node/somanet_drive.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
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
using mm::node::Device;
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

}  // namespace
