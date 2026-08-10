#include "node/process_data_ring.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

using mm::node::ProcessDataRing;

// Writes a record whose input/output regions are filled with a recognisable, seq-derived pattern.
void writeSeq(ProcessDataRing& ring, uint64_t seq, uint32_t inBytes, uint32_t outBytes) {
  std::vector<uint8_t> in(inBytes, static_cast<uint8_t>(seq & 0xFF));
  std::vector<uint8_t> out(outBytes, static_cast<uint8_t>((seq >> 8) & 0xFF));
  ring.write(/*timestampNs=*/seq * 1000, /*wkc=*/3, in, out);
}

TEST(ProcessDataRingTest, UnallocatedIsInert) {
  ProcessDataRing ring;
  EXPECT_FALSE(ring.allocated());
  EXPECT_EQ(ring.head(), 0u);
  EXPECT_EQ(ring.capacity(), 0u);
  EXPECT_EQ(ring.oldestValidSeq(), 0u);
  ProcessDataRing::Record rec;
  EXPECT_FALSE(ring.readRecord(0, rec));
  // write() on an unallocated ring is a safe no-op.
  std::vector<uint8_t> bytes{1, 2, 3};
  ring.write(123, 3, bytes, bytes);
  EXPECT_EQ(ring.head(), 0u);
}

TEST(ProcessDataRingTest, WriteThenReadRoundTrips) {
  ProcessDataRing ring;
  ring.allocate(/*inputCap=*/6, /*outputCap=*/4, /*capacityCycles=*/8);
  ASSERT_TRUE(ring.allocated());
  EXPECT_EQ(ring.capacity(), 8u);

  ring.write(/*timestampNs=*/123456, /*wkc=*/3, std::vector<uint8_t>{1, 2, 3, 4, 5, 6},
             std::vector<uint8_t>{9, 8, 7, 6});
  ASSERT_EQ(ring.head(), 1u);

  ProcessDataRing::Record rec;
  ASSERT_TRUE(ring.readRecord(0, rec));
  EXPECT_EQ(rec.seq, 0u);
  EXPECT_EQ(rec.timestampNs, 123456u);
  EXPECT_EQ(rec.wkc, 3);
  EXPECT_EQ(rec.inputs, (std::vector<uint8_t>{1, 2, 3, 4, 5, 6}));
  EXPECT_EQ(rec.outputs, (std::vector<uint8_t>{9, 8, 7, 6}));
}

TEST(ProcessDataRingTest, OldestValidTracksWrap) {
  ProcessDataRing ring;
  ring.allocate(2, 2, 4);  // capacity 4

  EXPECT_EQ(ring.oldestValidSeq(), 0u);
  for (uint64_t s = 0; s < 4; ++s) {
    writeSeq(ring, s, 2, 2);
  }
  EXPECT_EQ(ring.head(), 4u);
  EXPECT_EQ(ring.oldestValidSeq(), 0u);  // exactly full, nothing overwritten yet

  writeSeq(ring, 4, 2, 2);  // wraps: overwrites slot of seq 0
  EXPECT_EQ(ring.head(), 5u);
  EXPECT_EQ(ring.oldestValidSeq(), 1u);

  // seq 0 is gone; seqs 1..4 are present.
  ProcessDataRing::Record rec;
  EXPECT_FALSE(ring.readRecord(0, rec));
  for (uint64_t s = 1; s <= 4; ++s) {
    ASSERT_TRUE(ring.readRecord(s, rec)) << "seq " << s;
    EXPECT_EQ(rec.inputs[0], static_cast<uint8_t>(s));
  }
}

TEST(ProcessDataRingTest, RejectsNeverWrittenAndFutureSeqs) {
  ProcessDataRing ring;
  ring.allocate(2, 2, 4);
  writeSeq(ring, 0, 2, 2);
  ProcessDataRing::Record rec;
  EXPECT_TRUE(ring.readRecord(0, rec));
  EXPECT_FALSE(ring.readRecord(1, rec));   // not written yet (head == 1)
  EXPECT_FALSE(ring.readRecord(99, rec));  // far future
}

TEST(ProcessDataRingTest, ReallocateRestartsRecording) {
  ProcessDataRing ring;
  ring.allocate(4, 4, 4);
  writeSeq(ring, 0, 4, 4);
  writeSeq(ring, 1, 4, 4);
  ASSERT_EQ(ring.head(), 2u);

  // A layout change re-allocates: sequence restarts at 0 and the old records are gone.
  ring.allocate(2, 2, 8);
  EXPECT_EQ(ring.head(), 0u);
  EXPECT_EQ(ring.capacity(), 8u);
  ProcessDataRing::Record rec;
  EXPECT_FALSE(ring.readRecord(0, rec));

  ring.write(7, 3, std::vector<uint8_t>{0xAA, 0xBB}, std::vector<uint8_t>{0xCC, 0xDD});
  ASSERT_TRUE(ring.readRecord(0, rec));
  EXPECT_EQ(rec.inputs, (std::vector<uint8_t>{0xAA, 0xBB}));
}

TEST(ProcessDataRingTest, ClampsOversizedPayload) {
  ProcessDataRing ring;
  ring.allocate(/*inputCap=*/2, /*outputCap=*/2, /*capacityCycles=*/2);
  // Pass more bytes than the per-direction capacity — must be clamped, not overflow.
  ring.write(1, 3, std::vector<uint8_t>{1, 2, 3, 4, 5}, std::vector<uint8_t>{6, 7, 8});
  ProcessDataRing::Record rec;
  ASSERT_TRUE(ring.readRecord(0, rec));
  EXPECT_EQ(rec.inputs.size(), 2u);
  EXPECT_EQ(rec.outputs.size(), 2u);
  EXPECT_EQ(rec.inputs, (std::vector<uint8_t>{1, 2}));
  EXPECT_EQ(rec.outputs, (std::vector<uint8_t>{6, 7}));
}

// Producer/consumer stress: a single writer floods the ring while a reader continuously copies the
// newest record. The reader must never observe a torn record (a value inconsistent with its seq).
TEST(ProcessDataRingTest, ConcurrentWriterAndReaderNeverTears) {
  ProcessDataRing ring;
  ring.allocate(/*inputCap=*/8, /*outputCap=*/8, /*capacityCycles=*/64);

  std::atomic<bool> stop{false};
  constexpr uint64_t kWrites = 200'000;

  std::thread writer([&] {
    for (uint64_t s = 0; s < kWrites; ++s) {
      // Every byte of both regions encodes the low byte of seq, so a torn record is detectable.
      const uint8_t b = static_cast<uint8_t>(s & 0xFF);
      std::vector<uint8_t> in(8, b);
      std::vector<uint8_t> out(8, b);
      ring.write(s, 3, in, out);
    }
    stop.store(true, std::memory_order_release);
  });

  uint64_t reads = 0;
  uint64_t hits = 0;
  ProcessDataRing::Record rec;
  while (!stop.load(std::memory_order_acquire)) {
    const uint64_t head = ring.head();
    if (head == 0) {
      continue;
    }
    ++reads;
    if (ring.readRecord(head - 1, rec)) {
      ++hits;
      const uint8_t expected = static_cast<uint8_t>(rec.seq & 0xFF);
      for (uint8_t v : rec.inputs) {
        ASSERT_EQ(v, expected) << "torn input at seq " << rec.seq;
      }
      for (uint8_t v : rec.outputs) {
        ASSERT_EQ(v, expected) << "torn output at seq " << rec.seq;
      }
    }
  }
  writer.join();
  EXPECT_GT(reads, 0u);
  EXPECT_GT(hits, 0u);  // the newest record (head-1) is the longest-lived; reads should mostly hit
}

}  // namespace
