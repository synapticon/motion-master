#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <thread>

namespace mm::node {

class DeviceManager;

/// @brief Keeps a reference-counted set of SDO parameters fresh by polling them off-thread.
///
/// A monitoring that samples objects which are not PDO-mapped cannot read them from the process
/// image — they have to be fetched over SDO, which is far too slow to do on the sampling tick.
/// The refresher decouples the two: it owns one background thread that periodically calls
/// @c DeviceManager::readDeviceParameter (a live SDO upload that refreshes the device's parameter
/// cache), and the sampler then reads only that cache. Bus access is serialised by the driver's
/// control-plane mutex; a single shared thread matches the bus being one serial resource.
///
/// Entries are reference-counted by @c (devicePosition, index, subindex): several monitorings
/// asking for the same object poll it once, and it stops being polled only when the last one
/// releases it. The effective poll period of an entry is the shortest period any requester asked
/// for, clamped to a floor (SDO is slow; there is no point hammering the mailbox).
///
/// All public methods are thread-safe. The class is non-copyable and non-movable (it owns a
/// thread and a mutex).
class ParameterRefresher {
 public:
  /// @brief Constructs a refresher over @p deviceManager. Does not start the thread — call start().
  explicit ParameterRefresher(DeviceManager& deviceManager);

  /// @brief Stops the thread (if running) and joins it.
  ~ParameterRefresher();

  ParameterRefresher(const ParameterRefresher&) = delete;
  ParameterRefresher& operator=(const ParameterRefresher&) = delete;

  /// @brief Adds a reference to keeping @p (devicePosition, index, subindex) fresh.
  ///
  /// First reference starts polling the object; later references only raise the count and, if
  /// @p period is shorter than the current one, tighten the poll period. Pair every call with a
  /// @c release. @p period is clamped to an internal floor.
  ///
  /// @param devicePosition  1-based bus position.
  /// @param index           CoE object index.
  /// @param subindex        CoE object subindex.
  /// @param period          Desired maximum time between polls of this object.
  void acquire(uint16_t devicePosition, uint16_t index, uint8_t subindex,
               std::chrono::milliseconds period);

  /// @brief Drops a reference previously taken with @c acquire. The object stops being polled
  ///        when its last reference is released. A release with no matching acquire is ignored.
  void release(uint16_t devicePosition, uint16_t index, uint8_t subindex);

  /// @brief Starts the background polling thread. Idempotent.
  void start();

  /// @brief Stops and joins the background polling thread. Idempotent.
  void stop();

  /// @brief Number of distinct objects currently tracked (reference count > 0). For tests/UI.
  std::size_t trackedCount() const;

  /// @brief Polls every tracked object whose schedule is due as of now, advancing each one's
  ///        next-due time (and applying error backoff). Called by the background thread each
  ///        wake-up; exposed so tests can drive one pass deterministically without the thread.
  void pollDue();

 private:
  /// @brief One tracked object: its address, how many monitorings need it, its poll period, and
  ///        its schedule (next-due time and current error backoff).
  struct Entry {
    uint16_t devicePosition = 0;
    uint16_t index = 0;
    uint8_t subindex = 0;
    int refCount = 0;
    std::chrono::milliseconds period{};
    std::chrono::steady_clock::time_point nextDue{};  // default (epoch) => due immediately
    std::chrono::milliseconds backoff{};              // 0 until a poll fails
  };

  /// @brief Packs (devicePosition, index, subindex) into the entry-map key.
  static uint64_t makeKey(uint16_t devicePosition, uint16_t index, uint8_t subindex);

  /// @brief Background thread body: waits until the nearest entry is due (or it is woken by an
  ///        acquire/release/stop), then polls the due entries.
  void run();

  DeviceManager& deviceManager_;
  mutable std::mutex mutex_;           ///< Guards entries_ and running_.
  std::condition_variable cv_;         ///< Wakes the thread on acquire/release/stop.
  std::map<uint64_t, Entry> entries_;  ///< Tracked objects, keyed by makeKey().
  bool running_ = false;               ///< Whether the thread should keep looping.
  std::thread thread_;
};

}  // namespace mm::node
