#include "ring_log_sink.h"

#include <gtest/gtest.h>
#include <spdlog/logger.h>

#include <memory>
#include <mutex>

namespace {

// Logs through a RingLogSink with a message-only pattern, so entries() returns exactly the
// messages logged.
std::shared_ptr<mm::RingLogSinkMt> makeSink(std::size_t capacity) {
  auto sink = std::make_shared<mm::RingLogSinkMt>(capacity);
  sink->set_pattern("%v");
  return sink;
}

TEST(RingLogSink, RetainsEntriesInChronologicalOrder) {
  auto sink = makeSink(100);
  spdlog::logger logger("test", sink);
  logger.info("one");
  logger.info("two");
  logger.info("three");

  const auto entries = sink->entries();
  ASSERT_EQ(entries.size(), 3u);
  EXPECT_EQ(entries[0], "one");
  EXPECT_EQ(entries[1], "two");
  EXPECT_EQ(entries[2], "three");
}

TEST(RingLogSink, EvictsOldestOnceFull) {
  auto sink = makeSink(2);
  spdlog::logger logger("test", sink);
  logger.info("a");
  logger.info("b");
  logger.info("c");  // evicts "a"

  const auto entries = sink->entries();
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0], "b");
  EXPECT_EQ(entries[1], "c");
}

}  // namespace
