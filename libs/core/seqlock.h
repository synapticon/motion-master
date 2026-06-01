#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

  /// @brief Publishes only the first @p bytes of @p value.  Single-writer only.
  ///
  /// For payloads where only a leading prefix is live — e.g. a length-prefixed buffer sized to a
  /// large fixed capacity but usually carrying far less — copying just that prefix keeps the
  /// publish cost proportional to the real data instead of the full capacity.  @p bytes must come
  /// from a source independent of the payload (it is *not* read from @p value), so a torn in-band
  /// length can never widen the copy; it is clamped to @c sizeof(T) regardless.  Bytes of the
  /// stored value beyond @p bytes are left unchanged and must not be relied on by consumers.
  ///
  /// @param value  Snapshot whose leading @p bytes to publish.
  /// @param bytes  Number of leading bytes to copy (clamped to @c sizeof(T)).
  void store(const T& value, size_t bytes) {
    const size_t n = bytes < sizeof(T) ? bytes : sizeof(T);
    const uint32_t s = seq_.load(std::memory_order_relaxed);
    seq_.store(s + 1, std::memory_order_relaxed);         // odd: write in progress
    std::atomic_thread_fence(std::memory_order_release);  // odd marker before data
    std::memcpy(&value_, &value, n);
    std::atomic_thread_fence(std::memory_order_release);  // data before even marker
    seq_.store(s + 2, std::memory_order_relaxed);         // even: stable
  }

  /// @brief Copies the first @p bytes of the latest stable snapshot into @p out.
  ///
  /// The prefix counterpart of @c load(T&): see @c store(const T&, size_t) for when this applies.
  /// @p bytes is clamped to @c sizeof(T); bytes of @p out beyond @p bytes are left unchanged.
  ///
  /// @param out    Destination for the snapshot prefix.
  /// @param bytes  Number of leading bytes to copy (clamped to @c sizeof(T)).
  void load(T& out, size_t bytes) const {
    const size_t n = bytes < sizeof(T) ? bytes : sizeof(T);
    uint32_t before;
    uint32_t after;
    do {
      before = seq_.load(std::memory_order_acquire);
      if (before & 1u) {
        continue;  // write in progress — do not even copy
      }
      std::memcpy(&out, &value_, n);
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
