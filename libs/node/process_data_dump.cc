#include "node/process_data_dump.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "core/util.h"

namespace mm::node {
namespace {

// Appends an integer to the stream in little-endian byte order, width sizeof(T).
template <typename T>
void putLE(std::ostream& o, T value) {
  const auto bytes = mm::core::toBytes(value);
  o.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// Appends a length-prefixed string: u16 byte count (clamped to 0xFFFF) then the bytes.
void putStr(std::ostream& o, const std::string& s) {
  const auto n = static_cast<uint16_t>(std::min<size_t>(s.size(), 0xFFFFu));
  putLE<uint16_t>(o, n);
  o.write(s.data(), n);
}

// Appends exactly `cap` bytes from `src` (truncating if longer, zero-padding if shorter), so the
// row stride stays fixed at the header's region sizes regardless of a record's own size.
void putRegion(std::ostream& o, const std::vector<uint8_t>& src, uint32_t cap) {
  const uint32_t n = std::min<uint32_t>(static_cast<uint32_t>(src.size()), cap);
  o.write(reinterpret_cast<const char*>(src.data()), n);
  for (uint32_t i = n; i < cap; ++i) {
    o.put('\0');
  }
}

void writeHeader(std::ostream& o, const DumpHeader& header, uint64_t startSeq) {
  o.write(kDumpMagic.data(), kDumpMagic.size());
  putLE<uint16_t>(o, kDumpFormatVersion);
  putLE<uint16_t>(o, 0);  // flags (reserved)
  putLE<uint32_t>(o, header.cyclePeriodUs);
  putLE<uint64_t>(o, startSeq);
  putLE<uint64_t>(o, 0);  // rowCount placeholder, patched after the rows are streamed
  putLE<uint32_t>(o, header.inputBytes);
  putLE<uint32_t>(o, header.outputBytes);
  putLE<uint32_t>(o, static_cast<uint32_t>(header.devices.size()));

  for (const DumpDevice& d : header.devices) {
    putLE<uint16_t>(o, d.slavePosition);
    putLE<uint32_t>(o, d.vendorId);
    putLE<uint32_t>(o, d.productCode);
    putLE<uint32_t>(o, d.revisionNumber);
    putLE<uint32_t>(o, d.serialNumber);
    putStr(o, d.name);
    putLE<uint32_t>(o, static_cast<uint32_t>(d.entries.size()));
    for (const DumpPdoEntry& e : d.entries) {
      putLE<uint16_t>(o, e.index);
      putLE<uint8_t>(o, e.subindex);
      putLE<uint8_t>(o, static_cast<uint8_t>(e.isOutput ? 1 : 0));
      putLE<uint16_t>(o, e.dataType);
      putLE<uint16_t>(o, e.bitLength);
      putLE<uint32_t>(o, e.bitOffset);
      putStr(o, e.name);
    }
  }
}

}  // namespace

std::expected<uint64_t, std::string> writeProcessDataDump(std::ostream& out,
                                                          const DumpHeader& header,
                                                          uint64_t startSeq, uint64_t endSeq,
                                                          const DumpRecordReader& read) {
  writeHeader(out, header, startSeq);

  uint64_t written = 0;
  ProcessDataRing::Record rec;
  for (uint64_t seq = startSeq; seq < endSeq; ++seq) {
    if (!read(seq, rec)) {
      continue;  // lapped or torn — skip; the per-row sequence makes the gap self-describing
    }
    putLE<uint64_t>(out, rec.seq);
    putLE<uint64_t>(out, rec.timestampNs);
    putRegion(out, rec.inputs, header.inputBytes);
    putRegion(out, rec.outputs, header.outputBytes);
    ++written;
  }

  // Patch the real row count into the fixed prefix slot now that it is known.
  out.seekp(kDumpRowCountOffset, std::ios::beg);
  putLE<uint64_t>(out, written);
  out.seekp(0, std::ios::end);

  if (!out.good()) {
    return std::unexpected("failed to write process-data dump (stream error)");
  }
  return written;
}

}  // namespace mm::node
