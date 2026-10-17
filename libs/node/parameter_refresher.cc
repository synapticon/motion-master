#include "node/parameter_refresher.h"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <optional>
#include <stop_token>
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

// The number of worker threads. Over SOEM each read holds one of the driver's frame slots
// (ControlPlaneLocks::kFrameSlots) until it ends, so the pool stays below that number and leaves
// slots for the HTTP workers and the procedures.
constexpr std::size_t kWorkerCount = 4;

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
  cv_.notify_all();  // wake the workers to pick up the new/tightened entry
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
  cv_.notify_all();  // wake the workers to recompute the nearest deadline
}

void ParameterRefresher::start() {
  // The workers are created under the lock, so a concurrent stop() either finds none or finds all
  // of them. A new worker's first act is to take this mutex, so it waits out the rest of this
  // function.
  const std::lock_guard<std::mutex> lock(mutex_);
  if (!workers_.empty()) {
    return;
  }
  workers_.reserve(kWorkerCount);
  for (std::size_t i = 0; i < kWorkerCount; ++i) {
    workers_.emplace_back([this](const std::stop_token& stopToken) { work(stopToken); });
  }
}

void ParameterRefresher::stop() {
  std::vector<std::jthread> workers;
  {
    // The stop is requested under the lock that a worker holds while it checks its token and
    // starts to wait. So a worker either sees the request or is already waiting for the notify.
    const std::lock_guard<std::mutex> lock(mutex_);
    for (auto& worker : workers_) {
      worker.request_stop();
    }
    workers.swap(workers_);
  }
  cv_.notify_all();
  // Joined outside the lock, because a worker takes it once more after its read.
  for (auto& worker : workers) {
    worker.join();
  }
}

std::size_t ParameterRefresher::trackedCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.size();
}

std::optional<ParameterRefresher::Job> ParameterRefresher::takeDue(
    std::chrono::steady_clock::time_point now, std::chrono::steady_clock::time_point& wakeAt) {
  auto earliest = std::chrono::steady_clock::time_point::max();
  auto chosen = entries_.end();
  for (auto it = entries_.begin(); it != entries_.end(); ++it) {
    auto dueAt = it->second.nextDue;
    if (const auto drive = drives_.find(it->second.devicePosition); drive != drives_.end()) {
      // A drive that a worker reads now is skipped. The worker wakes the others when it is done.
      if (drive->second.busy) {
        continue;
      }
      dueAt = std::max(dueAt, drive->second.openAt);
    }
    if (dueAt < earliest) {
      earliest = dueAt;
      chosen = it;
    }
  }
  if (chosen == entries_.end() || earliest > now) {
    wakeAt = earliest;
    return std::nullopt;
  }
  drives_[chosen->second.devicePosition].busy = true;
  return Job{.key = chosen->first,
             .devicePosition = chosen->second.devicePosition,
             .index = chosen->second.index,
             .subindex = chosen->second.subindex};
}

void ParameterRefresher::poll(const Job& job) {
  const auto readStart = std::chrono::steady_clock::now();

  // Resolved here rather than hidden behind a DeviceManager forwarder: the handle is what keeps
  // the device alive across the upload, and a device that a rescan retired simply fails its next
  // transfer.
  std::expected<DeviceParameterValue, std::string> result =
      mm::node::deviceNotFound(job.devicePosition);
  if (const auto device = deviceManager_.deviceAt(job.devicePosition)) {
    result = device->readParameter(job.index, job.subindex);
  }

  const auto readEnd = std::chrono::steady_clock::now();
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    // The drive stays closed for as long as the read took, so the refresher uses at most half of
    // the drive's control-plane time. A failed read counts too: a mailbox timeout holds the drive
    // for 700 ms.
    Drive& drive = drives_[job.devicePosition];
    drive.busy = false;
    drive.openAt = readEnd + (readEnd - readStart);

    // A concurrent release can erase the entry during the read.
    if (auto it = entries_.find(job.key); it != entries_.end()) {
      if (result) {
        it->second.backoff = std::chrono::milliseconds{0};
        it->second.nextDue = readStart + it->second.period;
      } else {
        it->second.backoff = nextBackoff(it->second.backoff, it->second.period);
        it->second.nextDue = readStart + it->second.backoff;
      }
    }
  }
  // The other workers skipped this drive while it was busy, and may wait for nothing else.
  cv_.notify_all();
}

void ParameterRefresher::pollDue() {
  // Background polling: keep the driver's per-read SDO debug traces out of the log (demoted to
  // trace). A direct, user-initiated read runs without this guard and keeps its debug trace.
  const mm::comm::ScopedQuietSdoLog quietSdoLogging;
  const auto now = std::chrono::steady_clock::now();
  // Each read closes its drive past now and schedules its entry past now, so the pass ends.
  while (true) {
    std::optional<Job> job;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      auto wakeAt = std::chrono::steady_clock::time_point::max();
      job = takeDue(now, wakeAt);
    }
    if (!job) {
      return;
    }
    poll(*job);
  }
}

void ParameterRefresher::work(const std::stop_token& stopToken) {
  const mm::comm::ScopedQuietSdoLog quietSdoLogging;
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stopToken.stop_requested()) {
    auto wakeAt = std::chrono::steady_clock::time_point::max();
    if (const auto job = takeDue(std::chrono::steady_clock::now(), wakeAt)) {
      lock.unlock();
      poll(*job);
      lock.lock();
      continue;
    }
    if (wakeAt == std::chrono::steady_clock::time_point::max()) {
      cv_.wait(lock);  // nothing to poll on a free drive: sleep until something changes
    } else {
      cv_.wait_until(lock, wakeAt);  // wake at the deadline, or earlier when something changes
    }
  }
}

}  // namespace mm::node
