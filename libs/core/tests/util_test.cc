#include "core/util.h"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstdint>
#include <string>
#include <vector>

using mm::core::fromBytes;
using mm::core::isUrlSafeId;
using mm::core::parseHexOrDec;
using mm::core::toBytes;

TEST(ParseHexOrDecTest, DecimalUint16) {
  EXPECT_EQ(parseHexOrDec<uint16_t>("0"), 0u);
  EXPECT_EQ(parseHexOrDec<uint16_t>("65535"), 65535u);
  EXPECT_EQ(parseHexOrDec<uint16_t>("24676"), 24676u);
}

TEST(ParseHexOrDecTest, HexUint16) {
  EXPECT_EQ(parseHexOrDec<uint16_t>("0x0000"), 0u);
  EXPECT_EQ(parseHexOrDec<uint16_t>("0xFFFF"), 65535u);
  EXPECT_EQ(parseHexOrDec<uint16_t>("0x6064"), 24676u);
  EXPECT_EQ(parseHexOrDec<uint16_t>("0X6064"), 24676u);
}

TEST(ParseHexOrDecTest, DecimalUint8) {
  EXPECT_EQ(parseHexOrDec<uint8_t>("0"), 0u);
  EXPECT_EQ(parseHexOrDec<uint8_t>("255"), 255u);
}

TEST(ParseHexOrDecTest, HexUint8) {
  EXPECT_EQ(parseHexOrDec<uint8_t>("0x00"), 0u);
  EXPECT_EQ(parseHexOrDec<uint8_t>("0xff"), 255u);
}

TEST(ParseHexOrDecTest, HexEsiHashPrefix) {
  EXPECT_EQ(parseHexOrDec<uint16_t>("#x6064"), 24676u);
  EXPECT_EQ(parseHexOrDec<uint16_t>("#X6064"), 24676u);
  EXPECT_EQ(parseHexOrDec<uint32_t>("#x00000201"), 513u);
  EXPECT_EQ(parseHexOrDec<uint8_t>("#xff"), 255u);
}

TEST(ParseHexOrDecTest, RejectsEmpty) { EXPECT_EQ(parseHexOrDec<uint16_t>(""), std::nullopt); }

TEST(ParseHexOrDecTest, RejectsTrailingChars) {
  EXPECT_EQ(parseHexOrDec<uint16_t>("123abc"), std::nullopt);
  EXPECT_EQ(parseHexOrDec<uint16_t>("0x6064z"), std::nullopt);
}

TEST(ParseHexOrDecTest, RejectsOverflow) {
  EXPECT_EQ(parseHexOrDec<uint16_t>("65536"), std::nullopt);
  EXPECT_EQ(parseHexOrDec<uint8_t>("256"), std::nullopt);
  EXPECT_EQ(parseHexOrDec<uint8_t>("0x100"), std::nullopt);
}

TEST(ParseHexOrDecTest, RejectsNegative) { EXPECT_EQ(parseHexOrDec<uint16_t>("-1"), std::nullopt); }

TEST(ParseHexOrDecTest, RejectsBareHexPrefix) {
  EXPECT_EQ(parseHexOrDec<uint16_t>("0x"), std::nullopt);
}

TEST(IsUrlSafeIdTest, AcceptsTypicalIds) {
  EXPECT_TRUE(isUrlSafeId("left-leg"));
  EXPECT_TRUE(isUrlSafeId("Left_Leg.1"));
  EXPECT_TRUE(isUrlSafeId("a"));
  EXPECT_TRUE(isUrlSafeId("axis-0_actual.position-2"));
}

TEST(IsUrlSafeIdTest, AcceptsBoundaryLengths) {
  EXPECT_TRUE(isUrlSafeId(std::string(1, 'x')));
  EXPECT_TRUE(isUrlSafeId(std::string(64, 'x')));
  EXPECT_FALSE(isUrlSafeId(std::string(65, 'x')));
}

TEST(IsUrlSafeIdTest, RejectsEmpty) { EXPECT_FALSE(isUrlSafeId("")); }

TEST(IsUrlSafeIdTest, RejectsDisallowedCharacters) {
  EXPECT_FALSE(isUrlSafeId("left/leg"));
  EXPECT_FALSE(isUrlSafeId("left leg"));
  EXPECT_FALSE(isUrlSafeId("left%20leg"));
  EXPECT_FALSE(isUrlSafeId("left?leg"));
  EXPECT_FALSE(isUrlSafeId("left#leg"));
  EXPECT_FALSE(isUrlSafeId("leg:1"));
  EXPECT_FALSE(isUrlSafeId("über"));
}

TEST(IsUrlSafeIdTest, IsCaseSensitiveAndDistinct) {
  EXPECT_TRUE(isUrlSafeId("Motor"));
  EXPECT_TRUE(isUrlSafeId("motor"));
}

TEST(ToBytesTest, WidthMatchesType) {
  EXPECT_EQ(toBytes<uint8_t>(0).size(), 1u);
  EXPECT_EQ(toBytes<uint16_t>(0).size(), 2u);
  EXPECT_EQ(toBytes<uint32_t>(0).size(), 4u);
  EXPECT_EQ(toBytes<uint64_t>(0).size(), 8u);
}

TEST(ToBytesTest, LittleEndianByDefault) {
  EXPECT_EQ(toBytes<uint8_t>(0xAB), (std::array<uint8_t, 1>{0xAB}));
  EXPECT_EQ(toBytes<uint16_t>(0x1234), (std::array<uint8_t, 2>{0x34, 0x12}));
  // A packed CoE PDO mapping word: index 0x607A, subindex 0x00, 32 bits → 0x607A0020.
  EXPECT_EQ(toBytes<uint32_t>(0x607A0020), (std::array<uint8_t, 4>{0x20, 0x00, 0x7A, 0x60}));
}

TEST(ToBytesTest, BigEndianReversesByteOrder) {
  EXPECT_EQ(toBytes<uint16_t>(0x1234, std::endian::big), (std::array<uint8_t, 2>{0x12, 0x34}));
  EXPECT_EQ(toBytes<uint32_t>(0x607A0020, std::endian::big),
            (std::array<uint8_t, 4>{0x60, 0x7A, 0x00, 0x20}));
}

TEST(ToBytesTest, EncodesSignedThroughTwosComplement) {
  EXPECT_EQ(toBytes<int16_t>(-1), (std::array<uint8_t, 2>{0xFF, 0xFF}));
  EXPECT_EQ(toBytes<int32_t>(-2), (std::array<uint8_t, 4>{0xFE, 0xFF, 0xFF, 0xFF}));
}

TEST(FromBytesTest, LittleEndianByDefault) {
  EXPECT_EQ(fromBytes<uint8_t>(std::vector<uint8_t>{0xAB}), 0xABu);
  EXPECT_EQ(fromBytes<uint16_t>(std::vector<uint8_t>{0x34, 0x12}), 0x1234u);
  EXPECT_EQ(fromBytes<uint32_t>(std::vector<uint8_t>{0x20, 0x00, 0x7A, 0x60}), 0x607A0020u);
}

TEST(FromBytesTest, BigEndianReadsMostSignificantFirst) {
  EXPECT_EQ(fromBytes<uint16_t>(std::vector<uint8_t>{0x12, 0x34}, std::endian::big), 0x1234u);
  EXPECT_EQ(fromBytes<uint32_t>(std::vector<uint8_t>{0x60, 0x7A, 0x00, 0x20}, std::endian::big),
            0x607A0020u);
}

TEST(FromBytesTest, ShortBufferReadsAsZeroPadded) {
  EXPECT_EQ(fromBytes<uint8_t>(std::vector<uint8_t>{}), 0u);                      // empty
  EXPECT_EQ(fromBytes<uint32_t>(std::vector<uint8_t>{0x34, 0x12}), 0x00001234u);  // 2 of 4 bytes
}

TEST(FromBytesTest, ExcessBytesIgnored) {
  EXPECT_EQ(fromBytes<uint16_t>(std::vector<uint8_t>{0x34, 0x12, 0xFF, 0xFF}), 0x1234u);
}

TEST(BytesRoundTrip, EncodeThenDecodeIsIdentity) {
  for (uint32_t v : {0u, 1u, 0xFFu, 0x1234u, 0xDEADBEEFu, 0xFFFFFFFFu}) {
    EXPECT_EQ(fromBytes<uint32_t>(toBytes(v)), v);
    EXPECT_EQ(fromBytes<uint32_t>(toBytes(v, std::endian::big), std::endian::big), v);
  }
}
