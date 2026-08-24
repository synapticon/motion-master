#include "node/notification_bus.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using mm::node::NotificationBus;
using std::chrono::milliseconds;
using std::chrono::seconds;

/// Collects what the bus publishes, from whichever thread it publishes on.
class Collector {
 public:
  void operator()(std::string topic, std::string json) {
    const std::lock_guard lock(mutex_);
    topics_.push_back(std::move(topic));
    messages_.push_back(std::move(json));
  }

  std::vector<std::string> messages() const {
    const std::lock_guard lock(mutex_);
    return messages_;
  }

  std::vector<std::string> topics() const {
    const std::lock_guard lock(mutex_);
    return topics_;
  }

  std::size_t count() const {
    const std::lock_guard lock(mutex_);
    return messages_.size();
  }

 private:
  mutable std::mutex mutex_;
  std::vector<std::string> topics_;
  std::vector<std::string> messages_;
};

/// Waits for a predicate rather than for a duration, so a slow machine does not fail the test and a
/// fast one does not pay for the timeout.
template <typename Predicate>
bool waitFor(Predicate predicate, milliseconds timeout = seconds{2}) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(milliseconds{1});
  }
  return predicate();
}

TEST(NotificationBus, PublishesToTheOneNotificationTopicWhenTheRevisionChanges) {
  std::atomic<uint64_t> revision{0};
  Collector collector;

  NotificationBus bus;
  bus.setPublish([&collector](std::string topic, std::string json) {
    collector(std::move(topic), std::move(json));
  });
  bus.addSource({.revision = [&revision] { return revision.load(); },
                 .render = [&revision]() -> std::optional<nlohmann::json> {
                   return nlohmann::json{{"event", "test"}, {"value", revision.load()}};
                 },
                 .interval = milliseconds{1}});
  bus.start();

  revision.store(1);
  ASSERT_TRUE(waitFor([&collector] { return collector.count() == 1; }));
  bus.stop();

  EXPECT_EQ(collector.topics().front(), mm::node::kNotificationTopic);
  const auto message = nlohmann::json::parse(collector.messages().front());
  EXPECT_EQ(message.at("type"), "notification");
  EXPECT_EQ(message.at("data").at("event"), "test");
  EXPECT_EQ(message.at("data").at("value"), 1);
}

TEST(NotificationBus, SaysNothingWhileTheRevisionHoldsStill) {
  Collector collector;

  NotificationBus bus;
  bus.setPublish([&collector](std::string topic, std::string json) {
    collector(std::move(topic), std::move(json));
  });
  bus.addSource({.revision = [] { return uint64_t{7}; },
                 .render = []() -> std::optional<nlohmann::json> {
                   return nlohmann::json{{"event", "test"}};
                 },
                 .interval = milliseconds{1}});
  bus.start();

  // start() seeds the last-seen mark, so a source already at 7 is not news.
  std::this_thread::sleep_for(milliseconds{50});
  bus.stop();

  EXPECT_EQ(collector.count(), 0u);
}

TEST(NotificationBus, CoalescesSeveralBumpsIntoOneMessage) {
  std::atomic<uint64_t> revision{0};
  std::atomic<int> renders{0};
  Collector collector;

  NotificationBus bus;
  bus.setPublish([&collector](std::string topic, std::string json) {
    collector(std::move(topic), std::move(json));
  });
  bus.addSource({.revision = [&revision] { return revision.load(); },
                 .render = [&revision, &renders]() -> std::optional<nlohmann::json> {
                   ++renders;
                   return nlohmann::json{{"event", "test"}, {"value", revision.load()}};
                 },
                 .interval = milliseconds{30}});
  bus.start();

  // Every bump lands inside one interval, so the bus sees one change, not a thousand.
  for (uint64_t i = 1; i <= 1000; ++i) {
    revision.store(i);
  }
  ASSERT_TRUE(waitFor([&collector] { return collector.count() >= 1; }));
  std::this_thread::sleep_for(milliseconds{60});
  bus.stop();

  EXPECT_EQ(collector.count(), 1u);
  EXPECT_EQ(renders.load(), 1);
  // And the one message carries the current state, not the state at the first bump.
  const auto message = nlohmann::json::parse(collector.messages().front());
  EXPECT_EQ(message.at("data").at("value"), 1000);
}

TEST(NotificationBus, RenderingNullOptSuppressesTheMessageAndLeavesTheChangeOutstanding) {
  std::atomic<uint64_t> revision{0};
  std::atomic<bool> speak{false};
  Collector collector;

  NotificationBus bus;
  bus.setPublish([&collector](std::string topic, std::string json) {
    collector(std::move(topic), std::move(json));
  });
  bus.addSource({.revision = [&revision] { return revision.load(); },
                 .render = [&speak]() -> std::optional<nlohmann::json> {
                   if (!speak.load()) {
                     return std::nullopt;
                   }
                   return nlohmann::json{{"event", "test"}};
                 },
                 .interval = milliseconds{1}});
  bus.start();

  revision.store(1);
  std::this_thread::sleep_for(milliseconds{30});
  EXPECT_EQ(collector.count(), 0u);

  // The mark never advanced, so the same change is still pending — no second bump needed.
  speak.store(true);
  ASSERT_TRUE(waitFor([&collector] { return collector.count() == 1; }));
  bus.stop();
}

TEST(NotificationBus, HoldsAHighRateSourceToItsOwnInterval) {
  std::atomic<uint64_t> revision{0};
  Collector collector;

  NotificationBus bus;
  bus.setPublish([&collector](std::string topic, std::string json) {
    collector(std::move(topic), std::move(json));
  });
  bus.addSource({.revision = [&revision] { return revision.load(); },
                 .render = []() -> std::optional<nlohmann::json> {
                   return nlohmann::json{{"event", "slow"}};
                 },
                 .interval = seconds{10}});
  // A second source on a fast cadence, so the loop wakes often and the slow one still stays quiet.
  std::atomic<uint64_t> fast{0};
  bus.addSource({.revision = [&fast] { return fast.load(); },
                 .render = []() -> std::optional<nlohmann::json> {
                   return nlohmann::json{{"event", "fast"}};
                 },
                 .interval = milliseconds{1}});
  bus.start();

  for (uint64_t i = 1; i <= 200; ++i) {
    revision.store(i);
    fast.store(i);
    std::this_thread::sleep_for(std::chrono::microseconds{100});
  }
  // The fast source speaks; the slow one is not due for ten seconds and never does.
  ASSERT_TRUE(waitFor([&collector] { return collector.count() >= 1; }));
  std::this_thread::sleep_for(milliseconds{30});
  bus.stop();

  for (const auto& raw : collector.messages()) {
    EXPECT_EQ(nlohmann::json::parse(raw).at("data").at("event"), "fast");
  }
}

TEST(NotificationBus, RendersWithoutAPublishSeam) {
  std::atomic<uint64_t> revision{0};
  std::atomic<int> renders{0};

  NotificationBus bus;
  bus.addSource({.revision = [&revision] { return revision.load(); },
                 .render = [&renders]() -> std::optional<nlohmann::json> {
                   ++renders;
                   return nlohmann::json{{"event", "test"}};
                 },
                 .interval = milliseconds{1}});
  bus.start();

  revision.store(1);
  ASSERT_TRUE(waitFor([&renders] { return renders.load() == 1; }));
  bus.stop();
}

TEST(NotificationBus, StopsWithoutWaitingOutTheInterval) {
  NotificationBus bus;
  bus.addSource({.revision = [] { return uint64_t{0}; },
                 .render = []() -> std::optional<nlohmann::json> { return std::nullopt; },
                 .interval = seconds{10}});
  bus.start();

  const auto before = std::chrono::steady_clock::now();
  bus.stop();
  EXPECT_LT(std::chrono::steady_clock::now() - before, seconds{1});
}

TEST(NotificationBus, StopIsSafeWhenNothingWasStarted) {
  NotificationBus bus;
  bus.stop();
  bus.stop();
}

TEST(NotificationBus, RunsWithNoSourcesAtAll) {
  NotificationBus bus;
  bus.start();
  bus.stop();
}

}  // namespace
