#include "capture/pcap.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace mm::capture {

namespace {

constexpr uint32_t kMagicMicroseconds = 0xA1B2C3D4;
constexpr uint32_t kMagicNanoseconds = 0xA1B23C4D;
constexpr uint32_t kLinkTypeEthernet = 1;
constexpr uint32_t kSnapLength = 65535;
constexpr std::size_t kFileHeaderBytes = 24;
constexpr std::size_t kRecordHeaderBytes = 16;

void putU32(uint8_t* out, uint32_t v) {
  out[0] = static_cast<uint8_t>(v);
  out[1] = static_cast<uint8_t>(v >> 8);
  out[2] = static_cast<uint8_t>(v >> 16);
  out[3] = static_cast<uint8_t>(v >> 24);
}

void putU16(uint8_t* out, uint16_t v) {
  out[0] = static_cast<uint8_t>(v);
  out[1] = static_cast<uint8_t>(v >> 8);
}

uint32_t getU32(const uint8_t* in, bool swap) {
  const uint32_t v = static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
                     (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
  if (!swap) {
    return v;
  }
  return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) | ((v & 0x00FF0000u) >> 8) |
         ((v & 0xFF000000u) >> 24);
}

}  // namespace

PcapWriter::~PcapWriter() { close(); }

std::expected<void, std::string> PcapWriter::open(const std::filesystem::path& path) {
  close();
  file_ = std::fopen(path.string().c_str(), "wb");
  if (file_ == nullptr) {
    return std::unexpected("cannot create " + path.string());
  }
  std::array<uint8_t, kFileHeaderBytes> header{};
  putU32(&header[0], kMagicNanoseconds);
  putU16(&header[4], 2);   // major version
  putU16(&header[6], 4);   // minor version
  putU32(&header[8], 0);   // time zone offset, always 0
  putU32(&header[12], 0);  // timestamp accuracy, always 0
  putU32(&header[16], kSnapLength);
  putU32(&header[20], kLinkTypeEthernet);
  if (std::fwrite(header.data(), 1, header.size(), file_) != header.size()) {
    close();
    return std::unexpected("cannot write the header of " + path.string());
  }
  return {};
}

std::expected<void, std::string> PcapWriter::write(int64_t timestampNs,
                                                   std::span<const uint8_t> bytes) {
  if (file_ == nullptr) {
    return std::unexpected("the pcap file is not open");
  }
  std::array<uint8_t, kRecordHeaderBytes> header{};
  putU32(&header[0], static_cast<uint32_t>(timestampNs / 1'000'000'000));
  putU32(&header[4], static_cast<uint32_t>(timestampNs % 1'000'000'000));
  putU32(&header[8], static_cast<uint32_t>(bytes.size()));
  putU32(&header[12], static_cast<uint32_t>(bytes.size()));
  if (std::fwrite(header.data(), 1, header.size(), file_) != header.size() ||
      std::fwrite(bytes.data(), 1, bytes.size(), file_) != bytes.size()) {
    return std::unexpected("cannot write to the pcap file");
  }
  return {};
}

void PcapWriter::close() {
  if (file_ != nullptr) {
    std::fclose(file_);
    file_ = nullptr;
  }
}

std::expected<std::vector<Record>, std::string> parsePcap(std::span<const uint8_t> bytes) {
  if (bytes.size() < kFileHeaderBytes) {
    return std::unexpected("shorter than a pcap header");
  }
  const uint32_t magicAsIs = getU32(&bytes[0], false);
  bool swap = false;
  bool nanoseconds = false;
  if (magicAsIs == kMagicMicroseconds || magicAsIs == kMagicNanoseconds) {
    nanoseconds = magicAsIs == kMagicNanoseconds;
  } else {
    const uint32_t magicSwapped = getU32(&bytes[0], true);
    if (magicSwapped != kMagicMicroseconds && magicSwapped != kMagicNanoseconds) {
      return std::unexpected("not a pcap file");
    }
    swap = true;
    nanoseconds = magicSwapped == kMagicNanoseconds;
  }
  if (getU32(&bytes[20], swap) != kLinkTypeEthernet) {
    return std::unexpected("not an Ethernet capture");
  }
  std::vector<Record> records;
  std::size_t at = kFileHeaderBytes;
  while (at + kRecordHeaderBytes <= bytes.size()) {
    const uint32_t seconds = getU32(&bytes[at], swap);
    const uint32_t fraction = getU32(&bytes[at + 4], swap);
    const uint32_t captured = getU32(&bytes[at + 8], swap);
    at += kRecordHeaderBytes;
    if (at + captured > bytes.size()) {
      break;  // a record cut short at the end of the file, the way an interrupted capture ends
    }
    Record r;
    r.timestampNs = static_cast<int64_t>(seconds) * 1'000'000'000 +
                    (nanoseconds ? fraction : static_cast<int64_t>(fraction) * 1000);
    r.bytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                   bytes.begin() + static_cast<std::ptrdiff_t>(at + captured));
    at += captured;
    records.push_back(std::move(r));
  }
  return records;
}

std::expected<std::vector<Record>, std::string> readPcap(const std::filesystem::path& path) {
  std::FILE* file = std::fopen(path.string().c_str(), "rb");
  if (file == nullptr) {
    return std::unexpected("cannot open " + path.string());
  }
  std::vector<uint8_t> bytes;
  std::array<uint8_t, 65536> chunk{};
  for (;;) {
    const std::size_t n = std::fread(chunk.data(), 1, chunk.size(), file);
    if (n == 0) {
      break;
    }
    bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n));
  }
  std::fclose(file);
  auto records = parsePcap(bytes);
  if (!records) {
    return std::unexpected(path.string() + " is " + records.error());
  }
  return records;
}

}  // namespace mm::capture
