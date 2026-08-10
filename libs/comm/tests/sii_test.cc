#include "comm/sii.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

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
  // Physical Port at payload offset 16 (after the currentOnEBus word + the duplicate-GroupIdx and
  // reserved bytes): 0x0011 = port 0 MII, port 1 MII — the two-port Circulo daisy-chain. Physical
  // Memory Address (offset 18) is 0: this drive mirrors its ID selector via AL status, not PhyMem.
  EXPECT_EQ(g.physicalPort, 0x0011u);
  EXPECT_EQ(g.physicalMemoryAddress, 0u);
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
  // Category 40 payload is 4 bytes, one Unsigned8 per FMMU (ETG.2010 Table 9): FMMU0 Outputs,
  // FMMU1 Inputs, FMMU2 SyncM-status (read mailbox), plus a trailing pad byte for word alignment.
  ASSERT_EQ(result->category.fmmus.size(), 4u);
  EXPECT_EQ(result->category.fmmus[0], 0x01u);  // Outputs.
  EXPECT_EQ(result->category.fmmus[1], 0x02u);  // Inputs.
  EXPECT_EQ(result->category.fmmus[2], 0x03u);  // SyncM status (read mailbox).
  EXPECT_EQ(result->category.fmmus[3], 0x00u);  // Padding.
  EXPECT_TRUE(result->category.rxPdos.empty());
  EXPECT_TRUE(result->category.txPdos.empty());
  EXPECT_TRUE(result->category.distributedClocks.empty());
}

TEST(SiiParseTest, DecodesAllMailboxCapabilityBits) {
  // Synthetic image: a 128-byte header with every mailbox-protocol bit set, then a GENERAL category
  // with every CoE-detail bit set (plus FoE/EoE enabled), then the END marker. The captured Integro
  // fixture only carries CoE+FoE, so this locks the full bitfield decode — the AoE/EoE/SoE/VoE
  // protocol bits and CoE detail bits 4-5 (Upload, Complete Access) — through the parser, proving
  // the header word and the General detail bytes are read at the right offsets with no masking.
  constexpr size_t kHeaderBytes = 128;
  std::vector<uint8_t> img(kHeaderBytes, 0);
  img[56] = 0x3F;  // mailboxProtocol (word 0x1C, low byte): AoE|EoE|CoE|FoE|SoE|VoE.

  // GENERAL category (type 30): type word + following-word-size word + 16-word payload.
  constexpr uint16_t kGeneralWords = 16;
  img.push_back(30);  // Category type low byte.
  img.push_back(0);   // Category type high byte.
  img.push_back(static_cast<uint8_t>(kGeneralWords));
  img.push_back(static_cast<uint8_t>(kGeneralWords >> 8));
  std::vector<uint8_t> general(static_cast<size_t>(kGeneralWords) * 2, 0);
  general[5] = 0x3F;  // coeDetails: SDO|SDO-Info|PDO-Assign|PDO-Config|Upload|Complete-Access.
  general[6] = 0x01;  // foeDetails: FoE enabled.
  general[7] = 0x01;  // eoeDetails: EoE enabled.
  img.insert(img.end(), general.begin(), general.end());

  img.push_back(0xFF);  // END marker.
  img.push_back(0xFF);

  auto result = parseSii(img);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(result->info.mailboxProtocol, 0x3Fu);  // All six mailbox protocols advertised.
  const SiiCategoryGeneral& g = result->category.general;
  EXPECT_EQ(g.coeDetails, 0x3Fu);  // All six CoE detail bits.
  EXPECT_EQ(g.foeDetails, 0x01u);
  EXPECT_EQ(g.eoeDetails, 0x01u);
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

// The image a firmware package ships as `somanet_cia402.sii` is byte-identical to this fixture, so
// the validator is pinned against the same real EEPROM contents the parser is.
TEST(SiiValidateTest, AcceptsARealImage) {
  auto result = validateSiiImage(kIntegroSii);
  EXPECT_TRUE(result.has_value()) << result.error();
}

TEST(SiiValidateTest, RejectsAnOddLength) {
  std::vector<uint8_t> image(kIntegroSii.begin(), kIntegroSii.end());
  image.pop_back();
  EXPECT_FALSE(validateSiiImage(image));
}

TEST(SiiValidateTest, RejectsAnImageTooShortToHoldACategory) {
  // The fixed header alone (0x40 words): well-formed as far as it goes, but there is no first
  // category header to start the walk from.
  std::vector<uint8_t> image(kIntegroSii.begin(), kIntegroSii.begin() + 128);
  EXPECT_FALSE(validateSiiImage(image));
}

TEST(SiiValidateTest, RejectsACorruptedChecksum) {
  std::vector<uint8_t> image(kIntegroSii.begin(), kIntegroSii.end());
  image[14] ^= 0xFF;
  auto result = validateSiiImage(image);
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().find("checksum"), std::string::npos) << result.error();
}

// A flipped bit anywhere in words 0-6 must be caught even though the checksum byte is untouched —
// that is the point of the CRC covering the identity fields.
TEST(SiiValidateTest, RejectsAFlippedHeaderBit) {
  std::vector<uint8_t> image(kIntegroSii.begin(), kIntegroSii.end());
  image[2] ^= 0x01;
  EXPECT_FALSE(validateSiiImage(image));
}

TEST(SiiValidateTest, RejectsATruncatedCategorySection) {
  // Drop the trailing end marker: the walk then runs off the end with nothing terminating it.
  std::vector<uint8_t> image(kIntegroSii.begin(), kIntegroSii.end() - 2);
  auto result = validateSiiImage(image);
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().find("end marker"), std::string::npos) << result.error();
}

TEST(SiiValidateTest, RejectsACategoryClaimingMoreWordsThanTheImageHolds) {
  std::vector<uint8_t> image(kIntegroSii.begin(), kIntegroSii.end());
  // Word 0x41 is the size of the first category (STRINGS); inflate it past the image.
  constexpr size_t kFirstCategorySizeWord = 0x41;
  image[kFirstCategorySizeWord * 2] = 0xFF;
  image[kFirstCategorySizeWord * 2 + 1] = 0x00;
  auto result = validateSiiImage(image);
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().find("runs past"), std::string::npos) << result.error();
}

}  // namespace
}  // namespace mm::comm
