#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "comm/fieldbus_driver.h"

namespace mm::comm {
namespace {

TEST(FrameStatsTest, SerialisesEveryCountWithCamelCaseKeys) {
  FrameStats stats;
  stats.returned = 812;
  stats.over2Ms = 3;
  stats.over20Ms = 1;
  stats.over100Ms = 0;
  stats.lost = 2;
  stats.latestReturnUs = 23410;
  stats.mailbox.push_back(
      MailboxFrameCounts{.slavePosition = 1, .writesWithoutFrame = 2, .counter0Requests = 1});

  const nlohmann::json j = stats;
  EXPECT_EQ(j["returned"], 812);
  EXPECT_EQ(j["over2Ms"], 3);
  EXPECT_EQ(j["over20Ms"], 1);
  EXPECT_EQ(j["over100Ms"], 0);
  EXPECT_EQ(j["lost"], 2);
  EXPECT_EQ(j["latestReturnUs"], 23410);
  ASSERT_EQ(j["mailbox"].size(), 1u);
  EXPECT_EQ(j["mailbox"][0]["slavePosition"], 1);
  EXPECT_EQ(j["mailbox"][0]["writesWithoutFrame"], 2);
  EXPECT_EQ(j["mailbox"][0]["counter0Requests"], 1);
}

TEST(FrameStatsTest, AnEmptyMailboxListIsAnArray) {
  const nlohmann::json j = FrameStats{};
  EXPECT_TRUE(j["mailbox"].is_array());
  EXPECT_EQ(j["mailbox"].size(), 0u);
}

}  // namespace
}  // namespace mm::comm
