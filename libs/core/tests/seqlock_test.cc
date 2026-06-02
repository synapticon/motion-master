#include "core/seqlock.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

using mm::core::SeqLock;

namespace {

// A multi-word payload whose words must always agree.  A torn read (a copy that
// captured part of one write and part of the next) shows up as words that differ,
// which the torture test below asserts never reaches a reader.
struct Payload {
  std::array<uint64_t, 64> words;
};

}  // namespace

TEST(SeqLockTest, DefaultIsZeroInitialised) {
  SeqLock<Payload> lock;
  Payload p = lock.load();
  for (uint64_t w : p.words) {
    EXPECT_EQ(w, 0u);
  }
}

TEST(SeqLockTest, RoundTripsAValue) {
  SeqLock<Payload> lock;
  Payload in;
  in.words.fill(0xDEADBEEFCAFEF00Dull);
  lock.store(in);

  Payload out = lock.load();
  EXPECT_EQ(out.words, in.words);
}

TEST(SeqLockTest, LoadIntoCallerBuffer) {
  SeqLock<uint32_t> lock;
  lock.store(0x12345678u);
  uint32_t out = 0;
  lock.load(out);
  EXPECT_EQ(out, 0x12345678u);
}

// One writer mutates a multi-word invariant (all words equal a monotonically
// increasing counter) while several readers continuously snapshot it.  Every
// snapshot a reader *uses* must be internally consistent — all words equal — even
// though the writer is racing.  This is the property the RT process-data path
// relies on: a consumer never observes a half-updated IO image.
TEST(SeqLockTest, ConcurrentReadersNeverSeeATornValue) {
  SeqLock<Payload> lock;
  std::atomic<bool> stop{false};
  constexpr uint64_t kWrites = 2'000'000;

  std::thread writer([&] {
    for (uint64_t i = 1; i <= kWrites; ++i) {
      Payload p;
      p.words.fill(i);
      lock.store(p);
    }
    stop.store(true, std::memory_order_relaxed);
  });

  constexpr int kReaders = 4;
  std::vector<std::thread> readers;
  std::array<std::atomic<bool>, kReaders> tornSeen{};
  std::array<std::atomic<uint64_t>, kReaders> lastSeen{};
  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&, r] {
      Payload p;
      while (!stop.load(std::memory_order_relaxed)) {
        lock.load(p);
        const uint64_t first = p.words[0];
        if (std::any_of(p.words.begin(), p.words.end(),
                        [first](uint64_t w) { return w != first; })) {
          tornSeen[r].store(true, std::memory_order_relaxed);
        }
        lastSeen[r].store(first, std::memory_order_relaxed);
      }
    });
  }

  writer.join();
  for (auto& t : readers) {
    t.join();
  }

  for (int r = 0; r < kReaders; ++r) {
    EXPECT_FALSE(tornSeen[r].load()) << "reader " << r << " observed a torn snapshot";
    // Sanity: each reader made progress and saw a value within the written range.
    const uint64_t last = lastSeen[r].load();
    EXPECT_GT(last, 0u) << "reader " << r << " never observed a write";
    EXPECT_LE(last, kWrites) << "reader " << r << " observed an out-of-range value";
  }
}

// Mirrors the production payload: a fixed-capacity byte image the size of a fully
// loaded bus.  Confirms a snapshot of that size round-trips intact.
TEST(SeqLockTest, LargeProcessImagePayloadRoundTrips) {
  struct ProcessImageBytes {
    uint32_t size;
    std::array<uint8_t, 8192> bytes;
  };
  static_assert(std::is_trivially_copyable_v<ProcessImageBytes>);

  SeqLock<ProcessImageBytes> lock;
  ProcessImageBytes in{};
  in.size = 5120;  // 32 devices x 160 bytes per direction
  for (uint32_t i = 0; i < in.size; ++i) {
    in.bytes[i] = static_cast<uint8_t>(i * 31u + 7u);
  }
  lock.store(in);

  ProcessImageBytes out{};
  lock.load(out);
  EXPECT_EQ(out.size, in.size);
  EXPECT_EQ(out.bytes, in.bytes);
}

// The prefix store/load overloads copy only the leading bytes the caller names: the live region
// round-trips while the destination's tail is left intact, so the per-cycle copy is proportional
// to the real image rather than the full fixed capacity.
TEST(SeqLockTest, PrefixStoreAndLoadCopyOnlyTheLeadingBytes) {
  struct Buf {
    uint32_t size;
    std::array<uint8_t, 4096> bytes;
  };
  static_assert(std::is_trivially_copyable_v<Buf>);

  SeqLock<Buf> lock;
  Buf in{};
  in.size = 3;
  in.bytes[0] = 0xAA;
  in.bytes[1] = 0xBB;
  in.bytes[2] = 0xCC;
  in.bytes[3] = 0xDD;                            // beyond the live prefix — must not be published
  const size_t live = offsetof(Buf, bytes) + 3;  // size field + 3 live bytes
  lock.store(in, live);

  Buf out{};
  out.bytes[3] = 0x99;  // pre-existing tail content the prefix load must leave intact
  lock.load(out, live);
  EXPECT_EQ(out.size, 3u);
  EXPECT_EQ(out.bytes[0], 0xAA);
  EXPECT_EQ(out.bytes[1], 0xBB);
  EXPECT_EQ(out.bytes[2], 0xCC);
  EXPECT_EQ(out.bytes[3], 0x99);  // untouched by the prefix copy
}

// A byte count larger than the payload is clamped, never over-reading or over-writing.
TEST(SeqLockTest, PrefixCountIsClampedToPayloadSize) {
  SeqLock<uint32_t> lock;
  lock.store(0x12345678u, 999);  // clamped to sizeof(uint32_t)
  uint32_t out = 0;
  lock.load(out, 999);
  EXPECT_EQ(out, 0x12345678u);
}
