#include "node/parameter_refresher.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "comm/sdo_log.h"
#include "node/device_manager.h"

namespace mm::node {

namespace {

// SDO is slow (a mailbox round-trip per object), so polling faster than this wastes control-plane
// bandwidth without giving a monitoring meaningfully fresher data. Requested periods are clamped
// up to this floor.
constexpr std::chrono::milliseconds kMinRefreshPeriod{10};

// A failing object backs off exponentially up to this ceiling, so a faulted device is retried
// occasionally rather than hammered every period (and the logs are not flooded).
constexpr std::chrono::milliseconds kMaxBackoff{2000};

std::chrono::milliseconds nextBackoff(std::chrono::milliseconds current,
                                      std::chrono::milliseconds period) {
  const std::chrono::milliseconds next = current.count() == 0 ? period : current * 2;
  return std::min(next, kMaxBackoff);
}

}  // namespace

ParameterRefresher::ParameterRefresher(DeviceManager& deviceManager)
    : deviceManager_(deviceManager) {}

ParameterRefresher::~ParameterRefresher() { stop(); }

uint64_t ParameterRefresher::makeKey(uint16_t devicePosition, uint16_t index, uint8_t subindex) {
  return (static_cast<uint64_t>(devicePosition) << 24) | (static_cast<uint32_t>(index) << 8) |
         subindex;
}

void ParameterRefresher::acquire(uint16_t devicePosition, uint16_t index, uint8_t subindex,
                                 std::chrono::milliseconds period) {
  period = std::max(period, kMinRefreshPeriod);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t key = makeKey(devicePosition, index, subindex);
    auto it = entries_.find(key);
    if (it == entries_.end()) {
      Entry entry;
      entry.devicePosition = devicePosition;
      entry.index = index;
      entry.subindex = subindex;
      entry.refCount = 1;
      entry.period = period;
      // nextDue defaults to the clock epoch, i.e. due immediately, so a freshly-tracked object
      // is polled on the next wake-up rather than after a full period.
      entries_.emplace(key, entry);
    } else {
      ++it->second.refCount;
      it->second.period = std::min(it->second.period, period);  // honour the shortest requester
    }
  }
  cv_.notify_all();  // wake the thread to pick up the new/tightened entry
}

void ParameterRefresher::release(uint16_t devicePosition, uint16_t index, uint8_t subindex) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(makeKey(devicePosition, index, subindex));
    if (it == entries_.end()) {
      return;
    }
    if (--it->second.refCount <= 0) {
      entries_.erase(it);
    }
  }
  cv_.notify_all();  // wake the thread to recompute the nearest deadline
}

void ParameterRefresher::start() {
  // thread_ is assigned under the lock, not after releasing it. Releasing first leaves a window in
  // which a concurrent stop() sees running_ true, finds thread_ not yet joinable, and returns
  // without joining — after which start() installs a thread nobody will ever join, and destroying a
  // joinable std::thread calls std::terminate. The new thread's first act is to take this mutex, so
  // it simply waits out the rest of this function.
  const std::lock_guard<std::mutex> lock(mutex_);
  if (running_) {
    return;
  }
  running_ = true;
  thread_ = std::thread([this] { run(); });
}

void ParameterRefresher::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
      return;
    }
    running_ = false;
  }
  cv_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

std::size_t ParameterRefresher::trackedCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.size();
}

void ParameterRefresher::pollDue() {
  // Background polling: keep the driver's per-read SDO debug traces out of the log (demoted to
  // trace). A direct, user-initiated read runs without this guard and keeps its debug trace.
  const mm::comm::ScopedQuietSdoLog quietSdoLogging;
  const auto now = std::chrono::steady_clock::now();

  // Snapshot the keys due now, then poll each with the lock released — readDeviceParameter does a
  // mailbox upload and must not block acquire/release/trackedCount. Re-find each entry before and
  // after the poll, since a concurrent release may have erased it in the meantime.
  std::vector<uint64_t> dueKeys;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [key, entry] : entries_) {
      if (entry.nextDue <= now) {
        dueKeys.push_back(key);
      }
    }
  }

  for (const uint64_t key : dueKeys) {
    uint16_t devicePosition = 0;
    uint16_t index = 0;
    uint8_t subindex = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = entries_.find(key);
      if (it == entries_.end()) {
        continue;
      }
      devicePosition = it->second.devicePosition;
      index = it->second.index;
      subindex = it->second.subindex;
    }

    // Resolved here rather than hidden behind a DeviceManager forwarder: the handle is what keeps
    // the device alive across the upload, and a device that a rescan retired simply fails its next
    // transfer.
    std::expected<DeviceParameterValue, std::string> result =
        mm::node::deviceNotFound(devicePosition);
    if (const auto device = deviceManager_.deviceAt(devicePosition)) {
      result = device->readParameter(index, subindex);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end()) {
      continue;
    }
    if (result) {
      it->second.backoff = std::chrono::milliseconds{0};
      it->second.nextDue = now + it->second.period;
    } else {
      it->second.backoff = nextBackoff(it->second.backoff, it->second.period);
      it->second.nextDue = now + it->second.backoff;
    }
  }
}

void ParameterRefresher::run() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (running_) {
    if (entries_.empty()) {
      cv_.wait(lock);  // nothing to poll — sleep until an acquire (or stop) wakes us
      continue;
    }
    auto nearest = std::chrono::steady_clock::time_point::max();
    for (const auto& [key, entry] : entries_) {
      nearest = std::min(nearest, entry.nextDue);
    }
    if (nearest <= std::chrono::steady_clock::now()) {
      lock.unlock();
      pollDue();
      lock.lock();
    } else {
      cv_.wait_until(lock, nearest);  // wake at the deadline, or earlier on acquire/release/stop
    }
  }
}

}  // namespace mm::node
