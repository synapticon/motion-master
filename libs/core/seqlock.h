#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace mm::core {

/// @brief Single-writer / multi-reader sequence lock for a trivially-copyable value.
///
/// A seqlock lets one writer publish snapshots of @p T to any number of readers
/// without the readers ever blocking the writer.  It is the mechanism the RT
/// game loop uses to hand the process-data image to non-RT consumers (HTTP, the
/// monitoring WebSocket) and to receive output values back from them — the side
/// that must never stall is wait-free.
///
/// How it works: an even sequence counter means the value is stable; odd means a
/// write is in progress.  A reader copies the value between two counter reads and
/// retries if the counter changed (or was odd) — so it may copy a torn value, but
/// it never *uses* one.  This is why @p T must be trivially copyable: copying a
/// half-updated value must be defined behaviour (a raw byte copy), which rules out
/// types that own heap storage such as @c std::string or @c std::vector.  Wrap a
/// dynamically-sized payload in a fixed-capacity POD (a length field plus a
/// @c std::array) instead.
///
/// Threading contract:
/// - Exactly **one** writer.  @c store() is not safe to call concurrently with
///   itself; serialise multiple producers (e.g. several HTTP threads writing
///   output PDOs) with their own mutex before calling @c store().
/// - Any number of concurrent readers.  @c load() never blocks the writer and is
///   itself lock-free; it spins only while a write is actually in flight, which —
///   at the snapshot sizes and write rates here (~400 bytes, sub-microsecond
///   copies, infrequent writes) — is effectively never.
///
/// @tparam T  Trivially-copyable, default-constructible snapshot type.
template <typename T>
class SeqLock {
  static_assert(std::is_trivially_copyable_v<T>,
                "SeqLock<T> requires a trivially-copyable T — a torn read is byte-copied then "
                "discarded, which is only defined for trivially-copyable types");
  static_assert(std::is_default_constructible_v<T>,
                "SeqLock<T> requires a default-constructible T");

 public:
  SeqLock() = default;

  SeqLock(const SeqLock&) = delete;
  SeqLock& operator=(const SeqLock&) = delete;

  /// @brief Publishes @p value to readers.  Single-writer only.
  ///
  /// Marks the value odd (write in progress), stores it, then marks it even
  /// (stable) again.  The release fences ensure a reader that observes the even
  /// counter also observes the fully-written value.
  ///
  /// @param value  Snapshot to publish.
  void store(const T& value) {
    const uint32_t s = seq_.load(std::memory_order_relaxed);
    seq_.store(s + 1, std::memory_order_relaxed);         // odd: write in progress
    std::atomic_thread_fence(std::memory_order_release);  // odd marker before data
    value_ = value;
    std::atomic_thread_fence(std::memory_order_release);  // data before even marker
    seq_.store(s + 2, std::memory_order_relaxed);         // even: stable
  }

  /// @brief Copies the latest stable snapshot into @p out.
  ///
  /// Wait-free with respect to the writer: never takes a lock the writer waits on.
  /// Retries until it captures a value that was not modified mid-copy.
  ///
  /// @param out  Destination for the snapshot.
  void load(T& out) const {
    uint32_t before;
    uint32_t after;
    do {
      before = seq_.load(std::memory_order_acquire);
      if (before & 1u) {
        continue;  // write in progress — do not even copy
      }
      out = value_;
      std::atomic_thread_fence(std::memory_order_acquire);  // copy before re-reading counter
      after = seq_.load(std::memory_order_relaxed);
    } while ((before & 1u) || before != after);
  }

  /// @brief Convenience overload returning the snapshot by value.
  /// @return The latest stable snapshot.
  T load() const {
    T out;
    load(out);
    return out;
  }

 private:
  std::atomic<uint32_t> seq_{0};  // even = stable, odd = write in progress
  T value_{};
};

}  // namespace mm::core
