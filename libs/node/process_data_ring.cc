#include "node/process_data_ring.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

#if defined(__linux__) || defined(__APPLE__)
#include <sys/mman.h>
#endif

namespace mm::node {
namespace {

// A slot's sequence word holds this until its first record is published, and again while a write
// is in progress — a reader that sees it (or any value other than the seq it asked for) rejects
// the slot. UINT64_MAX is never a real sequence (it would take >5e8 years at a 1 ms cycle).
constexpr uint64_t kInvalidSeq = std::numeric_limits<uint64_t>::max();

}  // namespace

ProcessDataRing::~ProcessDataRing() { clear(); }

void ProcessDataRing::allocate(uint32_t inputCap, uint32_t outputCap, size_t capacity) {
  clear();
  if (capacity == 0) {
    return;
  }
  inputCap_ = inputCap;
  outputCap_ = outputCap;
  capacity_ = capacity;
  stride_ = kHeaderBytes + inputCap_ + outputCap_;

  buffer_.assign(capacity_ * stride_, uint8_t{0});
  // std::atomic is not copyable, so the vector is sized then each word seeded individually.
  seqWords_ = std::vector<std::atomic<uint64_t>>(capacity_);
  for (auto& w : seqWords_) {
    w.store(kInvalidSeq, std::memory_order_relaxed);
  }
  head_.store(0, std::memory_order_release);

#if defined(__linux__) || defined(__APPLE__)
  // Best-effort pin so the RT write() never page-faults. Process-wide mlockall(MCL_FUTURE) (set
  // when RT scheduling is in effect) already covers this; locking again is harmless and also pins
  // the ring when the loop runs non-RT. Failure (no CAP_IPC_LOCK) is ignored — non-RT fallback.
  locked_ = mlock(buffer_.data(), buffer_.size()) == 0;
  if (mlock(seqWords_.data(), seqWords_.size() * sizeof(std::atomic<uint64_t>)) != 0) {
    locked_ = false;
  }
#endif
}

void ProcessDataRing::clear() {
#if defined(__linux__) || defined(__APPLE__)
  if (locked_) {
    if (!buffer_.empty()) {
      munlock(buffer_.data(), buffer_.size());
    }
    if (!seqWords_.empty()) {
      munlock(seqWords_.data(), seqWords_.size() * sizeof(std::atomic<uint64_t>));
    }
  }
#endif
  locked_ = false;
  buffer_.clear();
  buffer_.shrink_to_fit();
  // Release the storage by swapping in an empty vector rather than clear()+shrink_to_fit():
  // shrink_to_fit may reallocate, which requires moving the elements, and std::atomic is neither
  // movable nor copyable (libc++/MSVC reject the instantiation; libstdc++ happens to tolerate it).
  // swap only exchanges the vectors' internal pointers, so it never touches the atomics.
  std::vector<std::atomic<uint64_t>>().swap(seqWords_);
  stride_ = 0;
  capacity_ = 0;
  inputCap_ = 0;
  outputCap_ = 0;
  head_.store(0, std::memory_order_release);
}

void ProcessDataRing::write(uint64_t timestampNs, int wkc, std::span<const uint8_t> inputs,
                            std::span<const uint8_t> outputs) {
  if (capacity_ == 0) {
    return;
  }
  const uint64_t seq = head_.load(std::memory_order_relaxed);  // single writer — its own counter
  const size_t slot = static_cast<size_t>(seq % capacity_);
  uint8_t* rec = buffer_.data() + slot * stride_;

  // Invalidate before mutating the payload so a concurrent reader of this slot detects the
  // overwrite (it will see a value other than the seq it asked for, on either side of its copy).
  seqWords_[slot].store(kInvalidSeq, std::memory_order_release);

  const uint32_t ib = std::min<uint32_t>(static_cast<uint32_t>(inputs.size()), inputCap_);
  const uint32_t ob = std::min<uint32_t>(static_cast<uint32_t>(outputs.size()), outputCap_);
  const int32_t w = static_cast<int32_t>(wkc);
  std::memcpy(rec + 0, &timestampNs, sizeof(timestampNs));
  std::memcpy(rec + 8, &w, sizeof(w));
  std::memcpy(rec + 12, &ib, sizeof(ib));
  std::memcpy(rec + 16, &ob, sizeof(ob));
  std::memcpy(rec + kHeaderBytes, inputs.data(), ib);
  std::memcpy(rec + kHeaderBytes + inputCap_, outputs.data(), ob);

  // Publish: the release store orders the payload writes above before any reader's acquire load
  // of this word observes the matching seq.
  seqWords_[slot].store(seq, std::memory_order_release);
  head_.store(seq + 1, std::memory_order_release);
}

uint64_t ProcessDataRing::oldestValidSeq() const {
  const uint64_t h = head_.load(std::memory_order_acquire);
  return h > capacity_ ? h - capacity_ : 0;
}

bool ProcessDataRing::readRecord(uint64_t seq, Record& out) const {
  if (capacity_ == 0) {
    return false;
  }
  const size_t slot = static_cast<size_t>(seq % capacity_);
  if (seqWords_[slot].load(std::memory_order_acquire) != seq) {
    return false;  // not yet written, already overwritten, or a write is in progress
  }
  const uint8_t* rec = buffer_.data() + slot * stride_;
  uint64_t ts = 0;
  int32_t w = 0;
  uint32_t ib = 0;
  uint32_t ob = 0;
  std::memcpy(&ts, rec + 0, sizeof(ts));
  std::memcpy(&w, rec + 8, sizeof(w));
  std::memcpy(&ib, rec + 12, sizeof(ib));
  std::memcpy(&ob, rec + 16, sizeof(ob));
  // Clamp against a torn header read so the copies below stay in bounds.
  ib = std::min<uint32_t>(ib, inputCap_);
  ob = std::min<uint32_t>(ob, outputCap_);
  out.seq = seq;
  out.timestampNs = ts;
  out.wkc = w;
  out.inputs.assign(rec + kHeaderBytes, rec + kHeaderBytes + ib);
  out.outputs.assign(rec + kHeaderBytes + inputCap_, rec + kHeaderBytes + inputCap_ + ob);

  // Ensure the payload copy completes before re-reading the sequence word, so a producer that
  // overwrote this slot mid-copy is detected as a mismatch.
  std::atomic_thread_fence(std::memory_order_acquire);
  return seqWords_[slot].load(std::memory_order_relaxed) == seq;
}

}  // namespace mm::node
