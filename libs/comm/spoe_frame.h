#pragma once

// The SPoE wire format: a 7-byte header and a data part. Pure functions with no I/O, so the
// connection and the tests share one definition of the bytes.
//
// The layout follows the netX firmware, `AppSockIf_MessageHandler.h`, `msg_frame_t`. Every field is
// little-endian and there is no padding between them.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mm::comm::spoe {

/// The message types the firmware handles. A reply carries the type of its request.
enum class MessageType : uint8_t {
  kSdoRead = 0x01,
  kSdoWrite = 0x02,
  kPdoFrame = 0x03,
  kPdoControl = 0x04,
  kSdoBatchRead = 0x06,
  kFirmwareUpdate = 0x0B,
  kFileRead = 0x0C,
  kFileWrite = 0x0D,
  kStateControl = 0x0E,
  kStateRead = 0x0F,
  kParamList = 0x10,
  kParamDesc = 0x11,
  kParamSubDesc = 0x12,
  kParamFullDesc = 0x13,
  kServerInfo = 0x20,
  kDeviceLocate = 0x21,
  kWatchdogTimeout = 0x22,
};

/// Message type u8, sequence id u16, status u16, data length u16.
constexpr std::size_t kHeaderSize = 7;

/// `MAX_FRAME_SIZE` in the firmware: the size of its buffer for the data part.
constexpr std::size_t kMaxDataSize = 512;

struct FrameHeader {
  uint8_t type = 0;
  uint16_t sequenceId = 0;
  uint16_t status = 0;
  uint16_t dataLength = 0;
};

/// One request or reply. @c type stays a raw byte, because a reply can carry a type that
/// @c MessageType does not name.
struct Frame {
  uint8_t type = 0;
  uint16_t sequenceId = 0;
  uint16_t status = 0;
  std::vector<uint8_t> data;
};

/// Encodes @p frame. Fails when the data part is larger than @c kMaxDataSize.
std::expected<std::vector<uint8_t>, std::string> encodeFrame(const Frame& frame);

/// Decodes the header at the start of @p bytes, or nullopt when fewer than @c kHeaderSize bytes
/// are there.
std::optional<FrameHeader> decodeHeader(std::span<const uint8_t> bytes);

}  // namespace mm::comm::spoe
