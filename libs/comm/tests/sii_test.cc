#include "comm/sii.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace mm::comm {
namespace {

// Real SII EEPROM image captured from a SOMANET Circulo (Integro variant): a 128-byte fixed header
// followed by STRINGS (10), GENERAL (30), FMMU (40) and SYNC_M (41) categories, terminated by the
// 0xFFFF END marker. The same fixture backs the original TypeScript parser's snapshot test.
constexpr std::array<uint8_t, 252> kIntegroSii = {
    0x8D, 0x3E, 0x02, 0x8C, 0xE8, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x58, 0x00,
    0xD2, 0x22, 0x00, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x04, 0x00, 0x14, 0x00, 0x04,
    0x00, 0x10, 0x00, 0x04, 0x00, 0x14, 0x00, 0x04, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x01, 0x00,
    0x0A, 0x00, 0x13, 0x00, 0x02, 0x07, 0x53, 0x4F, 0x4D, 0x41, 0x4E, 0x45, 0x54, 0x1C, 0x53, 0x4F,
    0x4D, 0x41, 0x4E, 0x45, 0x54, 0x20, 0x43, 0x69, 0x72, 0x63, 0x75, 0x6C, 0x6F, 0x20, 0x43, 0x69,
    0x41, 0x34, 0x30, 0x32, 0x20, 0x44, 0x72, 0x69, 0x76, 0x65, 0x1E, 0x00, 0x10, 0x00, 0x01, 0x00,
    0x00, 0x02, 0x00, 0x0F, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x11, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x28, 0x00,
    0x02, 0x00, 0x01, 0x02, 0x03, 0x00, 0x29, 0x00, 0x10, 0x00, 0x00, 0x10, 0x00, 0x04, 0x26, 0x00,
    0x01, 0x01, 0x00, 0x14, 0x00, 0x04, 0x22, 0x00, 0x01, 0x02, 0x00, 0x18, 0x23, 0x00, 0x64, 0x00,
    0x01, 0x03, 0x00, 0x1C, 0x2F, 0x00, 0x20, 0x00, 0x01, 0x04, 0xFF, 0xFF,
};

TEST(SiiParseTest, DecodesFixedHeader) {
  auto result = parseSii(kIntegroSii);
  ASSERT_TRUE(result.has_value()) << result.error();
  const SiiInfo& info = result->info;

  EXPECT_EQ(info.vendorId, 0x22D2u);  // Synapticon.
  EXPECT_EQ(info.productCode, 0x301u);
  EXPECT_EQ(info.revisionNumber, 0x0E000000u);
  EXPECT_EQ(info.serialNumber, 0u);
  EXPECT_EQ(info.standardReceiveMailboxOffset, 0x1000u);
  EXPECT_EQ(info.standardReceiveMailboxSize, 0x400u);
  EXPECT_EQ(info.standardSendMailboxOffset, 0x1400u);
  EXPECT_EQ(info.standardSendMailboxSize, 0x400u);
  EXPECT_EQ(info.mailboxProtocol, 0x0Cu);  // CoE (0x04) + FoE (0x08).
  EXPECT_EQ(info.size, 0xFFu);
  EXPECT_EQ(info.version, 1u);
}

TEST(SiiParseTest, DecodesStrings) {
  auto result = parseSii(kIntegroSii);
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->category.strings.size(), 2u);
  EXPECT_EQ(result->category.strings[0], "SOMANET");
  EXPECT_EQ(result->category.strings[1], "SOMANET Circulo CiA402 Drive");
}

TEST(SiiParseTest, DecodesGeneral) {
  auto result = parseSii(kIntegroSii);
  ASSERT_TRUE(result.has_value());
  const SiiCategoryGeneral& g = result->category.general;
  EXPECT_EQ(g.groupIdx, 1u);
  EXPECT_EQ(g.nameIdx, 2u);  // -> strings[1] "SOMANET Circulo CiA402 Drive" (1-based index).
  // General-category detail bytes, at the correct ETG.2000 offsets (a reserved byte precedes
  // coeDetails at offset 5). coeDetails 0x0F = SDO | SDO-Info | PDO-Assign | PDO-Config, matching
  // what SOEM's CoEdetails / the bus-config CoE capabilities report for this drive.
  EXPECT_EQ(g.coeDetails, 0x0Fu);
  EXPECT_EQ(g.foeDetails, 1u);
  EXPECT_EQ(g.eoeDetails, 0u);
  EXPECT_EQ(g.physicalPort, 1);
  EXPECT_EQ(g.physicalMemoryAddress, 0x11u);
}

TEST(SiiParseTest, DecodesSyncManagers) {
  auto result = parseSii(kIntegroSii);
  ASSERT_TRUE(result.has_value());
  const auto& sms = result->category.syncManagers;
  ASSERT_EQ(sms.size(), 4u);
  EXPECT_EQ(sms[0].physicalStartAddress, 0x1000u);
  EXPECT_EQ(sms[0].length, 0x400u);
  EXPECT_EQ(sms[0].syncManagerType, 1u);  // MbxOut.
  EXPECT_EQ(sms[1].syncManagerType, 2u);  // MbxIn.
  EXPECT_EQ(sms[2].syncManagerType, 3u);  // Outputs.
  EXPECT_EQ(sms[3].syncManagerType, 4u);  // Inputs.
}

TEST(SiiParseTest, FmmuPresentNoPdoOrDc) {
  auto result = parseSii(kIntegroSii);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->category.fmmus.size(), 2u);  // category 40 payload is 2 words.
  EXPECT_TRUE(result->category.rxPdos.empty());
  EXPECT_TRUE(result->category.txPdos.empty());
  EXPECT_TRUE(result->category.distributedClocks.empty());
}

TEST(SiiParseTest, RejectsTruncatedHeader) {
  std::array<uint8_t, 64> tooSmall{};
  auto result = parseSii(tooSmall);
  EXPECT_FALSE(result.has_value());
}

TEST(SiiParseTest, SerialisesToJson) {
  auto result = parseSii(kIntegroSii);
  ASSERT_TRUE(result.has_value());
  nlohmann::json j = *result;
  EXPECT_EQ(j["info"]["vendorId"], 0x22D2u);
  EXPECT_EQ(j["category"]["strings"][0], "SOMANET");
  EXPECT_EQ(j["category"]["syncManagers"].size(), 4u);
  EXPECT_TRUE(j["category"]["general"].contains("physicalMemoryAddress"));
}

TEST(SiiResolveCategoryTest, MapsKnownAndRangeValues) {
  EXPECT_EQ(resolveSiiCategoryType(0), SiiCategoryType::Nop);
  EXPECT_EQ(resolveSiiCategoryType(1), SiiCategoryType::DeviceSpecific);
  EXPECT_EQ(resolveSiiCategoryType(7), SiiCategoryType::DeviceSpecific);
  EXPECT_EQ(resolveSiiCategoryType(50), SiiCategoryType::TxPdo);
  EXPECT_EQ(resolveSiiCategoryType(51), SiiCategoryType::RxPdo);
  EXPECT_EQ(resolveSiiCategoryType(0x2002), SiiCategoryType::ApplicationSpecific);
  EXPECT_EQ(resolveSiiCategoryType(0x3003), SiiCategoryType::VendorSpecific);
  EXPECT_EQ(resolveSiiCategoryType(0xFFFF), SiiCategoryType::End);
  EXPECT_FALSE(resolveSiiCategoryType(0x0205).has_value());  // Unrecognised fixed value.
}

}  // namespace
}  // namespace mm::comm
