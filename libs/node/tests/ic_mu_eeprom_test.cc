#include "node/ic_mu_eeprom.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

using mm::node::somanet::IcMuI2cChunk;
using mm::node::somanet::IcMuI2cTarget;
using mm::node::somanet::IcMuRegisterWrite;

TEST(IcMuEepromAddress, MapsTheConfigurationAndTheSpoTable) {
  using mm::node::somanet::icMuEepromAddress;
  EXPECT_EQ(icMuEepromAddress(0x00), 0x00);
  EXPECT_EQ(icMuEepromAddress(0x18), 0x18);
  EXPECT_EQ(icMuEepromAddress(0x52), 0x19);  // SPO_BASE
  EXPECT_EQ(icMuEepromAddress(0x59), 0x20);
  EXPECT_FALSE(icMuEepromAddress(0x19));
  EXPECT_FALSE(icMuEepromAddress(0x51));
  EXPECT_FALSE(icMuEepromAddress(0x5A));
  EXPECT_FALSE(icMuEepromAddress(0x75));
}

TEST(IcMuEepromAddress, CoversEveryRestoreRegisterOnceAndInOrder) {
  std::vector<uint8_t> eeprom;
  for (const uint8_t ram : mm::node::somanet::icMuRestoreRegisters()) {
    const auto address = mm::node::somanet::icMuEepromAddress(ram);
    ASSERT_TRUE(address) << static_cast<int>(ram);
    eeprom.push_back(*address);
  }
  ASSERT_EQ(eeprom.size(), 33u);
  for (size_t i = 0; i < eeprom.size(); ++i) {
    EXPECT_EQ(eeprom[i], i);
  }
}

TEST(IcMuI2cDeviceId, AddsTheReadBitToEachTarget) {
  using mm::node::somanet::icMuI2cDeviceId;
  EXPECT_EQ(icMuI2cDeviceId(IcMuI2cTarget::kEeprom, true), 0xA0);
  EXPECT_EQ(icMuI2cDeviceId(IcMuI2cTarget::kEeprom, false), 0xA1);
  EXPECT_EQ(icMuI2cDeviceId(IcMuI2cTarget::kIcPvl, true), 0xC2);
  EXPECT_EQ(icMuI2cDeviceId(IcMuI2cTarget::kIcPvl, false), 0xC3);
  EXPECT_EQ(icMuI2cDeviceId(IcMuI2cTarget::kIcPvlStatus, true), 0xC0);
  EXPECT_EQ(icMuI2cDeviceId(IcMuI2cTarget::kIcPvlStatus, false), 0xC1);
}

TEST(IcMuI2cTarget, NamesRoundTrip) {
  for (const auto target :
       {IcMuI2cTarget::kEeprom, IcMuI2cTarget::kIcPvl, IcMuI2cTarget::kIcPvlStatus}) {
    EXPECT_EQ(mm::node::somanet::parseIcMuI2cTarget(mm::node::somanet::toString(target)), target);
  }
  EXPECT_FALSE(mm::node::somanet::parseIcMuI2cTarget("EEPROM"));
  EXPECT_FALSE(mm::node::somanet::parseIcMuI2cTarget(""));
}

TEST(IcMuI2cTransferSetup, PointsTheTransferAtTheExchangeRegisters) {
  const std::vector<IcMuRegisterWrite> expected = {
      {0x5E, 0xA1}, {0x5C, 0x60}, {0x5B, 0x40}, {0x5D, 0x6F}};
  EXPECT_EQ(mm::node::somanet::icMuI2cTransferSetup(0xA1, 0x40, 16), expected);
}

TEST(IcMuEepromReadChunks, SplitsTheConfigurationIntoTransfersOf16Bytes) {
  const std::vector<IcMuI2cChunk> expected = {
      {0x00, 16}, {0x10, 16}, {0x20, 16}, {0x30, 16}, {0x40, 13}};
  EXPECT_EQ(mm::node::somanet::icMuEepromReadChunks(0x00, 0x4C), expected);
  const std::vector<IcMuI2cChunk> one = {{0x05, 1}};
  EXPECT_EQ(mm::node::somanet::icMuEepromReadChunks(0x05, 0x05), one);
}

TEST(IcMuEepromWriteChunks, KeepsEachWriteInsideAnAlignedPageOf8Bytes) {
  const auto writes = mm::node::somanet::icMuEepromWriteChunks(0x00, 0x4C);
  ASSERT_EQ(writes.size(), 10u);
  unsigned next = 0;
  for (const auto& chunk : writes) {
    EXPECT_EQ(chunk.deviceStart, next);
    EXPECT_EQ(chunk.deviceStart / 8, (chunk.deviceStart + chunk.count - 1) / 8);
    next = chunk.deviceStart + chunk.count;
  }
  EXPECT_EQ(next, 0x4Du);

  // The iC-PVL configuration, 0x40-0x4C, crosses the page boundary at 0x48.
  const std::vector<IcMuI2cChunk> icPvl = {{0x40, 8}, {0x48, 5}};
  EXPECT_EQ(mm::node::somanet::icMuEepromWriteChunks(0x40, 0x4C), icPvl);
  const std::vector<IcMuI2cChunk> unaligned = {{0x05, 3}, {0x08, 2}};
  EXPECT_EQ(mm::node::somanet::icMuEepromWriteChunks(0x05, 0x09), unaligned);
}

}  // namespace
