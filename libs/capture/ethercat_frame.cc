#include "capture/ethercat_frame.h"

#include <cstddef>

namespace mm::capture {

namespace {

constexpr std::size_t kEthernetHeaderBytes = 14;
constexpr std::size_t kEtherCatHeaderBytes = 2;
constexpr std::size_t kDatagramHeaderBytes = 10;
constexpr std::size_t kWorkingCounterBytes = 2;
constexpr uint8_t kSourceMacLocalBit = 0x02;

uint16_t readU16(std::span<const uint8_t> bytes, std::size_t at) {
  return static_cast<uint16_t>(bytes[at] | (static_cast<uint16_t>(bytes[at + 1]) << 8));
}

}  // namespace

bool isLogicalCommand(uint8_t command) {
  // ETG.1000.4 Table 9: 10 LRD, 11 LWR, 12 LRW.
  return command >= 10 && command <= 12;
}

bool isPositionCountedUp(uint8_t command) {
  // ETG.1000.4 Table 9: 1 APRD, 2 APWR, 3 APRW, 7 BRD, 8 BWR, 9 BRW, 13 ARMW. Every slave adds one
  // to the position of these as the frame passes.
  switch (command) {
    case 1:
    case 2:
    case 3:
    case 7:
    case 8:
    case 9:
    case 13:
      return true;
    default:
      return false;
  }
}

std::optional<EtherCatFrame> parseEtherCatFrame(std::span<const uint8_t> bytes) {
  if (bytes.size() < kEthernetHeaderBytes + kEtherCatHeaderBytes) {
    return std::nullopt;
  }
  // Ethernet type is big-endian on the wire, the one field in the frame that is.
  const uint16_t etherType = static_cast<uint16_t>((bytes[12] << 8) | bytes[13]);
  if (etherType != kEtherCatEtherType) {
    return std::nullopt;
  }
  // EtherCAT header: 11 bits of length, one reserved bit, 4 bits of type. Type 1 is a frame of
  // EtherCAT commands; the other types carry other protocols and are not read here.
  const uint16_t header = readU16(bytes, kEthernetHeaderBytes);
  const std::size_t length = header & 0x07FF;
  const unsigned type = header >> 12;
  if (type != 1) {
    return std::nullopt;
  }
  std::size_t at = kEthernetHeaderBytes + kEtherCatHeaderBytes;
  const std::size_t end = at + length;
  if (end > bytes.size()) {
    return std::nullopt;
  }

  EtherCatFrame frame;
  frame.fromMaster = (bytes[6] & kSourceMacLocalBit) == 0;
  bool more = true;
  while (more) {
    if (at + kDatagramHeaderBytes > end) {
      return std::nullopt;
    }
    Datagram d;
    d.command = bytes[at];
    d.index = bytes[at + 1];
    d.adp = readU16(bytes, at + 2);
    d.ado = readU16(bytes, at + 4);
    // 11 bits of length, bit 14 says the frame is circulating, bit 15 says another datagram
    // follows.
    const uint16_t lengthField = readU16(bytes, at + 6);
    d.length = lengthField & 0x07FF;
    d.more = (lengthField & 0x8000) != 0;
    at += kDatagramHeaderBytes + d.length;
    if (at + kWorkingCounterBytes > end) {
      return std::nullopt;
    }
    d.wkc = readU16(bytes, at);
    at += kWorkingCounterBytes;
    more = d.more;
    frame.datagrams.push_back(d);
  }
  return frame;
}

}  // namespace mm::capture
