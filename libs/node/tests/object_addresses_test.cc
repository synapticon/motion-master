#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

#include "node/cia402.h"
#include "node/cia402_drive_objects.h"
#include "node/device_parameter.h"
#include "node/profile_device_objects.h"
#include "node/somanet_drive_objects.h"

namespace {

using mm::node::ObjectAddress;

// Including all three headers is most of what this file is for: 826 generated declarations either
// compile or they do not, and nothing else in the tree includes them yet.

// The addresses that motivated the whole thing. Getting 0x2031:01 wrong three times in one sitting
// — index, type and unit — is what a generated constant is supposed to make impossible, so these
// pin all three: the index, the C++ type carried with it, and (by pinning the type) the fact that
// a DINT is not an INTEGER16.
TEST(ObjectAddressesTest, DriveTemperatureCarriesItsRealIndexAndType) {
  constexpr auto kDriveTemp = mm::node::somanet::objects::kDriveTemperatureMeasuredTemperature;
  EXPECT_EQ(kDriveTemp.index, 0x2031);
  EXPECT_EQ(kDriveTemp.subindex, 0x01);
  static_assert(std::is_same_v<decltype(kDriveTemp), const ObjectAddress<int32_t>>,
                "0x2031:01 is a DINT — reading it as int16_t returns nothing at all");

  // Its sibling on the control board, which is a different object and a common mix-up.
  constexpr auto kCoreTemp = mm::node::somanet::objects::kCoreTemperatureMeasuredTemperature;
  EXPECT_EQ(kCoreTemp.index, 0x2030);
  static_assert(std::is_same_v<decltype(kCoreTemp), const ObjectAddress<int32_t>>);
}

// The generated CiA 402 addresses must agree with the hand-written constants in cia402.h. They come
// from different places — one from a vendor's ESI, one from the standard — so a disagreement means
// one of them is wrong.
TEST(ObjectAddressesTest, Cia402AddressesAgreeWithTheHandWrittenConstants) {
  namespace objects = mm::node::cia402::objects;
  EXPECT_EQ(objects::kControlword.index, mm::node::cia402::kControlword);
  EXPECT_EQ(objects::kStatusword.index, mm::node::cia402::kStatusword);
  EXPECT_EQ(objects::kTargetVelocity.index, mm::node::cia402::kTargetVelocity);
  EXPECT_EQ(objects::kTargetPosition.index, mm::node::cia402::kTargetPosition);
  EXPECT_EQ(objects::kModesOfOperation.index, mm::node::cia402::kModeOfOperation);
  EXPECT_EQ(objects::kErrorCode.index, mm::node::cia402::kErrorCode);

  // And the types the profile mandates.
  static_assert(std::is_same_v<decltype(objects::kControlword), const ObjectAddress<uint16_t>>);
  static_assert(std::is_same_v<decltype(objects::kStatusword), const ObjectAddress<uint16_t>>);
  static_assert(std::is_same_v<decltype(objects::kTargetVelocity), const ObjectAddress<int32_t>>);
  static_assert(std::is_same_v<decltype(objects::kTargetPosition), const ObjectAddress<int32_t>>);
}

// The width cross-check, end to end: these objects declare a BYTE element type but are 6, 8 and 25
// bytes wide. Typed from the code alone they would each be a uint8_t, and every read would return
// their first byte.
TEST(ObjectAddressesTest, ByteArraysAreNotTypedAsSingleBytes) {
  static_assert(std::is_same_v<decltype(mm::node::profile::objects::kOSCommandCommand),
                               const ObjectAddress<std::vector<uint8_t>>>,
                "0x1023:01 is an 8-byte command, not a byte");
  static_assert(std::is_same_v<decltype(mm::node::profile::objects::kOSCommandResponse),
                               const ObjectAddress<std::vector<uint8_t>>>);
  EXPECT_EQ(mm::node::profile::objects::kOSCommandCommand.index, 0x1023);
  EXPECT_EQ(mm::node::profile::objects::kOSCommandCommand.subindex, 0x01);
}

// Strings get an address like anything else — they are simply unreadable from a cycle, which is a
// compile error at the call site rather than a missing constant.
TEST(ObjectAddressesTest, StringObjectsAreTypedAsStrings) {
  static_assert(std::is_same_v<decltype(mm::node::profile::objects::kManufacturerDeviceName),
                               const ObjectAddress<std::string>>);
  EXPECT_EQ(mm::node::profile::objects::kManufacturerDeviceName.index, 0x1008);
}

// Subindex 0 of a composite is the entry-count field, not a value, and is named for it. Without
// that rule it would collide with whichever subindex repeats the object's own name.
TEST(ObjectAddressesTest, CompositeSubindexZeroIsNamedAsACount) {
  constexpr auto kCount = mm::node::somanet::objects::kDriveTemperatureCount;
  EXPECT_EQ(kCount.index, 0x2031);
  EXPECT_EQ(kCount.subindex, 0x00);
  static_assert(std::is_same_v<decltype(kCount), const ObjectAddress<uint8_t>>);
}

}  // namespace
