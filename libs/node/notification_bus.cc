#include "notification_bus.h"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>

namespace mm::node {

NotificationBus::NotificationBus() = default;

NotificationBus::~NotificationBus() { stop(); }

void NotificationBus::setPublish(PublishFn publish) { publish_ = std::move(publish); }

void NotificationBus::addSource(Source source) { sources_.push_back(std::move(source)); }

void NotificationBus::start() {
  if (thread_.joinable()) {
    return;
  }
  lastSeen_.clear();
  lastSeen_.reserve(sources_.size());
  std::transform(sources_.begin(), sources_.end(), std::back_inserter(lastSeen_),
                 [](const Source& source) { return source.revision(); });
  const auto now = std::chrono::steady_clock::now();
  nextDue_.clear();
  nextDue_.reserve(sources_.size());
  std::transform(sources_.begin(), sources_.end(), std::back_inserter(nextDue_),
                 [now](const Source& source) { return now + source.interval; });
  thread_ = std::jthread([this](std::stop_token stopToken) { run(std::move(stopToken)); });
}

void NotificationBus::stop() {
  if (!thread_.joinable()) {
    return;
  }
  thread_.request_stop();
  thread_.join();
}

std::string NotificationBus::envelope(const nlohmann::json& data) {
  return nlohmann::json{{"type", "notification"}, {"data", data}}.dump();
}

std::chrono::steady_clock::time_point NotificationBus::nextDeadline() const {
  const auto earliest = std::min_element(nextDue_.begin(), nextDue_.end());
  if (earliest == nextDue_.end()) {
    // No sources. Wake anyway rather than sleeping forever, so that a stop is not the only thing
    // that can ever end the wait.
    return std::chrono::steady_clock::now() + std::chrono::seconds{1};
  }
  return *earliest;
}

void NotificationBus::run(std::stop_token stopToken) {
  // This thread is the only one that touches the registry, so the mutex guards nothing. It is here
  // because condition_variable_any::wait_for takes a lock, and that overload is what lets a stop
  // request cut the wait short.
  std::mutex mutex;
  std::condition_variable_any cv;
  std::unique_lock lock(mutex);
  // Sleeping to the earliest due source is what keeps each source's cadence its own: a counter
  // read once a second costs one wake a second, not the fifty a lifecycle source beside it needed.
  // The predicate is what distinguishes the two ways out: it returns true only when the stop was
  // requested, so a timeout runs a poll and a stop leaves the loop — and shutdown does not wait out
  // the interval, which a plain sleep would make it do.
  while (!cv.wait_until(lock, stopToken, nextDeadline(),
                        [&stopToken] { return stopToken.stop_requested(); })) {
    const auto now = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < sources_.size(); ++i) {
      const auto& source = sources_[i];
      if (now < nextDue_[i]) {
        continue;  // another source woke us
      }
      // Measured from now rather than from the previous deadline: a poller that fell behind should
      // resume its cadence, not work off a backlog of readings nobody wanted.
      nextDue_[i] = now + source.interval;
      const auto revision = source.revision();
      if (revision == lastSeen_[i]) {
        continue;  // unchanged — a quiet source never speaks
      }
      auto data = source.render();
      // The mark advances only on a message. A render that declines leaves the change outstanding,
      // so a source that goes idle and busy again is still reported.
      if (!data) {
        continue;
      }
      lastSeen_[i] = revision;
      if (publish_) {
        publish_(std::string(kNotificationTopic), envelope(*data));
      }
    }
  }
}

}  // namespace mm::node
