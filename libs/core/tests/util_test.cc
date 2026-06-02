#include "core/util.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

using mm::core::isUrlSafeId;
using mm::core::parseHexOrDec;

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
