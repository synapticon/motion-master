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

}  // namespace
