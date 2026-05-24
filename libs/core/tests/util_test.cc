#include "core/util.h"

#include <gtest/gtest.h>

#include <cstdint>

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
