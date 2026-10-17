#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

namespace mm::node {

class DeviceManager;

/// @brief Keeps a reference-counted set of SDO parameters fresh by polling them off-thread.
///
/// A monitoring that samples objects which are not PDO-mapped cannot read them from the process
/// image — they have to be fetched over SDO, which is far too slow to do on the sampling tick.
/// The refresher decouples the two: a fixed pool of worker threads reads each object with
/// @c Device::readParameter (a live SDO upload into the object's parameter cell), and the sampler
/// then reads only that cell.
///
/// **Different drives are read at the same time.** Both fieldbus drivers let the mailbox work of
/// different drives run at the same time, and no two workers read one drive at the same time. So a
/// slow drive, or a drive that does not answer, delays only its own objects. The pool is
/// small on purpose: over SOEM every mailbox transaction holds one of a few frame slots for the
/// whole bus, and the HTTP workers and the procedures need the rest of them.
///
/// **The refresher uses at most half of the control-plane time of a drive.** After each read, it
/// reads nothing more from that drive for as long as that read took. So other calls to the drive
/// always find free gaps, on a slow drive and on a fast one. A slow drive pays for this with a
/// longer refresh period.
///
/// Entries are reference-counted by @c (devicePosition, index, subindex): several monitorings
/// asking for the same object poll it once, and it stops being polled only when the last one
/// releases it. The effective poll period of an entry is the shortest period any requester asked
/// for, clamped to a floor (SDO is slow; there is no point hammering the mailbox).
///
/// All public methods are thread-safe. The class is non-copyable and non-movable (it owns threads
/// and a mutex).
class ParameterRefresher {
 public:
  /// @brief Constructs a refresher over @p deviceManager. Does not start the workers — call
  /// start().
  explicit ParameterRefresher(DeviceManager& deviceManager);

  /// @brief Stops the workers (if running) and joins them.
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

  /// @brief Starts the worker threads. Idempotent.
  void start();

  /// @brief Stops and joins the worker threads. Idempotent.
  void stop();

  /// @brief Number of distinct objects currently tracked (reference count > 0). For tests/UI.
  std::size_t trackedCount() const;

  /// @brief Polls the due objects on the calling thread, one pass, with no worker running.
  ///
  /// A read closes its drive until the read's duration has passed again, so one pass reads at most
  /// one object per drive. For tests, which drive the schedule without threads.
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

  /// @brief The refresher's view of one drive: whether a worker reads it now, and when the
  ///        refresher may read it next.
  struct Drive {
    bool busy = false;
    std::chrono::steady_clock::time_point openAt{};  // default (epoch) => open
  };

  /// @brief One read a worker took: the entry's key and the object's address.
  struct Job {
    uint64_t key = 0;
    uint16_t devicePosition = 0;
    uint16_t index = 0;
    uint8_t subindex = 0;
  };

  /// @brief Packs (devicePosition, index, subindex) into the entry-map key.
  static uint64_t makeKey(uint16_t devicePosition, uint16_t index, uint8_t subindex);

  /// @brief Takes the due entry with the earliest time on a drive that no worker reads, and marks
  ///        that drive busy. Call with mutex_ held.
  ///
  /// @param now     The time an entry must be due by.
  /// @param wakeAt  Set to the earliest time an entry on a free drive becomes due, or to
  ///                @c time_point::max() when there is none. Left alone when a job is returned.
  std::optional<Job> takeDue(std::chrono::steady_clock::time_point now,
                             std::chrono::steady_clock::time_point& wakeAt);

  /// @brief Reads one object, then schedules its entry and closes its drive for the read's
  ///        duration. Call with mutex_ not held.
  void poll(const Job& job);

  /// @brief Worker body: takes a due read, or waits until one is due or it is woken by an
  ///        acquire, a release, a finished read or a stop.
  void work(const std::stop_token& stopToken);

  DeviceManager& deviceManager_;
  mutable std::mutex mutex_;           ///< Guards entries_, drives_ and workers_.
  std::condition_variable cv_;         ///< Wakes the workers on acquire/release/read/stop.
  std::map<uint64_t, Entry> entries_;  ///< Tracked objects, keyed by makeKey().
  /// The drives the refresher has read, by position. Bounded by the positions on a bus, so an
  /// entry is never removed.
  std::map<uint16_t, Drive> drives_;
  std::vector<std::jthread> workers_;  ///< Empty while stopped.
};

}  // namespace mm::node
