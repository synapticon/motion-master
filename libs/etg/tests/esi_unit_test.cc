#include "etg/esi_unit.h"

#include <gtest/gtest.h>

#include <vector>

namespace mm::etg {
namespace {

TEST(EsiUnitTest, SplitsTheNotationValueMostSignificantByteFirst) {
  // #xFD260000 is milli (0xFD, a two's-complement exponent of -3) volt (0x26), dimensionless
  // denominator. Reading the bytes the other way round would yield nonsense for every unit.
  const UnitParts parts = esiUnitParts(0xFD260000);
  EXPECT_EQ(parts.prefix, 0xFD);
  EXPECT_EQ(parts.numerator, 0x26);
  EXPECT_EQ(parts.denominator, 0x00);
  EXPECT_EQ(parts.reserved, 0x00);
}

TEST(EsiUnitTest, ComposesPrefixNumeratorAndDenominator) {
  EXPECT_EQ(esiUnitSymbol(0xFD260000), "mV");       // milli + volt.
  EXPECT_EQ(esiUnitSymbol(0x03200000), "kHz");      // kilo + hertz.
  EXPECT_EQ(esiUnitSymbol(0x00B50000), "Inc");      // increments, no prefix, no denominator.
  EXPECT_EQ(esiUnitSymbol(0xF756B500), "nNm/Inc");  // nano + newtonmetre per increment.
  EXPECT_EQ(esiUnitSymbol(0xFD030000), "ms");       // milli + second.
  EXPECT_EQ(esiUnitSymbol(0x00040000), "A");        // ampere.
}

TEST(EsiUnitTest, TreatsTheDimensionlessUnitAsAnAbsenceNotADivisor) {
  // Notation 0x00 is "1". Rendering it literally would give "Inc/1" for a plain increment count,
  // and a bare "1" for a dimensionless value.
  EXPECT_EQ(esiUnitSymbol(0x00B50000), "Inc");
  EXPECT_EQ(esiUnitSymbol(0x00000000), "");  // Unit absent entirely.

  // A prefix on nothing is meaningless; reporting "m" would read as metres.
  EXPECT_EQ(esiUnitSymbol(0xFD000000), "");
}

TEST(EsiUnitTest, CollapsesTheRevolutionsPerMinuteComposition) {
  // 0x00B44700 composes literally to "Revolution/min". Every vendor writes that value meaning
  // rotational speed and every user expects to read "rpm".
  EXPECT_EQ(esiUnitSymbol(0x00B44700), "rpm");
}

TEST(EsiUnitTest, ADictionaryLocalUnitTypeOverridesTheBuiltInCatalogue) {
  // Synapticon's FSoE dictionaries redefine notation index 0xB6 — "rpm" in ETG.1004 — as "Bit".
  // A parser that ignored <UnitTypes> would label a bit count as a speed.
  const std::vector<UnitType> local{
      UnitType{.notationIndex = 0xB5, .index = 0x4B5, .name = "Increments", .symbol = "Inc"},
      UnitType{.notationIndex = 0xB6, .index = 0x4B6, .name = "Bits", .symbol = "Bit"},
  };

  EXPECT_EQ(esiUnitSymbol(0x00B60000, local), "Bit");
  EXPECT_EQ(esiUnitSymbol(0x00B60000), "rpm");  // Same value, no local table.

  // An index the local table does not mention still falls through to the built-in catalogue.
  EXPECT_EQ(esiUnitSymbol(0xFD260000, local), "mV");
}

TEST(EsiUnitTest, ALocalOverrideSuppressesTheRpmSpecialCase) {
  // The "Revolution/min" collapse is a rendering convenience for the standard composition. Once a
  // dictionary has redefined one of those bytes, the composition means whatever that dictionary
  // says and the shortcut would be a lie.
  const std::vector<UnitType> local{
      UnitType{.notationIndex = 0xB4, .index = 0x4B4, .name = "Widgets", .symbol = "Wg"},
  };
  EXPECT_EQ(esiUnitSymbol(0x00B44700, local), "Wg/min");
}

TEST(EsiUnitTest, AnUnknownNotationYieldsNothingRatherThanGarbage) {
  // 0x99 is not in the catalogue. Emitting a placeholder would put a fake unit on a UI; an empty
  // symbol lets the caller show the raw code instead.
  EXPECT_EQ(esiUnitSymbol(0x00990000), "");
  EXPECT_EQ(esiUnitSymbol(0xFF999900), "");
}

}  // namespace
}  // namespace mm::etg
