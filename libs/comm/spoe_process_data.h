#pragma once

// The pieces that carry SPoE process data between the RT thread and the exchange thread of one
// drive. The RT thread never waits on either of them.
//
// The drive buffers its input frames and returns them in batches, so the inputs travel through a
// queue and the RT thread takes one frame per cycle. That paces the frames to the RT cycle, which
// is what lets monitoring see every frame in order. The outputs are latest-wins: a frame the
// exchange thread did not send yet is replaced by a newer one.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mm::comm::spoe {

/// `pop_from_pdo_fifo_buffer` in the firmware returns at most this many bytes of inputs at once.
constexpr std::size_t kInputPopLimit = 500;

/// Hands the newest output frame from one writer to one reader. Three buffers: the writer fills
/// its own and swaps it with the middle one, and the reader swaps the middle one with its own when
/// a newer frame is there. Neither side waits, and neither side sees a half-written frame.
class SpoeOutputSlot {
 public:
  /// Sizes every buffer and clears them. Not safe while the writer or the reader runs.
  void reset(std::size_t frameBytes);

  /// Writer side. @p frame must hold the size given to @c reset.
  void write(std::span<const uint8_t> frame);

  /// Reader side. Returns the newest frame, or the previous one again when nothing newer arrived.
  std::span<const uint8_t> read();

 private:
  static constexpr uint8_t kFresh = 0x04;
  static constexpr uint8_t kIndexMask = 0x03;

  std::array<std::vector<uint8_t>, 3> buffers_;
  uint8_t writeIndex_ = 0;
  uint8_t readIndex_ = 1;
  std::atomic<uint8_t> middle_{2};
};

/// A single-producer, single-consumer queue of fixed-size input frames. The exchange thread pushes
/// and the RT thread pops.
class SpoeInputQueue {
 public:
  /// Sizes the queue and empties it. Not safe while the producer or the consumer runs.
  void reset(std::size_t frameBytes, std::size_t capacity);

  /// Producer side. Returns false, and drops @p frame, when the queue is full.
  bool push(std::span<const uint8_t> frame);

  /// Consumer side. Copies the oldest frame into @p frame and removes it. Returns false, and
  /// leaves @p frame untouched, when the queue is empty.
  bool pop(std::span<uint8_t> frame);

  /// Consumer side. Removes the oldest frames until at most @p keep remain, and returns how many
  /// it removed. Keeping the queue short keeps the delay between the drive and the RT cycle short.
  std::size_t dropOldest(std::size_t keep);

  std::size_t size() const;

 private:
  std::vector<uint8_t> storage_;
  std::size_t frameBytes_ = 0;
  std::size_t capacity_ = 0;
  std::atomic<std::size_t> head_{0};  // written by the producer
  std::atomic<std::size_t> tail_{0};  // written by the consumer
};

/// Cuts the input bytes of successive replies into whole frames.
///
/// The drive's buffer holds whole frames, but one reply carries at most @c kInputPopLimit bytes,
/// so a reply can end inside a frame. The rest of that frame starts the next reply, and is joined
/// to the part kept here.
///
/// A shorter reply means the drive emptied its buffer, so the reply ends on a frame boundary. If
/// the joined bytes are not a whole number of frames then, the kept part was stale: the drive
/// empties a full buffer outright, which can discard the rest of a cut frame. The stale bytes are
/// dropped and counted, and the framing is right again from that reply on.
class SpoeFrameAssembler {
 public:
  void reset(std::size_t frameBytes);

  /// Adds the input bytes of one reply. Returns the whole frames they complete, joined.
  std::vector<uint8_t> add(std::span<const uint8_t> reply);

  /// How many times stale bytes were dropped to restore the framing.
  uint64_t framingLosses() const { return framingLosses_; }

 private:
  std::size_t frameBytes_ = 0;
  std::vector<uint8_t> partial_;
  uint64_t framingLosses_ = 0;
};

}  // namespace mm::comm::spoe
