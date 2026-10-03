#include "comm/spoe_process_data.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <span>
#include <thread>
#include <vector>

namespace {

using mm::comm::spoe::kInputPopLimit;
using mm::comm::spoe::SpoeFrameAssembler;
using mm::comm::spoe::SpoeInputQueue;
using mm::comm::spoe::SpoeOutputSlot;

std::vector<uint8_t> frame(std::size_t bytes, uint8_t value) {
  return std::vector<uint8_t>(bytes, value);
}

TEST(SpoeOutputSlot, ReadsTheNewestFrame) {
  SpoeOutputSlot slot;
  slot.reset(2);
  slot.write(frame(2, 1));
  slot.write(frame(2, 2));
  const auto read = slot.read();
  EXPECT_EQ(std::vector<uint8_t>(read.begin(), read.end()), frame(2, 2));
}

TEST(SpoeOutputSlot, ReadsThePreviousFrameAgainWhenNothingNewArrived) {
  SpoeOutputSlot slot;
  slot.reset(2);
  slot.write(frame(2, 7));
  slot.read();
  const auto again = slot.read();
  EXPECT_EQ(std::vector<uint8_t>(again.begin(), again.end()), frame(2, 7));
}

TEST(SpoeOutputSlot, NeverShowsAHalfWrittenFrame) {
  // Each frame repeats one byte value. A torn read would mix two values in one frame.
  SpoeOutputSlot slot;
  constexpr std::size_t kBytes = 64;
  slot.reset(kBytes);
  std::atomic<bool> done{false};
  std::thread writer([&] {
    for (int i = 0; i < 100000; ++i) {
      slot.write(frame(kBytes, static_cast<uint8_t>(i)));
    }
    done.store(true);
  });
  int torn = 0;
  while (!done.load()) {
    const auto read = slot.read();
    const auto differs = [&read](uint8_t b) { return b != read[0]; };
    if (std::ranges::any_of(read, differs)) {
      ++torn;
    }
  }
  writer.join();
  EXPECT_EQ(torn, 0);
}

TEST(SpoeInputQueue, KeepsTheOrder) {
  SpoeInputQueue queue;
  queue.reset(2, 4);
  ASSERT_TRUE(queue.push(frame(2, 1)));
  ASSERT_TRUE(queue.push(frame(2, 2)));
  std::vector<uint8_t> out(2);
  ASSERT_TRUE(queue.pop(out));
  EXPECT_EQ(out, frame(2, 1));
  ASSERT_TRUE(queue.pop(out));
  EXPECT_EQ(out, frame(2, 2));
  EXPECT_FALSE(queue.pop(out));
  EXPECT_EQ(out, frame(2, 2));
}

TEST(SpoeInputQueue, RefusesAFrameWhenFull) {
  SpoeInputQueue queue;
  queue.reset(1, 2);
  EXPECT_TRUE(queue.push(frame(1, 1)));
  EXPECT_TRUE(queue.push(frame(1, 2)));
  EXPECT_FALSE(queue.push(frame(1, 3)));
  EXPECT_EQ(queue.size(), 2U);
}

TEST(SpoeInputQueue, DropsTheOldestFramesDownToTheLimit) {
  SpoeInputQueue queue;
  queue.reset(1, 8);
  for (uint8_t i = 1; i <= 5; ++i) {
    queue.push(frame(1, i));
  }
  EXPECT_EQ(queue.dropOldest(2), 3U);
  std::vector<uint8_t> out(1);
  ASSERT_TRUE(queue.pop(out));
  EXPECT_EQ(out[0], 4);
  EXPECT_EQ(queue.dropOldest(2), 0U);
}

TEST(SpoeInputQueue, PassesEveryFrameBetweenTwoThreads) {
  SpoeInputQueue queue;
  queue.reset(4, 16);
  constexpr uint32_t kFrames = 50000;
  std::thread producer([&] {
    for (uint32_t i = 0; i < kFrames;) {
      const std::vector<uint8_t> f{static_cast<uint8_t>(i), static_cast<uint8_t>(i >> 8),
                                   static_cast<uint8_t>(i >> 16), static_cast<uint8_t>(i >> 24)};
      if (queue.push(f)) {
        ++i;
      }
    }
  });
  uint32_t expected = 0;
  int outOfOrder = 0;
  std::vector<uint8_t> out(4);
  while (expected < kFrames) {
    if (queue.pop(out)) {
      const uint32_t got =
          out[0] | (out[1] << 8) | (out[2] << 16) | (static_cast<uint32_t>(out[3]) << 24);
      if (got != expected) {
        ++outOfOrder;
      }
      ++expected;
    }
  }
  producer.join();
  EXPECT_EQ(outOfOrder, 0);
}

TEST(SpoeFrameAssembler, SplitsAReplyIntoFrames) {
  SpoeFrameAssembler assembler;
  assembler.reset(3);
  EXPECT_EQ(assembler.add(std::vector<uint8_t>{1, 1, 1, 2, 2, 2}),
            (std::vector<uint8_t>{1, 1, 1, 2, 2, 2}));
  EXPECT_EQ(assembler.framingLosses(), 0U);
}

TEST(SpoeFrameAssembler, JoinsAFrameCutAtTheFiveHundredByteLimit) {
  // 21 frames of 24 bytes are 504 bytes. The first reply carries 500 of them and ends four bytes
  // into the last frame. The next reply carries those four bytes.
  SpoeFrameAssembler assembler;
  assembler.reset(24);
  std::vector<uint8_t> stream;
  for (uint8_t i = 0; i < 21; ++i) {
    const auto f = frame(24, i);
    stream.insert(stream.end(), f.begin(), f.end());
  }
  const auto first = assembler.add(std::span<const uint8_t>(stream).first(kInputPopLimit));
  EXPECT_EQ(first.size(), 20U * 24U);
  const auto second = assembler.add(std::span<const uint8_t>(stream).subspan(kInputPopLimit));
  EXPECT_EQ(second, frame(24, 20));
  EXPECT_EQ(assembler.framingLosses(), 0U);
}

TEST(SpoeFrameAssembler, DropsTheRestOfACutFrameTheDriveDiscarded) {
  // A reply ends inside a frame, then the drive's buffer overflows and is emptied, so the rest of
  // that frame never comes. The next short reply starts on a frame boundary. The stale part is
  // dropped, and the frames after it come out whole.
  SpoeFrameAssembler assembler;
  assembler.reset(24);
  std::vector<uint8_t> stream;
  for (uint8_t i = 0; i < 21; ++i) {
    const auto f = frame(24, i);
    stream.insert(stream.end(), f.begin(), f.end());
  }
  assembler.add(std::span<const uint8_t>(stream).first(kInputPopLimit));
  const auto next = assembler.add(frame(24, 99));
  EXPECT_EQ(next, frame(24, 99));
  EXPECT_EQ(assembler.framingLosses(), 1U);
}

}  // namespace
