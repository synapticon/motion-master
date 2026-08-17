#include "node/ic_haus_registers.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <nlohmann/json.hpp>
#include <string>

namespace {

using mm::node::somanet::IcHausChip;
using mm::node::somanet::IcHausRegisterSpace;
using mm::node::somanet::icHausRegisterSpaces;

// Finds one space by its name, so a test names what it means rather than an index.
const IcHausRegisterSpace* spaceNamed(std::string_view name) {
  for (const auto& space : icHausRegisterSpaces()) {
    if (space.name == name) {
      return &space;
    }
  }
  return nullptr;
}

TEST(IcHausRegisters, CarriesFiveSpacesAcrossTwoChips) {
  // Two for the iC-MU, three for the iC-PVL. Pinned because a dropped space is invisible
  // otherwise, and because the count is the whole claim the page makes about coverage.
  const auto spaces = icHausRegisterSpaces();
  ASSERT_EQ(spaces.size(), 5u);
  EXPECT_EQ(std::ranges::count(spaces, IcHausChip::kIcMu, &IcHausRegisterSpace::chip), 2);
  EXPECT_EQ(std::ranges::count(spaces, IcHausChip::kIcPvl, &IcHausRegisterSpace::chip), 3);
}

TEST(IcHausRegisters, EverySpaceIsAscendingAndDoesNotOverlap) {
  // Each row is one address or a printed range, and a transcription slip shows up here as a row
  // starting before the previous one ended.
  for (const auto& space : icHausRegisterSpaces()) {
    const auto& registers = space.registers;
    ASSERT_FALSE(registers.empty()) << space.name;
    for (const auto& r : registers) {
      EXPECT_LE(r.address, r.lastAddress) << space.name;
    }
    for (size_t i = 1; i < registers.size(); ++i) {
      EXPECT_GT(registers[i].address, registers[i - 1].lastAddress) << space.name;
    }
  }
}

TEST(IcHausRegisters, AReservedRowNamesNoFields) {
  // The two are one fact stated twice, and a row claiming both would be a transcription error.
  for (const auto& space : icHausRegisterSpaces()) {
    for (const auto& r : space.registers) {
      EXPECT_EQ(r.reserved, r.fields.empty()) << space.name << " 0x" << std::hex << int{r.address};
    }
  }
}

TEST(IcHausRegisters, OnlyTheIcMuStaticTailIsSpiOnly) {
  // The datasheet marks SER > 0x7F as SPI-reachable only, which is what puts those rows out of OS
  // command 0's reach. Nothing else in either chip carries the flag.
  for (const auto& space : icHausRegisterSpaces()) {
    for (const auto& r : space.registers) {
      if (r.spiOnly) {
        EXPECT_EQ(space.chip, IcHausChip::kIcMu) << space.name;
        EXPECT_GE(r.address, 0x80) << space.name;
      }
    }
  }
}

TEST(IcHausRegisters, TheTwoIcPvlWindowsDisagreeAboutAddressZero) {
  // The reason a space is part of a register's identity: the same chip answers address 0x00 with a
  // different register depending on which I²C device id addressed it.
  const auto* full = spaceNamed("I²C slave mode, ID 0b1100001");
  const auto* limited = spaceNamed("I²C slave mode, ID 0b1100000");
  ASSERT_NE(full, nullptr);
  ASSERT_NE(limited, nullptr);
  EXPECT_EQ(full->registers.front().fields, "EN_PAR | EN_ERR | DIR | ST_GRAY | MT_GRAY | INT_MODE");
  EXPECT_EQ(limited->registers.front().fields,
            "PRESET | PDR | BAT_WRN | BAT_ERR | POS_ERR | CTR_ERR | CFG_ERR | STUP_ERR");
}

TEST(IcHausRegisters, TheFirmwaresOwnIcMuAddressesAreAllPresent) {
  // Every address `biss_icpvl_icmu.h` names, so a transcription that drops one is caught against
  // the registers the firmware itself uses rather than only against the datasheet.
  const auto* bank0 = spaceNamed("CONF, bank 0, addresses 0x00-0x3F");
  const auto* statics = spaceNamed("Static part, addresses 0x40-0xBF");
  ASSERT_NE(bank0, nullptr);
  ASSERT_NE(statics, nullptr);

  const auto covers = [](const IcHausRegisterSpace& space, uint8_t address) {
    return std::ranges::any_of(space.registers, [address](const auto& r) {
      return address >= r.address && address <= r.lastAddress;
    });
  };

  EXPECT_TRUE(covers(*bank0, 0x0F));  // SPO_MT_MPC
  for (uint8_t address : {0x5B, 0x5C, 0x5D, 0x5E, 0x75, 0x76, 0x77}) {
    EXPECT_TRUE(covers(*statics, address)) << "0x" << std::hex << int{address};
  }
  for (uint8_t address = 0x60; address <= 0x65; ++address) {
    EXPECT_TRUE(covers(*statics, address)) << "0x" << std::hex << int{address};
  }
}

TEST(IcHausRegisters, SerialisesEachSpaceWithItsRegisters) {
  const nlohmann::json j = icHausRegisterSpaces();
  ASSERT_TRUE(j.is_array());
  ASSERT_EQ(j.size(), 5u);
  EXPECT_EQ(j[0].at("chip").get<std::string>(), "iC-MU");
  EXPECT_FALSE(j[0].at("addressing").get<std::string>().empty());

  const auto& first = j[0].at("registers").at(0);
  EXPECT_EQ(first.at("address").get<int>(), 0);
  EXPECT_EQ(first.at("lastAddress").get<int>(), 0);
  EXPECT_EQ(first.at("fields").get<std::string>(), "GC_M(1:0) | GF_M(5:0)");
  EXPECT_FALSE(first.at("reserved").get<bool>());
  EXPECT_FALSE(first.at("spiOnly").get<bool>());
}

}  // namespace
