#include "comm/control_plane_locks.h"

#include <utility>

namespace mm::comm {

void BusLock::lock() {
  std::unique_lock<std::mutex> guard(mutex_);
  ++writersWaiting_;
  writerCanEnter_.wait(guard, [this] { return !writer_ && readers_ == 0; });
  --writersWaiting_;
  writer_ = true;
}

void BusLock::unlock() {
  {
    const std::lock_guard<std::mutex> guard(mutex_);
    writer_ = false;
  }
  // A waiting writer goes before a waiting reader, because a reader re-checks writersWaiting_.
  writerCanEnter_.notify_one();
  readerCanEnter_.notify_all();
}

void BusLock::lock_shared() {
  std::unique_lock<std::mutex> guard(mutex_);
  readerCanEnter_.wait(guard, [this] { return !writer_ && writersWaiting_ == 0; });
  ++readers_;
}

void BusLock::unlock_shared() {
  bool last = false;
  {
    const std::lock_guard<std::mutex> guard(mutex_);
    --readers_;
    last = readers_ == 0;
  }
  if (last) {
    writerCanEnter_.notify_one();
  }
}

FrameSlot::FrameSlot(std::counting_semaphore<>& slots) : slots_(&slots) { slots_->acquire(); }

FrameSlot::~FrameSlot() {
  if (slots_ != nullptr) {
    slots_->release();
  }
}

FrameSlot::FrameSlot(FrameSlot&& other) noexcept : slots_(std::exchange(other.slots_, nullptr)) {}

ControlPlaneLocks::ControlPlaneLocks(std::size_t positions)
    : drives_(std::make_unique<std::mutex[]>(positions)) {}

std::unique_lock<BusLock> ControlPlaneLocks::lockBus() { return std::unique_lock<BusLock>(bus_); }

std::shared_lock<BusLock> ControlPlaneLocks::lockBusShared() {
  return std::shared_lock<BusLock>(bus_);
}

BusIoLock ControlPlaneLocks::lockBusIo() {
  std::shared_lock<BusLock> bus(bus_);
  return BusIoLock{.bus = std::move(bus), .frame = FrameSlot(frameSlots_)};
}

DriveLock ControlPlaneLocks::lockDrive(uint16_t position) {
  std::shared_lock<BusLock> bus(bus_);
  std::unique_lock<std::mutex> drive(drives_[position]);
  return DriveLock{
      .bus = std::move(bus), .drive = std::move(drive), .frame = FrameSlot(frameSlots_)};
}

}  // namespace mm::comm
