#pragma once

#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

namespace mm::node {

/// @brief A lock-free circular recorder of the raw process image, one record per RT cycle.
///
/// The RT loop appends one record every cycle via @c write() and never blocks: a record is the
/// whole raw input and output IOmap for that cycle plus its timestamp and working counter. The
/// ring overwrites its oldest record on wrap, so it holds the most recent @c capacity() cycles —
/// a flight-data recorder, not a drain queue. Any number of non-RT readers consume it through
/// independent read cursors (a read index, not a tail): a reader never frees a slot and never
/// gates the producer, so the RT loop overwrites unconditionally and may lap a slow reader. That
/// is allowed and detected, not a bug — a lapped reader sees @c readRecord return @c false and
/// resyncs from @c oldestValidSeq().
///
/// It is the single RT-written source for both the live monitoring stream (each monitoring holds
/// a cursor and ships every record in @c [cursor, head)) and point reads of the freshest value
/// (read @c head()-1) — one write per cycle, and one record holds both directions from the
/// *same* cycle.
///
/// Sequencing: each record carries an absolute cycle sequence number; @c slot = seq % capacity.
/// @c head() is the producer's monotonic next-seq (it never wraps). The per-slot sequence word is
/// stored after the payload with release ordering — its publication is what makes a record
/// readable — and a reader re-checks it after copying to detect a write that raced the copy (a
/// per-slot sequence re-check). The RT @c write() is wait-free: a few @c memcpy plus two release
/// stores.
///
/// Single writer (the RT loop), many concurrent readers. Not copyable or movable.
class ProcessDataRing {
 public:
  /// @brief One recorded cycle, copied out by a reader. @c inputs / @c outputs are the raw IOmap
  ///        bytes for that cycle (sized to the live image at record time).
  struct Record {
    uint64_t seq = 0;  ///< Absolute cycle sequence number.
    // Wall-clock epoch nanoseconds at exchange (system clock). u64 holds it until ~year 2554.
    // INTERNAL precision only: epoch-ns today (~1.7e18) far exceeds the 2^53 exact-integer limit
    // of a JavaScript double. The live monitoring row reduces it to epoch MICROSECONDS (ns / 1000)
    // before JSON — epoch-µs stays exact in JS until ~year 2255 and, unlike milliseconds, gives
    // every cycle a distinct timestamp even at sub-millisecond cycle periods (periodUs < 1000).
    // The full ns precision is retained here for the binary dump.
    uint64_t timestampNs = 0;
    int32_t wkc = 0;  ///< Working counter for the cycle.
    std::vector<uint8_t> inputs;
    std::vector<uint8_t> outputs;
  };

  ProcessDataRing() = default;
  ~ProcessDataRing();

  ProcessDataRing(const ProcessDataRing&) = delete;
  ProcessDataRing& operator=(const ProcessDataRing&) = delete;

  /// @brief (Re)allocates the ring for an image of @p inputCap / @p outputCap bytes per direction
  ///        and @p capacity cycles, discarding any prior recording (sequence restarts at 0).
  ///
  /// Called from @c DeviceManager::configureProcessData once the process-image sizes are known —
  /// a layout-changing re-map re-allocates, because records under the old layout are undecodable
  /// under a new one. The storage is best-effort @c mlock'd on POSIX so the RT @c write() never
  /// page-faults (process-wide @c mlockall already covers it when RT scheduling is in effect;
  /// this is belt-and-suspenders and silently does nothing if locking is not permitted).
  void allocate(uint32_t inputCap, uint32_t outputCap, size_t capacity);

  /// @brief Releases the storage and resets to the unallocated state. Idempotent.
  void clear();

  /// @brief Whether @c allocate has been called with a non-zero capacity.
  bool allocated() const { return capacity_ != 0; }

  /// @brief Records one cycle. RT, wait-free, single-writer. No-op until @c allocate.
  ///
  /// @p inputs / @p outputs are clamped to the per-direction capacities the ring was allocated
  /// with (they match the live image exactly in normal use).
  void write(uint64_t timestampNs, int wkc, std::span<const uint8_t> inputs,
             std::span<const uint8_t> outputs);

  /// @brief The producer's next sequence number; @c head()-1 is the newest recorded cycle, and
  ///        @c head()==0 means nothing has been recorded yet.
  uint64_t head() const { return head_.load(std::memory_order_acquire); }

  /// @brief Number of cycles the ring can hold.
  size_t capacity() const { return capacity_; }

  /// @brief The oldest sequence number still in the ring: @c max(0, head - capacity). A cursor
  ///        below this has been lapped (its data overwritten) and must resync to this value.
  uint64_t oldestValidSeq() const;

  /// @brief Copies the record for @p seq into @p out. Lock-free; retries are the caller's job.
  ///
  /// @return @c true if @p seq is present and was copied without a concurrent overwrite; @c false
  ///         if @p seq is not in the ring (never written, already overwritten) or the copy raced
  ///         the producer (a torn read — the caller may retry or skip the cycle).
  bool readRecord(uint64_t seq, Record& out) const;

 private:
  // Per-record payload layout in buffer_ (accessed by memcpy, so no alignment constraints):
  //   [0] timestampNs (u64)  [8] wkc (i32)  [12] inputBytes (u32)  [16] outputBytes (u32)
  //   [20] input region (inputCap_ bytes)   [20+inputCap_] output region (outputCap_ bytes)
  static constexpr size_t kHeaderBytes = 20;

  std::vector<uint8_t> buffer_;  // capacity_ * stride_ bytes of payload records
  std::vector<std::atomic<uint64_t>>
      seqWords_;  // one publication word per slot; kInvalidSeq = empty
  size_t stride_ = 0;
  size_t capacity_ = 0;
  uint32_t inputCap_ = 0;
  uint32_t outputCap_ = 0;
  std::atomic<uint64_t> head_{0};  // next sequence number to write (monotonic, never wraps)
  bool locked_ = false;            // whether the storage was successfully mlock'd
};

}  // namespace mm::node
