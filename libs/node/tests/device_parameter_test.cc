#include "node/device_parameter.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
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

// As param(), additionally setting the slave-reported min/max bounds.
DeviceParameter paramWithBounds(uint16_t dataType, DeviceParameterValue value,
                                DeviceParameterValue lo, DeviceParameterValue hi) {
  DeviceParameter p = param(dataType, std::move(value));
  p.minValue = std::move(lo);
  p.maxValue = std::move(hi);
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

// --- bounds: inRange / clampToRange ------------------------------------------

TEST(DeviceParameterBounds, InRangeRespectsMinMax) {
  DeviceParameter p =
      paramWithBounds(kI32, DeviceParameterValue{int32_t{0}}, DeviceParameterValue{int32_t{0}},
                      DeviceParameterValue{int32_t{100}});
  EXPECT_TRUE(p.inRange(DeviceParameterValue{int32_t{50}}));
  EXPECT_TRUE(p.inRange(DeviceParameterValue{int32_t{0}}));
  EXPECT_TRUE(p.inRange(DeviceParameterValue{int32_t{100}}));
  EXPECT_FALSE(p.inRange(DeviceParameterValue{int32_t{150}}));
  EXPECT_FALSE(p.inRange(DeviceParameterValue{int32_t{-5}}));
}

TEST(DeviceParameterBounds, InRangeTrueWhenNoBounds) {
  DeviceParameter p = param(kI32, DeviceParameterValue{int32_t{0}});
  EXPECT_TRUE(p.inRange(DeviceParameterValue{int32_t{999999}}));
}

TEST(DeviceParameterBounds, ClampToRangeClampsAndPreservesType) {
  DeviceParameter p =
      paramWithBounds(kI32, DeviceParameterValue{int32_t{0}}, DeviceParameterValue{int32_t{0}},
                      DeviceParameterValue{int32_t{100}});
  EXPECT_EQ(p.clampToRange(DeviceParameterValue{int32_t{150}}), DeviceParameterValue{int32_t{100}});
  EXPECT_EQ(p.clampToRange(DeviceParameterValue{int32_t{-5}}), DeviceParameterValue{int32_t{0}});
  // Within range: returned unchanged, same alternative.
  EXPECT_EQ(p.clampToRange(DeviceParameterValue{int32_t{50}}), DeviceParameterValue{int32_t{50}});
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

}  // namespace
