#include "node/device_parameter.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

using mm::node::decodeSdoBytes;
using mm::node::DeviceParameter;
using mm::node::DeviceParameterValue;
using mm::node::encodeSdoBytes;
using mm::node::numericValue;
using mm::node::SyncState;

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
DeviceParameter param(uint16_t dataType, DeviceParameterValue value) {
  DeviceParameter p{};
  p.dataType = dataType;
  p.value = std::move(value);
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
  EXPECT_TRUE(std::holds_alternative<uint32_t>(p.value));
  EXPECT_EQ(p.getValue<uint32_t>().value(), 5u);

  // A differently-sized integer is coerced too.
  ASSERT_TRUE(p.setValue(uint16_t{7}).has_value());
  EXPECT_TRUE(std::holds_alternative<uint32_t>(p.value));
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

}  // namespace
