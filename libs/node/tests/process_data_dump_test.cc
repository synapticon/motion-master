#include "node/process_data_dump.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "node/process_data_ring.h"

namespace {

using mm::node::DumpDevice;
using mm::node::DumpHeader;
using mm::node::DumpPdoEntry;
using mm::node::kDumpFormatVersion;
using mm::node::kDumpMagic;
using mm::node::ProcessDataRing;
using mm::node::writeProcessDataDump;

// A minimal little-endian reader over the serialized bytes, mirroring the writer's layout so the
// test decodes a dump exactly as an offline tool would.
class DumpReader {
 public:
  explicit DumpReader(std::string bytes) : bytes_(std::move(bytes)) {}

  template <typename T>
  T getLE() {
    uint64_t value = 0;
    for (size_t i = 0; i < sizeof(T); ++i) {
      value |= static_cast<uint64_t>(static_cast<uint8_t>(bytes_.at(pos_++))) << (8 * i);
    }
    return static_cast<T>(value);
  }

  std::string getStr() {
    auto n = getLE<uint16_t>();
    std::string s = bytes_.substr(pos_, n);
    pos_ += n;
    return s;
  }

  std::vector<uint8_t> getBytes(size_t n) {
    const auto at = static_cast<std::ptrdiff_t>(pos_);
    std::vector<uint8_t> v(bytes_.begin() + at,
                           bytes_.begin() + at + static_cast<std::ptrdiff_t>(n));
    pos_ += n;
    return v;
  }

  void skip(size_t n) { pos_ += n; }
  size_t pos() const { return pos_; }
  size_t size() const { return bytes_.size(); }

 private:
  std::string bytes_;
  size_t pos_ = 0;
};

// Fills a record's regions with a recognisable, seq-derived pattern and writes it.
void writeSeq(ProcessDataRing& ring, uint64_t seq, uint32_t inBytes, uint32_t outBytes) {
  std::vector<uint8_t> in(inBytes, static_cast<uint8_t>(seq & 0xFF));
  std::vector<uint8_t> out(outBytes, static_cast<uint8_t>((seq >> 8) & 0xFF));
  ring.write(/*timestampNs=*/seq * 1000 + 7, /*wkc=*/3, in, out);
}

DumpHeader makeHeader(uint32_t inputBytes, uint32_t outputBytes) {
  DumpHeader h;
  h.inputBytes = inputBytes;
  h.outputBytes = outputBytes;
  DumpDevice d{.slavePosition = 1,
               .vendorId = 0x000022D2,
               .productCode = 0x00000201,
               .revisionNumber = 0x0A000002,
               .serialNumber = 0xDEADBEEF,
               .name = "SOMANET",
               .entries = {}};
  d.entries.push_back(DumpPdoEntry{.index = 0x6041,
                                   .subindex = 0,
                                   .isOutput = false,
                                   .dataType = 0x0006,
                                   .bitLength = 16,
                                   .bitOffset = 0,
                                   .name = "Statusword"});
  d.entries.push_back(DumpPdoEntry{.index = 0x6040,
                                   .subindex = 0,
                                   .isOutput = true,
                                   .dataType = 0x0006,
                                   .bitLength = 16,
                                   .bitOffset = 0,
                                   .name = "Controlword"});
  h.devices.push_back(std::move(d));
  return h;
}

TEST(ProcessDataDumpTest, RoundTripsHeaderAndRows) {
  constexpr uint32_t kIn = 6, kOut = 4;
  ProcessDataRing ring;
  ring.allocate(kIn, kOut, /*capacityCycles=*/64);
  for (uint64_t seq = 0; seq < 10; ++seq) {
    writeSeq(ring, seq, kIn, kOut);
  }

  DumpHeader header = makeHeader(kIn, kOut);
  std::ostringstream out(std::ios::binary);
  auto written = writeProcessDataDump(
      out, header, ring.oldestValidSeq(), ring.head(),
      [&ring](uint64_t seq, ProcessDataRing::Record& rec) { return ring.readRecord(seq, rec); });
  ASSERT_TRUE(written.has_value()) << written.error();
  EXPECT_EQ(*written, 10u);

  DumpReader r(out.str());
  // Magic + version + flags.
  EXPECT_EQ(r.getBytes(4), std::vector<uint8_t>(kDumpMagic.begin(), kDumpMagic.end()));
  EXPECT_EQ(r.getLE<uint16_t>(), kDumpFormatVersion);
  EXPECT_EQ(r.getLE<uint16_t>(), 0u);    // flags
  EXPECT_EQ(r.getLE<uint64_t>(), 0u);    // startSequence
  EXPECT_EQ(r.getLE<uint64_t>(), 10u);   // rowCount (patched)
  EXPECT_EQ(r.getLE<uint32_t>(), kIn);   // inputBytes
  EXPECT_EQ(r.getLE<uint32_t>(), kOut);  // outputBytes
  ASSERT_EQ(r.getLE<uint32_t>(), 1u);    // deviceCount

  // Device record.
  EXPECT_EQ(r.getLE<uint16_t>(), 1u);           // slavePosition
  EXPECT_EQ(r.getLE<uint32_t>(), 0x000022D2u);  // vendorId
  EXPECT_EQ(r.getLE<uint32_t>(), 0x00000201u);  // productCode
  EXPECT_EQ(r.getLE<uint32_t>(), 0x0A000002u);  // revisionNumber
  EXPECT_EQ(r.getLE<uint32_t>(), 0xDEADBEEFu);  // serialNumber
  EXPECT_EQ(r.getStr(), "SOMANET");
  ASSERT_EQ(r.getLE<uint32_t>(), 2u);  // entryCount

  // Entry 0: Statusword (input).
  EXPECT_EQ(r.getLE<uint16_t>(), 0x6041u);
  EXPECT_EQ(r.getLE<uint8_t>(), 0u);
  EXPECT_EQ(r.getLE<uint8_t>(), 0u);  // direction: input
  EXPECT_EQ(r.getLE<uint16_t>(), 0x0006u);
  EXPECT_EQ(r.getLE<uint16_t>(), 16u);
  EXPECT_EQ(r.getLE<uint32_t>(), 0u);
  EXPECT_EQ(r.getStr(), "Statusword");
  // Entry 1: Controlword (output).
  EXPECT_EQ(r.getLE<uint16_t>(), 0x6040u);
  EXPECT_EQ(r.getLE<uint8_t>(), 0u);
  EXPECT_EQ(r.getLE<uint8_t>(), 1u);  // direction: output
  EXPECT_EQ(r.getLE<uint16_t>(), 0x0006u);
  EXPECT_EQ(r.getLE<uint16_t>(), 16u);
  EXPECT_EQ(r.getLE<uint32_t>(), 0u);
  EXPECT_EQ(r.getStr(), "Controlword");

  // Rows: fixed stride, in sequence order, with the seq-derived payload.
  for (uint64_t seq = 0; seq < 10; ++seq) {
    EXPECT_EQ(r.getLE<uint64_t>(), seq);             // sequence
    EXPECT_EQ(r.getLE<uint64_t>(), seq * 1000 + 7);  // timestampNs
    EXPECT_EQ(r.getBytes(kIn), std::vector<uint8_t>(kIn, static_cast<uint8_t>(seq & 0xFF)));
    EXPECT_EQ(r.getBytes(kOut),
              std::vector<uint8_t>(kOut, static_cast<uint8_t>((seq >> 8) & 0xFF)));
  }
  EXPECT_EQ(r.pos(), r.size());  // consumed exactly, no trailing bytes
}

TEST(ProcessDataDumpTest, EmptySpanWritesHeaderOnlyWithZeroRows) {
  ProcessDataRing ring;
  ring.allocate(4, 4, 8);
  DumpHeader header = makeHeader(4, 4);
  std::ostringstream out(std::ios::binary);
  // head == oldest == 0: an empty [0, 0) span.
  auto written = writeProcessDataDump(
      out, header, 0, 0,
      [&ring](uint64_t seq, ProcessDataRing::Record& rec) { return ring.readRecord(seq, rec); });
  ASSERT_TRUE(written.has_value());
  EXPECT_EQ(*written, 0u);

  DumpReader r(out.str());
  r.skip(16);                          // magic..startSequence
  EXPECT_EQ(r.getLE<uint64_t>(), 0u);  // rowCount patched to 0
}

TEST(ProcessDataDumpTest, PadsAndTruncatesRegionsToHeaderSizes) {
  // The ring holds 8/8-byte regions, but the header declares 4 input / 12 output bytes; the writer
  // must truncate inputs and zero-pad outputs so every row matches the header stride.
  constexpr uint32_t kRingIn = 8, kRingOut = 8;
  ProcessDataRing ring;
  ring.allocate(kRingIn, kRingOut, 8);
  std::vector<uint8_t> in(kRingIn, 0xAB);
  std::vector<uint8_t> out(kRingOut, 0xCD);
  ring.write(42, 3, in, out);

  DumpHeader header = makeHeader(/*inputBytes=*/4, /*outputBytes=*/12);
  std::ostringstream os(std::ios::binary);
  auto written = writeProcessDataDump(
      os, header, ring.oldestValidSeq(), ring.head(),
      [&ring](uint64_t seq, ProcessDataRing::Record& rec) { return ring.readRecord(seq, rec); });
  ASSERT_TRUE(written.has_value());
  EXPECT_EQ(*written, 1u);

  DumpReader r(os.str());
  r.skip(24);                           // magic..startSequence + rowCount (up to inputBytes)
  EXPECT_EQ(r.getLE<uint32_t>(), 4u);   // inputBytes
  EXPECT_EQ(r.getLE<uint32_t>(), 12u);  // outputBytes
  ASSERT_EQ(r.getLE<uint32_t>(), 1u);   // deviceCount
  // Walk past the one device record (slavePosition, four u32 ids, name, entries).
  r.getLE<uint16_t>();  // slavePosition
  r.skip(16);           // vendorId, productCode, revisionNumber, serialNumber
  r.getStr();           // name
  ASSERT_EQ(r.getLE<uint32_t>(), 2u);
  for (int i = 0; i < 2; ++i) {
    r.skip(2 + 1 + 1 + 2 + 2 + 4);  // index..bitOffset
    r.getStr();                     // entry name
  }
  // Single row: input truncated to 4 bytes, output zero-padded to 12.
  EXPECT_EQ(r.getLE<uint64_t>(), 0u);   // sequence
  EXPECT_EQ(r.getLE<uint64_t>(), 42u);  // timestampNs
  EXPECT_EQ(r.getBytes(4), std::vector<uint8_t>(4, 0xAB));
  std::vector<uint8_t> expectedOut(8, 0xCD);
  expectedOut.resize(12, 0x00);
  EXPECT_EQ(r.getBytes(12), expectedOut);
  EXPECT_EQ(r.pos(), r.size());
}

TEST(ProcessDataDumpTest, SkipsUnreadableRecordsAndCountsRest) {
  // A reader that fails for odd sequences leaves gaps: rowCount counts only the rows written, and
  // each row self-describes via its sequence (no gap markers).
  DumpHeader header = makeHeader(2, 2);
  std::ostringstream out(std::ios::binary);
  auto reader = [](uint64_t seq, ProcessDataRing::Record& rec) {
    if (seq % 2 != 0) return false;  // simulate a lapped/torn record
    rec.seq = seq;
    rec.timestampNs = seq;
    rec.inputs = {static_cast<uint8_t>(seq), 0};
    rec.outputs = {0, 0};
    return true;
  };
  auto written = writeProcessDataDump(out, header, 0, 6, reader);
  ASSERT_TRUE(written.has_value());
  EXPECT_EQ(*written, 3u);  // seqs 0, 2, 4

  DumpReader r(out.str());
  r.skip(16);
  EXPECT_EQ(r.getLE<uint64_t>(), 3u);  // rowCount
}

}  // namespace
