#include "etg/esi_data_type.h"

#include <gtest/gtest.h>

#include <format>
#include <string>
#include <unordered_set>

namespace mm::etg {
namespace {

TEST(EsiDataTypeTest, MapsIec61131NamesToEtg1020Codes) {
  // An ESI names types with IEC 61131-3 spellings while the CoE wire uses ETG.1020 numbering, and
  // the two vocabularies do not overlap: "UDINT" and "UNSIGNED32" are the same type. These pairs
  // are the bridge, cross-checked against libs/comm/object_data_types.h — duplicated as literals
  // rather than included, so a pure XML parser need not link SOEM.
  const struct {
    const char* name;
    uint16_t code;
    uint16_t bitSize;
    bool isSigned;
  } expected[] = {
      {"BOOL", 0x0001, 1, false},    {"SINT", 0x0002, 8, true},     {"INT", 0x0003, 16, true},
      {"DINT", 0x0004, 32, true},    {"LINT", 0x0015, 64, true},    {"USINT", 0x0005, 8, false},
      {"UINT", 0x0006, 16, false},   {"UDINT", 0x0007, 32, false},  {"ULINT", 0x001B, 64, false},
      {"REAL", 0x0008, 32, false},   {"LREAL", 0x0011, 64, false},  {"BYTE", 0x001E, 8, false},
      {"DWORD", 0x0020, 32, false},  {"GUID", 0x001D, 128, false},  {"INT24", 0x0010, 24, true},
      {"UINT24", 0x0016, 24, false}, {"BITARR8", 0x002D, 8, false}, {"BIT3", 0x0032, 3, false},
  };

  for (const auto& e : expected) {
    const auto resolved = resolvePrimitiveType(e.name);
    ASSERT_TRUE(resolved.has_value()) << e.name;
    EXPECT_EQ(resolved->code, e.code) << e.name;
    EXPECT_EQ(resolved->bitSize, e.bitSize) << e.name;
    EXPECT_EQ(resolved->isSigned, e.isSigned) << e.name;
  }
}

TEST(EsiDataTypeTest, WordIsABitStringNotAnUnsignedInteger) {
  // WORD (0x001F) and UINT (0x0006) are both 16 bits, but a device reporting WORD is describing a
  // bitfield. Collapsing them would render a status word as a decimal number. v5.6.6.xml uses
  // WORD, so this is a live distinction, not a theoretical one.
  const auto word = resolvePrimitiveType("WORD");
  const auto uint = resolvePrimitiveType("UINT");
  ASSERT_TRUE(word.has_value());
  ASSERT_TRUE(uint.has_value());
  EXPECT_EQ(word->code, 0x001F);
  EXPECT_EQ(uint->code, 0x0006);
  EXPECT_EQ(word->bitSize, uint->bitSize);
}

TEST(EsiDataTypeTest, ComputesTheWidthOfParameterisedStringTypes) {
  const auto s50 = resolvePrimitiveType("STRING(50)");
  ASSERT_TRUE(s50.has_value());
  EXPECT_EQ(s50->code, 0x0009);  // VISIBLE_STRING.
  EXPECT_EQ(s50->bitSize, 400);  // 50 bytes.

  const auto octet = resolvePrimitiveType("OCTET_STRING(8)");
  ASSERT_TRUE(octet.has_value());
  EXPECT_EQ(octet->code, 0x000A);
  EXPECT_EQ(octet->bitSize, 64);

  // UNICODE_STRING counts UCS-2 code units, so its width is twice the element count.
  const auto unicode = resolvePrimitiveType("UNICODE_STRING(8)");
  ASSERT_TRUE(unicode.has_value());
  EXPECT_EQ(unicode->code, 0x000B);
  EXPECT_EQ(unicode->bitSize, 128);

  EXPECT_TRUE(isStringTypeName("STRING(2)"));
  EXPECT_TRUE(isStringTypeName("OCTET_STRING(16)"));
  EXPECT_FALSE(isStringTypeName("UDINT"));
  EXPECT_FALSE(isStringTypeName("STRING"));  // Unparameterised: not a concrete type.
}

TEST(EsiDataTypeTest, RejectsMalformedOrNonPrimitiveNames) {
  EXPECT_FALSE(resolvePrimitiveType("STRING()").has_value());
  EXPECT_FALSE(resolvePrimitiveType("STRING(abc)").has_value());
  EXPECT_FALSE(resolvePrimitiveType("STRING(50").has_value());
  EXPECT_FALSE(resolvePrimitiveType("").has_value());
  // A composite declared by the dictionary, not a primitive: the caller resolves it through the
  // dictionary's own DataType table instead.
  EXPECT_FALSE(resolvePrimitiveType("DT1018").has_value());
}

TEST(EsiDataTypeTest, SaturatesRatherThanWrapsAnAbsurdStringWidth) {
  // 65536 bytes is 524288 bits, far past the 16-bit width field. Wrapping would silently report a
  // small width for a huge type; saturating leaves the value obviously wrong so the flattener's
  // width-mismatch check fires instead of quietly truncating data.
  const auto huge = resolvePrimitiveType("STRING(65536)");
  ASSERT_TRUE(huge.has_value());
  EXPECT_EQ(huge->bitSize, 0xFFFF);
}

TEST(EsiDataTypeTest, ResolvesAnArrayCompositionToItsElementType) {
  // "ARRAY [0..7] OF BYTE" names the ETG.2000 Table 12 ARRAY INFO helper type. It never becomes
  // an entry's own type — the entry takes the element type via <BaseType> — but resolving it to
  // the element keeps a caller that asks anyway from getting nothing.
  EXPECT_EQ(arrayElementTypeName("ARRAY [0..7] OF BYTE"), "BYTE");
  EXPECT_EQ(arrayElementTypeName("ARRAY [0..24] OF USINT"), "USINT");
  EXPECT_TRUE(arrayElementTypeName("UDINT").empty());
  EXPECT_TRUE(arrayElementTypeName("ARRAY").empty());

  const auto resolved = resolvePrimitiveType("ARRAY [0..7] OF BYTE");
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(resolved->code, 0x001E);  // BYTE.
}

TEST(EsiDataTypeTest, TheTableIsWellFormed) {
  std::unordered_set<std::string> names;
  for (const PrimitiveType& type : kPrimitiveTypes) {
    EXPECT_FALSE(type.name.empty());
    EXPECT_TRUE(names.insert(std::string(type.name)).second) << "duplicate entry " << type.name;
    EXPECT_NE(type.code, 0) << type.name;
    // Only integers are two's complement. A string or bit-string wrongly marked signed would make
    // the flattener sign-extend a short default and corrupt it.
    if (type.isSigned) {
      EXPECT_GT(type.bitSize, 0) << type.name;
    }
  }

  // BITn declares its own width in its name; a mismatch would silently mis-size an entry.
  for (int bits = 1; bits <= 16; ++bits) {
    const auto resolved = resolvePrimitiveType(std::format("BIT{}", bits));
    ASSERT_TRUE(resolved.has_value()) << bits;
    EXPECT_EQ(resolved->bitSize, bits);
  }
}

TEST(EsiDataTypeTest, ObjectCodeValuesAreTheCoeWireCodes) {
  // The enum's underlying value is assigned straight to DeviceParameter::objectCode, so it has to
  // be ETG.1000.6 Table 41's numbering rather than an arbitrary ordinal.
  EXPECT_EQ(static_cast<uint16_t>(ObjectCode::Var), 0x07);
  EXPECT_EQ(static_cast<uint16_t>(ObjectCode::Array), 0x08);
  EXPECT_EQ(static_cast<uint16_t>(ObjectCode::Record), 0x09);

  EXPECT_EQ(objectCodeName(ObjectCode::Var), "VAR");
  EXPECT_EQ(objectCodeName(ObjectCode::Array), "ARRAY");
  EXPECT_EQ(objectCodeName(ObjectCode::Record), "RECORD");
}

}  // namespace
}  // namespace mm::etg
