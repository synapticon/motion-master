#include "comm/spoe_process_data.h"

#include <algorithm>
#include <vector>

namespace mm::comm::spoe {

void SpoeOutputSlot::reset(std::size_t frameBytes) {
  for (auto& buffer : buffers_) {
    buffer = std::vector<uint8_t>(frameBytes);
  }
  writeIndex_ = 0;
  readIndex_ = 1;
  middle_.store(2);
}

void SpoeOutputSlot::write(std::span<const uint8_t> frame) {
  std::vector<uint8_t>& target = buffers_[writeIndex_];
  std::copy_n(frame.begin(), std::min(frame.size(), target.size()), target.begin());
  // Release publishes the bytes with the index. The reader acquires them in read().
  const uint8_t previous = middle_.exchange(writeIndex_ | kFresh, std::memory_order_acq_rel);
  writeIndex_ = previous & kIndexMask;
}

std::span<const uint8_t> SpoeOutputSlot::read() {
  if ((middle_.load(std::memory_order_relaxed) & kFresh) != 0) {
    const uint8_t previous = middle_.exchange(readIndex_, std::memory_order_acq_rel);
    readIndex_ = previous & kIndexMask;
  }
  return buffers_[readIndex_];
}

void SpoeInputQueue::reset(std::size_t frameBytes, std::size_t capacity) {
  frameBytes_ = frameBytes;
  capacity_ = capacity;
  storage_ = std::vector<uint8_t>(frameBytes * capacity);
  head_.store(0);
  tail_.store(0);
}

bool SpoeInputQueue::push(std::span<const uint8_t> frame) {
  const std::size_t head = head_.load(std::memory_order_relaxed);
  const std::size_t tail = tail_.load(std::memory_order_acquire);
  if (capacity_ == 0 || head - tail >= capacity_) {
    return false;
  }
  const std::size_t slot = (head % capacity_) * frameBytes_;
  std::copy_n(frame.begin(), std::min(frame.size(), frameBytes_),
              storage_.begin() + static_cast<std::ptrdiff_t>(slot));
  head_.store(head + 1, std::memory_order_release);
  return true;
}

bool SpoeInputQueue::pop(std::span<uint8_t> frame) {
  const std::size_t tail = tail_.load(std::memory_order_relaxed);
  const std::size_t head = head_.load(std::memory_order_acquire);
  if (tail == head) {
    return false;
  }
  const std::size_t slot = (tail % capacity_) * frameBytes_;
  std::copy_n(storage_.begin() + static_cast<std::ptrdiff_t>(slot),
              std::min(frame.size(), frameBytes_), frame.begin());
  tail_.store(tail + 1, std::memory_order_release);
  return true;
}

std::size_t SpoeInputQueue::dropOldest(std::size_t keep) {
  const std::size_t tail = tail_.load(std::memory_order_relaxed);
  const std::size_t head = head_.load(std::memory_order_acquire);
  const std::size_t queued = head - tail;
  if (queued <= keep) {
    return 0;
  }
  const std::size_t dropped = queued - keep;
  tail_.store(tail + dropped, std::memory_order_release);
  return dropped;
}

std::size_t SpoeInputQueue::size() const {
  return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
}

void SpoeFrameAssembler::reset(std::size_t frameBytes) {
  frameBytes_ = frameBytes;
  partial_.clear();
  framingLosses_ = 0;
}

std::vector<uint8_t> SpoeFrameAssembler::add(std::span<const uint8_t> reply) {
  if (frameBytes_ == 0) {
    return {};
  }
  partial_.insert(partial_.end(), reply.begin(), reply.end());
  if (reply.size() < kInputPopLimit) {
    if (const std::size_t stale = partial_.size() % frameBytes_; stale != 0) {
      partial_.erase(partial_.begin(), partial_.begin() + static_cast<std::ptrdiff_t>(stale));
      ++framingLosses_;
    }
  }
  const std::size_t whole = partial_.size() - partial_.size() % frameBytes_;
  std::vector<uint8_t> frames(partial_.begin(),
                              partial_.begin() + static_cast<std::ptrdiff_t>(whole));
  partial_.erase(partial_.begin(), partial_.begin() + static_cast<std::ptrdiff_t>(whole));
  return frames;
}

}  // namespace mm::comm::spoe
