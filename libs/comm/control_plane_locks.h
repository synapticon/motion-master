#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <semaphore>
#include <shared_mutex>

namespace mm::comm {

/// @brief A reader/writer lock that lets no new reader in while a writer waits.
///
/// A fieldbus driver takes it shared for the work on one drive and exclusive for the work on the
/// whole bus. The shared holders come from several threads: HTTP workers, the monitoring refresher
/// and a procedure. Their holds can overlap with no gap between them. std::shared_mutex on glibc
/// lets a new reader in while a writer waits, so a scan could wait for as long as the overlap
/// lasts. This lock makes a waiting writer stop new readers, so a scan waits only for the holds
/// that already started.
///
/// The member names are the ones std::unique_lock and std::shared_lock call.
class BusLock {
 public:
  void lock();
  void unlock();
  void lock_shared();
  void unlock_shared();

 private:
  std::mutex mutex_;
  std::condition_variable readerCanEnter_;
  std::condition_variable writerCanEnter_;
  int readers_ = 0;
  int writersWaiting_ = 0;
  bool writer_ = false;
};

/// @brief One of a fixed number of slots for a frame on the wire. Released when destroyed.
///
/// SOEM has a fixed number of frame indexes (EC_MAXBUF), shared by every thread that sends a frame.
/// When none is free, ecx_getindex hands out one that is still in use, and two frames then share
/// it. Each holder of a slot sends one frame at a time, so the slots bound how many frames the
/// control plane has on the wire.
class FrameSlot {
 public:
  explicit FrameSlot(std::counting_semaphore<>& slots);
  ~FrameSlot();
  FrameSlot(FrameSlot&& other) noexcept;
  FrameSlot(const FrameSlot&) = delete;
  FrameSlot& operator=(const FrameSlot&) = delete;
  FrameSlot& operator=(FrameSlot&&) = delete;

 private:
  std::counting_semaphore<>* slots_;
};

/// @brief The bus lock held shared, the lock of one drive, and a frame slot.
///
/// The members release in reverse order, so the frame slot goes first and the bus lock last.
struct DriveLock {
  std::shared_lock<BusLock> bus;
  std::unique_lock<std::mutex> drive;
  FrameSlot frame;
};

/// @brief The bus lock held shared, and a frame slot. The slot goes first.
struct BusIoLock {
  std::shared_lock<BusLock> bus;
  FrameSlot frame;
};

/// @brief The control-plane locks of one fieldbus driver: one for the bus, one per drive.
///
/// The mailbox work of different drives runs at the same time. Each drive has its own mailbox, so
/// the work on one drive needs only that drive's lock, with the bus lock held shared. The work on
/// the whole bus, such as a scan or a re-map, holds the bus lock exclusive and so excludes every
/// drive. The order is always the bus lock first, then one drive lock. Nobody holds two drive
/// locks, and an exclusive holder takes no drive lock.
///
/// Every lock is held for one transaction or one transfer. Never hold one across a sleep that
/// waits for something other than the bus.
///
/// A drive lock and a bus I/O lock also take a frame slot, after every other lock they take. No
/// holder of a slot waits for another lock, so the slots cannot deadlock. An exclusive holder takes
/// no slot, because nothing else on the control plane sends a frame while it holds the bus.
class ControlPlaneLocks {
 public:
  /// The number of frame slots. SOEM's frame indexes (EC_MAXBUF, set in ports/soem) are shared with
  /// the RT thread, which uses one per frame of process data in a cycle. The slots cap the
  /// control plane's share, and the RT thread gets the rest.
  ///
  /// The cap is small on purpose. The NIC sends frames in order, so the RT frame can wait behind
  /// every control-plane frame in flight. A mailbox frame carries the drive's whole mailbox, and a
  /// full frame takes about 120 µs at 100 Mbit/s. So eight full frames delay the RT frame by about
  /// 1 ms. This was calculated, not measured.
  ///
  /// A slot holds two indexes for a moment after a mailbox read that got no frame, because the
  /// port keeps the index of that read until the status read after it comes back. So with 32
  /// indexes, eight slots leave the RT thread 16 in the worst case. A frame carries 1486 bytes,
  /// and a SOMANET drive maps about 100, so one frame holds about 14 drives. Separate LRD and LWR
  /// datagrams, which some devices need, double the frames. So 16 indexes serve about 112 SOMANET
  /// drives in the worst case.
  static constexpr std::ptrdiff_t kFrameSlots = 8;

  /// @param positions  The number of drive positions, position 0 included. A position passed to
  ///                   lockDrive must be below it.
  explicit ControlPlaneLocks(std::size_t positions);

  /// @brief Excludes all other control-plane work. For work on the whole bus.
  [[nodiscard]] std::unique_lock<BusLock> lockBus();

  /// @brief Excludes only work on the whole bus. For reads of what a scan or a re-map produced,
  /// which send no frame.
  [[nodiscard]] std::shared_lock<BusLock> lockBusShared();

  /// @brief Excludes only work on the whole bus, and takes a frame slot. For reads that send frames
  /// to several drives and change nothing in the context.
  [[nodiscard]] BusIoLock lockBusIo();

  /// @brief Excludes work on the whole bus and other work on the same drive.
  [[nodiscard]] DriveLock lockDrive(uint16_t position);

 private:
  BusLock bus_;
  std::unique_ptr<std::mutex[]> drives_;
  std::counting_semaphore<> frameSlots_{kFrameSlots};
};

}  // namespace mm::comm
