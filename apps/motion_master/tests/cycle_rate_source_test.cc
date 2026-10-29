#include "cycle_rate_source.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace {

constexpr double kTarget = 1000.0;

// Feeds @p windows windows of a second, each with @p skippedPerWindow of 1000 grid cycles
// skipped, and returns the last line the watch gave, or an empty string.
std::string feed(mm::CycleRateWatch& watch, uint64_t& executed, uint64_t& skipped, int windows,
                 uint64_t skippedPerWindow) {
  std::string last;
  for (int i = 0; i < windows; ++i) {
    executed += 1000 - skippedPerWindow;
    skipped += skippedPerWindow;
    if (auto line = watch.observe(executed, skipped, kTarget)) {
      last = *line;
    }
  }
  return last;
}

TEST(CycleRateWatch, WarnsAfterThreeLowSecondsAndSaysWhenTheRateIsBack) {
  mm::CycleRateWatch watch;
  uint64_t executed = 0;
  uint64_t skipped = 0;
  EXPECT_FALSE(watch.observe(executed, skipped, kTarget));  // the baseline

  // Two low seconds say nothing; the third warns, with the rate the loop ran.
  EXPECT_EQ(feed(watch, executed, skipped, 2, 900), "");
  const auto warning = feed(watch, executed, skipped, 1, 900);
  EXPECT_NE(warning.find("skipped 90 %"), std::string::npos) << warning;
  EXPECT_NE(warning.find("about 100 Hz of 1000 Hz"), std::string::npos) << warning;
  EXPECT_TRUE(watch.low());

  // Still low: no repeat.
  EXPECT_EQ(feed(watch, executed, skipped, 5, 900), "");

  // Three good seconds say it is back.
  EXPECT_EQ(feed(watch, executed, skipped, 2, 0), "");
  const auto back = feed(watch, executed, skipped, 1, 0);
  EXPECT_NE(back.find("about 1000 Hz of 1000 Hz again"), std::string::npos) << back;
  EXPECT_FALSE(watch.low());
}

TEST(CycleRateWatch, IgnoresABurstAndARateAtTheLimit) {
  mm::CycleRateWatch watch;
  uint64_t executed = 0;
  uint64_t skipped = 0;
  watch.observe(executed, skipped, kTarget);
  // Two low seconds between good ones never make three in a row.
  EXPECT_EQ(feed(watch, executed, skipped, 2, 500), "");
  EXPECT_EQ(feed(watch, executed, skipped, 1, 0), "");
  EXPECT_EQ(feed(watch, executed, skipped, 2, 500), "");
  // Exactly 10 % is not low.
  EXPECT_EQ(feed(watch, executed, skipped, 10, 100), "");
}

TEST(CycleRateWatch, TakesACounterResetAsANewBaseline) {
  mm::CycleRateWatch watch;
  uint64_t executed = 0;
  uint64_t skipped = 0;
  watch.observe(executed, skipped, kTarget);
  feed(watch, executed, skipped, 2, 900);
  // A period change sets the counts to zero: no judgement, and the streak starts again.
  executed = 0;
  skipped = 0;
  EXPECT_FALSE(watch.observe(executed, skipped, kTarget));
  EXPECT_EQ(feed(watch, executed, skipped, 2, 900), "");
  EXPECT_NE(feed(watch, executed, skipped, 1, 900), "");
}

}  // namespace
