#include "node/device_parameter.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using mm::node::decodeSdoBytes;
using mm::node::defaultValueForDataType;
using mm::node::DeviceParameter;
using mm::node::DeviceParameterValue;
using mm::node::encodeSdoBytes;
using mm::node::isScalarDataType;
using mm::node::numericValue;
using mm::node::packLeBits;
using mm::node::scalarByteWidth;
using mm::node::SyncState;
using mm::node::unpackLeBits;

// ETG.1020 data type codes used across the tests.
constexpr uint16_t kU8 = 0x0005;
constexpr uint16_t kU16 = 0x0006;
constexpr uint16_t kU32 = 0x0007;
constexpr uint16_t kI16 = 0x0003;
constexpr uint16_t kI32 = 0x0004;
constexpr uint16_t kReal32 = 0x0008;
constexpr uint16_t kReal64 = 0x0011;
constexpr uint16_t kVisibleString = 0x0009;

// Builds a parameter with the given type and value (other fields value-initialised).
// Avoids partial aggregate initialisation, which -Wmissing-field-initializers rejects.
DeviceParameter param(uint16_t dataType, const DeviceParameterValue& value) {
  DeviceParameter p{};
  p.dataType = dataType;
  // Through the typed setter, so the fixture exercises the same coerce-and-encode path production
  // does rather than reaching into storage.
  (void)p.setValue(value);
  return p;
}

// --- encode / decode symmetry -----------------------------------------------

// Asserts a value survives a decode(encode(v)) round-trip for the given data type.
void expectRoundTrip(uint16_t dataType, const DeviceParameterValue& value) {
  auto bytes = encodeSdoBytes(dataType, value);
  ASSERT_TRUE(bytes.has_value());
  auto decoded = decodeSdoBytes(dataType, *bytes);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, value);
}

// Every supported scalar must survive a decode(encode(v)) round-trip.
TEST(EncodeSdoBytes, ScalarRoundTrips) {
  expectRoundTrip(kU8, DeviceParameterValue{uint8_t{0xAB}});
  expectRoundTrip(kU16, DeviceParameterValue{uint16_t{0x1234}});
  expectRoundTrip(kU32, DeviceParameterValue{uint32_t{0xDEADBEEF}});
  expectRoundTrip(kI16, DeviceParameterValue{int16_t{-1234}});
  expectRoundTrip(kI32, DeviceParameterValue{int32_t{-2000000000}});
  expectRoundTrip(kReal32, DeviceParameterValue{3.5F});
  expectRoundTrip(kReal64, DeviceParameterValue{-1234.5});
}

TEST(EncodeSdoBytes, StringHasNoTrailingNul) {
  DeviceParameterValue v{std::string{"hello"}};
  auto bytes = encodeSdoBytes(kVisibleString, v);
  ASSERT_TRUE(bytes.has_value());
  EXPECT_EQ(bytes->size(), 5u);  // no trailing '\0' — symmetric with decode
  auto decoded = decodeSdoBytes(kVisibleString, *bytes);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, v);
}

TEST(EncodeSdoBytes, RejectsMismatchedAlternative) {
  // dataType says UNSIGNED32 but the value holds a string.
  auto bytes = encodeSdoBytes(kU32, DeviceParameterValue{std::string{"x"}});
  EXPECT_FALSE(bytes.has_value());
}

// --- numericValue / getValue / numeric --------------------------------------

TEST(NumericValue, ConvertsArithmeticAlternatives) {
  EXPECT_EQ(numericValue(DeviceParameterValue{uint32_t{42}}), 42.0);
  EXPECT_EQ(numericValue(DeviceParameterValue{int16_t{-7}}), -7.0);
  EXPECT_EQ(numericValue(DeviceParameterValue{2.5F}), 2.5);
  EXPECT_FALSE(numericValue(DeviceParameterValue{std::string{"x"}}).has_value());
  EXPECT_FALSE(numericValue(DeviceParameterValue{std::vector<uint8_t>{1, 2}}).has_value());
}

TEST(DeviceParameterGetValue, TypeExactSucceedsAndMismatchFails) {
  DeviceParameter p = param(kU32, DeviceParameterValue{uint32_t{42}});
  auto exact = p.getValue<uint32_t>();
  ASSERT_TRUE(exact.has_value());
  EXPECT_EQ(*exact, 42u);
  // Type-exact: a uint32 parameter does not satisfy getValue<int32_t>().
  EXPECT_FALSE(p.getValue<int32_t>().has_value());
}

TEST(DeviceParameterNumeric, WorksRegardlessOfWidthAndRejectsStrings) {
  DeviceParameter num = param(kU16, DeviceParameterValue{uint16_t{1000}});
  auto n = num.numeric();
  ASSERT_TRUE(n.has_value());
  EXPECT_EQ(*n, 1000.0);

  DeviceParameter str = param(kVisibleString, DeviceParameterValue{std::string{"x"}});
  EXPECT_FALSE(str.numeric().has_value());
}

// --- setValue coercion -------------------------------------------------------

TEST(DeviceParameterSetValue, CoercesIntoDeclaredWidth) {
  DeviceParameter p = param(kU32, DeviceParameterValue{uint32_t{0}});

  // A bare int literal must land in the parameter's declared alternative (uint32),
  // not int32 — this is the "don't worry about the type" guarantee.
  ASSERT_TRUE(p.setValue(5).has_value());
  EXPECT_TRUE(std::holds_alternative<uint32_t>(p.currentValue()));
  EXPECT_EQ(p.getValue<uint32_t>().value(), 5u);

  // A differently-sized integer is coerced too.
  ASSERT_TRUE(p.setValue(uint16_t{7}).has_value());
  EXPECT_TRUE(std::holds_alternative<uint32_t>(p.currentValue()));
  EXPECT_EQ(p.getValue<uint32_t>().value(), 7u);
}

TEST(DeviceParameterSetValue, RejectsCategoryMismatch) {
  DeviceParameter numeric = param(kU32, DeviceParameterValue{uint32_t{0}});
  EXPECT_FALSE(numeric.setValue(std::string{"nope"}).has_value());

  DeviceParameter str = param(kVisibleString, DeviceParameterValue{std::string{}});
  EXPECT_FALSE(str.setValue(5).has_value());
  ASSERT_TRUE(str.setValue(std::string{"ok"}).has_value());
  EXPECT_EQ(str.getValue<std::string>().value(), "ok");
}

// --- equality (exact-type) ---------------------------------------------------

TEST(DeviceParameterValueEquality, IsExactType) {
  EXPECT_EQ(DeviceParameterValue{uint32_t{5}}, DeviceParameterValue{uint32_t{5}});
  // Same number, different alternative — variant equality is exact-type.
  EXPECT_NE(DeviceParameterValue{int32_t{5}}, DeviceParameterValue{uint32_t{5}});
}

// --- a value that does not fit the data type ---------------------------------

// Each integer type with its code, checked at both limits and one past each. The incoming value is
// the widest of its kind, as the HTTP layer passes it.
template <typename T>
void expectIntegerLimits(uint16_t dataType) {
  constexpr auto lo = std::numeric_limits<T>::min();
  constexpr auto hi = std::numeric_limits<T>::max();
  DeviceParameter p = param(dataType, DeviceParameterValue{T{0}});

  ASSERT_TRUE(p.setValue(DeviceParameterValue{static_cast<int64_t>(lo)}).has_value());
  EXPECT_EQ(p.currentValue(), DeviceParameterValue{lo});
  ASSERT_TRUE(p.setValue(DeviceParameterValue{static_cast<uint64_t>(hi)}).has_value());
  EXPECT_EQ(p.currentValue(), DeviceParameterValue{hi});

  if constexpr (std::is_signed_v<T> && sizeof(T) < sizeof(int64_t)) {
    EXPECT_FALSE(p.setValue(DeviceParameterValue{int64_t{lo} - 1}).has_value());
  }
  if constexpr (std::is_unsigned_v<T>) {
    EXPECT_FALSE(p.setValue(DeviceParameterValue{int64_t{-1}}).has_value());
  }
  if constexpr (sizeof(T) < sizeof(uint64_t)) {
    EXPECT_FALSE(p.setValue(DeviceParameterValue{uint64_t{hi} + 1}).has_value());
  }
  if constexpr (std::is_same_v<T, int64_t>) {
    EXPECT_FALSE(p.setValue(DeviceParameterValue{uint64_t{hi} + 1}).has_value());
  }
  // A refused value leaves the stored one alone.
  EXPECT_EQ(p.currentValue(), DeviceParameterValue{hi});
}

TEST(DeviceParameterFit, EveryIntegerTypeTakesItsLimitsAndRefusesOnePast) {
  expectIntegerLimits<int8_t>(0x0002);
  expectIntegerLimits<int16_t>(kI16);
  expectIntegerLimits<int32_t>(kI32);
  expectIntegerLimits<int64_t>(0x0015);
  expectIntegerLimits<uint8_t>(kU8);
  expectIntegerLimits<uint16_t>(kU16);
  expectIntegerLimits<uint32_t>(kU32);
  expectIntegerLimits<uint64_t>(0x001B);
}

TEST(DeviceParameterFit, RefusesWhatTheBenchWrapped) {
  // 300 into an UNSIGNED8 used to reach the drive as 44, and -1 as 255.
  DeviceParameter p = param(kU8, DeviceParameterValue{uint8_t{20}});
  const auto refused = p.setValue(DeviceParameterValue{uint64_t{300}});
  ASSERT_FALSE(refused.has_value());
  EXPECT_NE(refused.error().find("UNSIGNED8"), std::string::npos) << refused.error();
  EXPECT_FALSE(p.setValue(DeviceParameterValue{int64_t{-1}}).has_value());
  EXPECT_EQ(p.currentValue(), DeviceParameterValue{uint8_t{20}});
}

TEST(DeviceParameterFit, AnIntegerTypeTakesAWholeRealAndRefusesAFraction) {
  DeviceParameter p = param(kI32, DeviceParameterValue{int32_t{0}});
  ASSERT_TRUE(p.setValue(DeviceParameterValue{-2.0}).has_value());
  EXPECT_EQ(p.currentValue(), DeviceParameterValue{int32_t{-2}});
  EXPECT_FALSE(p.setValue(DeviceParameterValue{1.7}).has_value());
  EXPECT_FALSE(p.setValue(DeviceParameterValue{std::numeric_limits<double>::quiet_NaN()}));
  EXPECT_FALSE(p.setValue(DeviceParameterValue{std::numeric_limits<double>::infinity()}));
  EXPECT_FALSE(p.setValue(DeviceParameterValue{2147483648.0}).has_value());
  ASSERT_TRUE(p.setValue(DeviceParameterValue{2147483647.0}).has_value());
  ASSERT_TRUE(p.setValue(DeviceParameterValue{-2147483648.0}).has_value());
  EXPECT_FALSE(p.setValue(DeviceParameterValue{-2147483649.0}).has_value());

  // The largest whole double below 2^64 fits an UNSIGNED64, and 2^64 does not.
  DeviceParameter u64 = param(0x001B, DeviceParameterValue{uint64_t{0}});
  EXPECT_TRUE(u64.setValue(DeviceParameterValue{std::nextafter(18446744073709551616.0, 0.0)}));
  EXPECT_FALSE(u64.setValue(DeviceParameterValue{18446744073709551616.0}).has_value());
}

TEST(DeviceParameterFit, Real32RefusesAFiniteValueItCannotHold) {
  DeviceParameter p = param(kReal32, DeviceParameterValue{0.0F});
  EXPECT_FALSE(p.setValue(DeviceParameterValue{1e39}).has_value());
  EXPECT_FALSE(p.setValue(DeviceParameterValue{-1e39}).has_value());
  ASSERT_TRUE(
      p.setValue(DeviceParameterValue{static_cast<double>(std::numeric_limits<float>::max())})
          .has_value());
  // REAL32 carries infinity and NaN, so they pass as themselves.
  ASSERT_TRUE(p.setValue(DeviceParameterValue{std::numeric_limits<double>::infinity()}));
  EXPECT_TRUE(std::isinf(std::get<float>(p.currentValue())));
  ASSERT_TRUE(p.setValue(DeviceParameterValue{std::numeric_limits<double>::quiet_NaN()}));
  EXPECT_TRUE(std::isnan(std::get<float>(p.currentValue())));
  // Every integer fits, with rounding.
  EXPECT_TRUE(p.setValue(DeviceParameterValue{std::numeric_limits<uint64_t>::max()}));
}

TEST(DeviceParameterFit, Real64TakesAnything) {
  DeviceParameter p = param(kReal64, DeviceParameterValue{0.0});
  EXPECT_TRUE(p.setValue(DeviceParameterValue{1e300}));
  EXPECT_TRUE(p.setValue(DeviceParameterValue{std::numeric_limits<int64_t>::min()}));
}

TEST(DeviceParameterFit, TheDeclaredRangeIsNotChecked) {
  // The drive decides whether a value is valid, so a caller can send one it will refuse.
  DeviceParameter p = param(kI32, DeviceParameterValue{int32_t{0}});
  p.minValue = DeviceParameterValue{int32_t{0}};
  p.maxValue = DeviceParameterValue{int32_t{100}};
  EXPECT_TRUE(p.setValue(DeviceParameterValue{int64_t{150}}));
  EXPECT_TRUE(p.setValue(DeviceParameterValue{int64_t{-5}}));
}

// --- default sync state ------------------------------------------------------

TEST(DeviceParameter, DefaultsToUnknownSyncState) {
  DeviceParameter p{};
  EXPECT_EQ(p.syncState, SyncState::Unknown);
}

// --- value storage: the cell -------------------------------------------------

// Which field holds a value must agree with which alternative defaultValueForDataType reports, or
// currentValue() would look in the wrong place. Checked over the whole table rather than a sample.
TEST(DeviceParameterStorage, ScalarClassificationMatchesTheDefaultValueTable) {
  for (uint32_t dataType = 0; dataType <= 0x0030; ++dataType) {
    const DeviceParameterValue zero = defaultValueForDataType(static_cast<uint16_t>(dataType));
    const bool arithmetic = !std::holds_alternative<std::string>(zero) &&
                            !std::holds_alternative<std::vector<uint8_t>>(zero);
    EXPECT_EQ(isScalarDataType(static_cast<uint16_t>(dataType)), arithmetic)
        << "data type 0x" << std::hex << dataType;
  }
}

TEST(DeviceParameterStorage, PackAndUnpackRoundTripLittleEndian) {
  const std::vector<uint8_t> bytes = {0x44, 0x33, 0x22, 0x11};
  EXPECT_EQ(packLeBits(bytes), 0x11223344ULL);

  std::array<uint8_t, 8> out{};
  unpackLeBits(0x11223344ULL, out);
  EXPECT_EQ(out, (std::array<uint8_t, 8>{0x44, 0x33, 0x22, 0x11, 0, 0, 0, 0}));

  // More than eight bytes cannot fit the cell: the excess is dropped, not wrapped.
  const std::vector<uint8_t> tooMany(12, 0xFF);
  EXPECT_EQ(packLeBits(tooMany), 0xFFFFFFFFFFFFFFFFULL);
}

// The cell holds wire bytes and currentValue() rebuilds the variant from them, so a value must
// survive the round trip for every width — including the signed and floating-point types, where a
// naive reinterpretation of the cell would be wrong.
TEST(DeviceParameterStorage, EveryScalarWidthRoundTripsThroughTheCell) {
  EXPECT_EQ(param(kU8, DeviceParameterValue{uint8_t{0xAB}}).currentValue(),
            DeviceParameterValue{uint8_t{0xAB}});
  EXPECT_EQ(param(kI16, DeviceParameterValue{int16_t{-1234}}).currentValue(),
            DeviceParameterValue{int16_t{-1234}});
  EXPECT_EQ(param(kU32, DeviceParameterValue{uint32_t{0xDEADBEEF}}).currentValue(),
            DeviceParameterValue{uint32_t{0xDEADBEEF}});
  EXPECT_EQ(param(kI32, DeviceParameterValue{int32_t{-1}}).currentValue(),
            DeviceParameterValue{int32_t{-1}});
  EXPECT_EQ(param(kReal32, DeviceParameterValue{-12.5F}).currentValue(),
            DeviceParameterValue{-12.5F});
  EXPECT_EQ(param(kReal64, DeviceParameterValue{2.718281828}).currentValue(),
            DeviceParameterValue{2.718281828});
}

// A never-written parameter reads as its type's zero rather than as an error or empty optional —
// callers std::visit the result directly.
TEST(DeviceParameterStorage, UnwrittenParameterReadsAsTheTypeZero) {
  DeviceParameter scalar{};
  scalar.dataType = kI32;
  EXPECT_EQ(scalar.currentValue(), DeviceParameterValue{int32_t{0}});

  DeviceParameter text{};
  text.dataType = kVisibleString;
  EXPECT_EQ(text.currentValue(), DeviceParameterValue{std::string{}});
}

// setRawValue is the storage-level setter: it takes the bytes a transfer produced, with no
// coercion and no syncState change.
TEST(DeviceParameterStorage, SetRawValueStoresWireBytesForBothKinds) {
  DeviceParameter scalar{};
  scalar.dataType = kU32;
  const std::vector<uint8_t> le = {0x44, 0x33, 0x22, 0x11};
  scalar.setRawValue(le);
  EXPECT_EQ(scalar.loadBits(), 0x11223344ULL);
  EXPECT_EQ(scalar.currentValue(), DeviceParameterValue{uint32_t{0x11223344}});
  EXPECT_EQ(scalar.syncState, SyncState::Unknown);  // storage only — freshness is the caller's

  DeviceParameter text{};
  text.dataType = kVisibleString;
  const std::vector<uint8_t> hello = {'h', 'i', ' ', 0x00};  // padded, as a slave answers
  text.setRawValue(hello);
  EXPECT_EQ(text.rawValue, hello);                                          // stored verbatim
  EXPECT_EQ(text.currentValue(), DeviceParameterValue{std::string{"hi"}});  // padding stripped
}

// The RT read: type-exact, and a mismatch reads as nothing rather than as a wrong number.
TEST(DeviceParameterStorage, ScalarReadsTheCellTypeExactly) {
  DeviceParameter p = param(kI32, DeviceParameterValue{int32_t{-77}});
  EXPECT_EQ(p.scalar<int32_t>(), std::optional<int32_t>{-77});
  EXPECT_EQ(p.scalar<uint32_t>(), std::nullopt);  // right width, wrong signedness
  EXPECT_EQ(p.scalar<int16_t>(), std::nullopt);   // right family, wrong width

  // A string parameter has no scalar reading at all.
  DeviceParameter text = param(kVisibleString, DeviceParameterValue{std::string{"hi"}});
  EXPECT_EQ(text.scalar<int32_t>(), std::nullopt);

  // Never written reads as zero, not nullopt — a control loop wants a number it can act on, and
  // "never read" is what syncState records.
  DeviceParameter fresh{};
  fresh.dataType = kI32;
  EXPECT_EQ(fresh.scalar<int32_t>(), std::optional<int32_t>{0});
  EXPECT_EQ(fresh.syncState, SyncState::Unknown);
}

// rawValueBytes is the inverse of setRawValue and must return the object's declared width — not
// the eight bytes of the cell, and not bitLength, which describes the process-image slot.
TEST(DeviceParameterStorage, RawValueBytesReturnsTheDeclaredWidth) {
  DeviceParameter p = param(kU16, DeviceParameterValue{uint16_t{0x1234}});
  p.bitLength = 4;  // deliberately inconsistent: the wire width comes from dataType
  EXPECT_EQ(p.rawValueBytes(), (std::vector<uint8_t>{0x34, 0x12}));

  DeviceParameter wide = param(kReal64, DeviceParameterValue{1.0});
  EXPECT_EQ(wide.rawValueBytes().size(), 8u);

  // Round trip against the storage setter, for both kinds.
  DeviceParameter scalar{};
  scalar.dataType = kU32;
  const std::vector<uint8_t> le = {0x44, 0x33, 0x22, 0x11};
  scalar.setRawValue(le);
  EXPECT_EQ(scalar.rawValueBytes(), le);

  DeviceParameter text{};
  text.dataType = kVisibleString;
  const std::vector<uint8_t> padded = {'h', 'i', 0x00, 0x00};
  text.setRawValue(padded);
  // Verbatim, padding included — unlike currentValue(), which strips it for display.
  EXPECT_EQ(text.rawValueBytes(), padded);
  EXPECT_EQ(text.currentValue(), DeviceParameterValue{std::string{"hi"}});
}

TEST(DeviceParameterStorage, ScalarByteWidthMatchesTheDeclaredType) {
  EXPECT_EQ(scalarByteWidth(kU8), 1u);
  EXPECT_EQ(scalarByteWidth(kI16), 2u);
  EXPECT_EQ(scalarByteWidth(kU32), 4u);
  EXPECT_EQ(scalarByteWidth(kReal32), 4u);
  EXPECT_EQ(scalarByteWidth(kReal64), 8u);
  EXPECT_EQ(scalarByteWidth(kVisibleString), 0u);  // no fixed width
}

// The point of the cell: copies are compiler-generated, so a field added later is copied without
// anyone remembering to. A copy must carry the value, not share or drop it.
TEST(DeviceParameterStorage, CopiesCarryTheCellIndependently) {
  DeviceParameter original = param(kU32, DeviceParameterValue{uint32_t{7}});
  DeviceParameter copy = original;
  EXPECT_EQ(copy.currentValue(), DeviceParameterValue{uint32_t{7}});

  ASSERT_TRUE(original.setValue(uint32_t{9}).has_value());
  EXPECT_EQ(original.currentValue(), DeviceParameterValue{uint32_t{9}});
  EXPECT_EQ(copy.currentValue(), DeviceParameterValue{uint32_t{7}});  // snapshot, not a view
}

// The ESI fields reach a client only through this serialisation, and are left out when empty.
TEST(DeviceParameterJson, CarriesTheEsiFieldsOnlyWhenTheyAreSet) {
  DeviceParameter p;
  p.index = 0x6060;
  p.dataType = 0x0002;  // INTEGER8
  p.bitLength = 8;
  p.unit = 0x00B44700;
  p.unitSymbol = "rpm";
  p.description = "<p>Modes of operation</p>";
  p.options = {{.value = 8, .label = "CSP"}, {.value = 9, .label = "CSV"}};
  p.esiFile = "esi-000022d2-v5.6.11.xml";

  const nlohmann::json j = p;

  EXPECT_EQ(j.at("unitSymbol"), "rpm");
  EXPECT_EQ(j.at("description"), "<p>Modes of operation</p>");
  EXPECT_EQ(j.at("options"),
            nlohmann::json::parse(R"([{"value":8,"label":"CSP"},{"value":9,"label":"CSV"}])"));
  EXPECT_EQ(j.at("esiFile"), "esi-000022d2-v5.6.11.xml");

  const nlohmann::json bare = DeviceParameter{};
  for (const char* key : {"unitSymbol", "description", "options", "esiFile"}) {
    EXPECT_FALSE(bare.contains(key)) << key;
  }
}

}  // namespace
