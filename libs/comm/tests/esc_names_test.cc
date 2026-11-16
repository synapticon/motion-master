#include "comm/esc_names.h"

#include <gtest/gtest.h>

#include <string_view>
#include <vector>

namespace {

TEST(EscNames, MailboxProtocolsInTheOrderOfTheirBits) {
  const std::vector<std::string_view> coeAndFoe{"CoE", "FoE"};
  const std::vector<std::string_view> all{"AoE", "EoE", "CoE", "FoE", "SoE", "VoE"};
  EXPECT_EQ(mm::comm::mailboxProtocolNames(0x000C), coeAndFoe);
  EXPECT_EQ(mm::comm::mailboxProtocolNames(0x003F), all);
  EXPECT_TRUE(mm::comm::mailboxProtocolNames(0).empty());
}

TEST(EscNames, CoeDetailsInTheOrderOfTheirBits) {
  const std::vector<std::string_view> expected{"SDO", "SDO Info", "Complete Access"};
  EXPECT_EQ(mm::comm::coeDetailNames(0x23), expected);
  EXPECT_TRUE(mm::comm::coeDetailNames(0).empty());
}

TEST(EscNames, SyncManagerTypes) {
  EXPECT_EQ(mm::comm::syncManagerTypeName(1), "Mailbox Out");
  EXPECT_EQ(mm::comm::syncManagerTypeName(4), "Inputs");
  EXPECT_EQ(mm::comm::syncManagerTypeName(6), "Dynamic inputs");
  EXPECT_FALSE(mm::comm::syncManagerTypeName(7).has_value());
}

TEST(EscNames, TheSiiUsageAndTheRegisterTypeNumberTheOtherWayRound) {
  EXPECT_EQ(mm::comm::fmmuUsageName(0x01), "Outputs");
  EXPECT_EQ(mm::comm::fmmuUsageName(0x02), "Inputs");
  EXPECT_EQ(mm::comm::fmmuUsageName(0xFF), "Unused");
  EXPECT_FALSE(mm::comm::fmmuUsageName(0x06).has_value());
  EXPECT_EQ(mm::comm::fmmuTypeName(1, 0x1100), "Inputs");
  EXPECT_EQ(mm::comm::fmmuTypeName(2, 0x1000), "Outputs");
  EXPECT_EQ(mm::comm::fmmuTypeName(3, 0x1000), "Inputs and outputs");
  EXPECT_FALSE(mm::comm::fmmuTypeName(4, 0x1000).has_value());
}

TEST(EscNames, AnFmmuOnTheSyncManagerRegistersMapsAMailboxState) {
  EXPECT_EQ(mm::comm::fmmuTypeName(1, 0x080D), "Mailbox state");
  EXPECT_EQ(mm::comm::fmmuTypeName(1, 0x0880), "Inputs");
}

}  // namespace
