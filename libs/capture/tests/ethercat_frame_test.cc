#include "capture/ethercat_frame.h"

#include <gtest/gtest.h>

#include "test_frames.h"

namespace mm::capture {
namespace {

TEST(EtherCatFrameTest, ReadsEveryDatagramAndTheDirection) {
  const auto bytes = test::frame({{4, 7, 0x1001, 0x0130, 2, 1}, {10, 7, 0, 0, 4, 3}}, false);
  const auto frame = parseEtherCatFrame(bytes);
  ASSERT_TRUE(frame.has_value());
  EXPECT_FALSE(frame->fromMaster);
  ASSERT_EQ(frame->datagrams.size(), 2u);
  EXPECT_EQ(frame->datagrams[0].command, 4);
  EXPECT_EQ(frame->datagrams[0].index, 7);
  EXPECT_EQ(frame->datagrams[0].adp, 0x1001);
  EXPECT_EQ(frame->datagrams[0].ado, 0x0130);
  EXPECT_EQ(frame->datagrams[0].length, 2);
  EXPECT_TRUE(frame->datagrams[0].more);
  EXPECT_EQ(frame->datagrams[0].wkc, 1);
  EXPECT_FALSE(frame->datagrams[1].more);
  EXPECT_EQ(frame->datagrams[1].wkc, 3);
}

TEST(EtherCatFrameTest, AFrameFromTheMasterHasTheLocalBitClear) {
  const auto frame = parseEtherCatFrame(test::frame({{4, 1, 0x1001, 0x0130, 2}}, true));
  ASSERT_TRUE(frame.has_value());
  EXPECT_TRUE(frame->fromMaster);
}

TEST(EtherCatFrameTest, RefusesAnotherEtherTypeAndAShortFrame) {
  auto bytes = test::frame({{4, 1, 0x1001, 0x0130, 2}}, true);
  bytes[12] = 0x08;
  bytes[13] = 0x00;
  EXPECT_FALSE(parseEtherCatFrame(bytes).has_value());

  auto cut = test::frame({{4, 1, 0x1001, 0x0130, 2}}, true);
  cut.resize(cut.size() - 1);
  EXPECT_FALSE(parseEtherCatFrame(cut).has_value());
}

TEST(EtherCatFrameTest, LogicalCommandsAreTheCyclicFrame) {
  EXPECT_TRUE(isLogicalCommand(10));
  EXPECT_TRUE(isLogicalCommand(11));
  EXPECT_TRUE(isLogicalCommand(12));
  EXPECT_FALSE(isLogicalCommand(4));
  EXPECT_FALSE(isLogicalCommand(5));
}

TEST(EtherCatFrameTest, AutoIncrementAndBroadcastPositionsAreCountedUp) {
  EXPECT_TRUE(isPositionCountedUp(1));
  EXPECT_TRUE(isPositionCountedUp(7));
  EXPECT_TRUE(isPositionCountedUp(13));
  EXPECT_FALSE(isPositionCountedUp(4));
  EXPECT_FALSE(isPositionCountedUp(10));
}

}  // namespace
}  // namespace mm::capture
