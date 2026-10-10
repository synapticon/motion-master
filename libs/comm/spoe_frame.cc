#include "comm/spoe_frame.h"

#include <array>
#include <format>
#include <string>
#include <vector>

namespace mm::comm::spoe {

namespace {

uint16_t readU16(std::span<const uint8_t> bytes, std::size_t offset) {
  return static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

}  // namespace

std::expected<std::vector<uint8_t>, std::string> encodeFrame(const Frame& frame) {
  if (frame.data.size() > kMaxDataSize) {
    return std::unexpected(std::format("SPoE frame data of {} bytes exceeds the {}-byte limit",
                                       frame.data.size(), kMaxDataSize));
  }
  const auto length = static_cast<uint16_t>(frame.data.size());
  const std::array<uint8_t, kHeaderSize> header{frame.type,
                                                static_cast<uint8_t>(frame.sequenceId),
                                                static_cast<uint8_t>(frame.sequenceId >> 8),
                                                static_cast<uint8_t>(frame.status),
                                                static_cast<uint8_t>(frame.status >> 8),
                                                static_cast<uint8_t>(length),
                                                static_cast<uint8_t>(length >> 8)};
  std::vector<uint8_t> bytes(header.begin(), header.end());
  bytes.insert(bytes.end(), frame.data.begin(), frame.data.end());
  return bytes;
}

std::optional<FrameHeader> decodeHeader(std::span<const uint8_t> bytes) {
  if (bytes.size() < kHeaderSize) {
    return std::nullopt;
  }
  return FrameHeader{.type = bytes[0],
                     .sequenceId = readU16(bytes, 1),
                     .status = readU16(bytes, 3),
                     .dataLength = readU16(bytes, 5)};
}

}  // namespace mm::comm::spoe
