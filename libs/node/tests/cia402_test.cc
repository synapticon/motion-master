#include "node/cia402.h"

#include <gtest/gtest.h>

namespace {

using mm::node::cia402::decodeState;
using mm::node::cia402::isFaulted;
using mm::node::cia402::OperationMode;
using mm::node::cia402::State;
using mm::node::cia402::toString;

// The canonical statusword bit patterns for each state (the values a drive reports). decodeState
// must classify these and, crucially, must still classify them correctly with the don't-care
// bits (warning bit 7, voltage-enabled bit 4, manufacturer bits 8+) set in any combination.
TEST(Cia402DecodeState, RecognisesCanonicalPatterns) {
  EXPECT_EQ(decodeState(0x0000), State::kNotReadyToSwitchOn);
  EXPECT_EQ(decodeState(0x0040), State::kSwitchOnDisabled);
  EXPECT_EQ(decodeState(0x0021), State::kReadyToSwitchOn);
  EXPECT_EQ(decodeState(0x0023), State::kSwitchedOn);
  EXPECT_EQ(decodeState(0x0027), State::kOperationEnabled);
  EXPECT_EQ(decodeState(0x0007), State::kQuickStopActive);
  EXPECT_EQ(decodeState(0x000F), State::kFaultReactionActive);
  EXPECT_EQ(decodeState(0x0008), State::kFault);
}

TEST(Cia402DecodeState, IgnoresDontCareBits) {
  // Operation enabled is (sw & 0x6F) == 0x27; setting warning (bit 7), voltage (already in 0x27),
  // and manufacturer-specific bits (0xF000) must not change the classification.
  EXPECT_EQ(decodeState(0x0027 | 0x0080 | 0xF000), State::kOperationEnabled);
  // Switch-on disabled is (sw & 0x4F) == 0x40 — quick-stop bit 5 is a don't-care here.
  EXPECT_EQ(decodeState(0x0040 | 0x0020), State::kSwitchOnDisabled);
  // A real SOMANET statusword in OP with warning + internal-limit-active bits set.
  EXPECT_EQ(decodeState(0x1237 & ~0x0008), State::kOperationEnabled);
}

TEST(Cia402DecodeState, FaultPredicate) {
  EXPECT_TRUE(isFaulted(0x0008));
  EXPECT_TRUE(isFaulted(0x000F));
  EXPECT_FALSE(isFaulted(0x0027));
  EXPECT_FALSE(isFaulted(0x0040));
}

TEST(Cia402ToString, StatesAndModes) {
  EXPECT_EQ(toString(State::kOperationEnabled), "OperationEnabled");
  EXPECT_EQ(toString(State::kSwitchOnDisabled), "SwitchOnDisabled");
  EXPECT_EQ(toString(OperationMode::kCyclicSyncPosition), "CyclicSyncPosition");
  EXPECT_EQ(toString(OperationMode::kProfileTorque), "ProfileTorque");
}

TEST(Cia402ToOperationMode, AcceptsKnownRejectsUnknown) {
  using mm::node::cia402::toOperationMode;
  EXPECT_EQ(toOperationMode(0), OperationMode::kNoMode);
  EXPECT_EQ(toOperationMode(1), OperationMode::kProfilePosition);
  EXPECT_EQ(toOperationMode(3), OperationMode::kProfileVelocity);
  EXPECT_EQ(toOperationMode(4), OperationMode::kProfileTorque);
  EXPECT_EQ(toOperationMode(6), OperationMode::kHoming);
  EXPECT_EQ(toOperationMode(8), OperationMode::kCyclicSyncPosition);
  EXPECT_EQ(toOperationMode(9), OperationMode::kCyclicSyncVelocity);
  EXPECT_EQ(toOperationMode(10), OperationMode::kCyclicSyncTorque);
  // Unassigned / out-of-range values are rejected so an API boundary can 400 them.
  EXPECT_FALSE(toOperationMode(2).has_value());  // "reserved" in the profile
  EXPECT_FALSE(toOperationMode(7).has_value());  // interpolated position — not modelled
  EXPECT_FALSE(toOperationMode(11).has_value());
  EXPECT_FALSE(toOperationMode(200).has_value());  // beyond INT8 range of the enum
  // Out-of-INT8-range values must be rejected before the narrowing cast — otherwise 264 (0x108)
  // would alias to 8 (CSP) and slip past validation.
  EXPECT_FALSE(toOperationMode(264).has_value());  // ≡ 8 mod 256, but out of INT8 range
  EXPECT_FALSE(toOperationMode(256).has_value());  // ≡ 0 mod 256
  EXPECT_FALSE(toOperationMode(-56).has_value());  // in-range but not a mode
}

}  // namespace
