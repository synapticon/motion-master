#include "etg/fsoe_frame.h"

#include <array>
#include <cstddef>

#include "etg/fsoe_crc.h"

namespace mm::etg {
namespace {

/// Returns whether @c count octets starting at @c off lie inside @c size.
constexpr bool fits(size_t size, uint16_t off, uint16_t count) {
  return static_cast<size_t>(off) + count <= size;
}

/// Returns whether block @c k exists in this layout. Every accessor below checks this rather than
/// trusting the caller: an out-of-range index computes a plausible offset past the end of a short
/// frame, which reads as data instead of as an error.
constexpr bool blockInRange(const FsoeFrameLayout& layout, uint16_t k) {
  const uint16_t m = layout.blockCount();
  return m != 0 && k < m;
}

constexpr uint16_t readLe16(std::span<const uint8_t> pdu, uint16_t off) {
  return static_cast<uint16_t>(static_cast<uint16_t>(pdu[off]) |
                               (static_cast<uint16_t>(pdu[off + 1]) << 8));
}

constexpr void writeLe16(std::span<uint8_t> pdu, uint16_t off, uint16_t v) {
  pdu[off] = static_cast<uint8_t>(v & 0xFF);
  pdu[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

}  // namespace

uint8_t fsoeFrameCommand(std::span<const uint8_t> pdu) { return pdu.empty() ? 0 : pdu[0]; }

void fsoeFrameSetCommand(std::span<uint8_t> pdu, uint8_t command) {
  if (!pdu.empty()) {
    pdu[0] = command;
  }
}

uint16_t fsoeFrameConnId(std::span<const uint8_t> pdu, const FsoeFrameLayout& layout) {
  const uint16_t off = layout.connIdOffset();
  return fits(pdu.size(), off, 2) ? readLe16(pdu, off) : 0;
}

void fsoeFrameSetConnId(std::span<uint8_t> pdu, const FsoeFrameLayout& layout, uint16_t connId) {
  const uint16_t off = layout.connIdOffset();
  if (fits(pdu.size(), off, 2)) {
    writeLe16(pdu, off, connId);
  }
}

uint8_t fsoeFrameReadBlock(std::span<const uint8_t> pdu, const FsoeFrameLayout& layout, uint16_t k,
                           std::span<uint8_t> out) {
  const auto len = static_cast<uint8_t>(layout.blockDataLen());
  const uint16_t off = layout.dataOffset(k);
  if (!blockInRange(layout, k) || !fits(pdu.size(), off, len) || out.size() < len) {
    return 0;
  }
  out[0] = pdu[off];
  if (len > 1) {
    out[1] = pdu[off + 1];
  }
  return len;
}

void fsoeFrameWriteBlock(std::span<uint8_t> pdu, const FsoeFrameLayout& layout, uint16_t k,
                         std::span<const uint8_t> in, uint8_t len) {
  const uint16_t off = layout.dataOffset(k);
  if (!blockInRange(layout, k) || !fits(pdu.size(), off, len) || in.size() < len) {
    return;
  }
  pdu[off] = in[0];
  if (len > 1) {
    pdu[off + 1] = in[1];
  }
}

uint16_t fsoeFrameReadCrc(std::span<const uint8_t> pdu, const FsoeFrameLayout& layout, uint16_t k) {
  const uint16_t off = layout.crcOffset(k);
  if (!blockInRange(layout, k) || !fits(pdu.size(), off, 2)) {
    return 0;
  }
  return readLe16(pdu, off);
}

void fsoeFrameWriteCrc(std::span<uint8_t> pdu, const FsoeFrameLayout& layout, uint16_t k,
                       uint16_t crc) {
  const uint16_t off = layout.crcOffset(k);
  if (!blockInRange(layout, k) || !fits(pdu.size(), off, 2)) {
    return;
  }
  writeLe16(pdu, off, crc);
}

uint16_t fsoeFrameReadSafeData(std::span<const uint8_t> pdu, const FsoeFrameLayout& layout,
                               std::span<uint8_t> out) {
  const uint16_t width = layout.blockDataLen();
  uint16_t written = 0;
  for (uint16_t k = 0; k < layout.blockCount(); ++k) {
    std::array<uint8_t, 2> block{};
    const uint8_t len = fsoeFrameReadBlock(pdu, layout, k, block);
    for (uint16_t i = 0; i < len && written < out.size(); ++i) {
      out[written] = block[i];
      ++written;
    }
    if (len != width) {
      break;
    }
  }
  return written;
}

void fsoeFrameWriteSafeData(std::span<uint8_t> pdu, const FsoeFrameLayout& layout,
                            std::span<const uint8_t> data) {
  const uint16_t width = layout.blockDataLen();
  for (uint16_t k = 0; k < layout.blockCount(); ++k) {
    std::array<uint8_t, 2> block{};
    const size_t base = static_cast<size_t>(k) * width;
    for (uint16_t i = 0; i < width; ++i) {
      block[i] = (base + i) < data.size() ? data[base + i] : 0;
    }
    fsoeFrameWriteBlock(pdu, layout, k, block, static_cast<uint8_t>(width));
  }
}

uint16_t fsoePduBuildCrcs(std::span<uint8_t> pdu, const FsoeFrameLayout& layout, uint16_t recvCrc0,
                          uint16_t seqNo) {
  const uint8_t command = fsoeFrameCommand(pdu);
  const uint16_t connId = fsoeFrameConnId(pdu, layout);
  uint16_t crc0 = 0;

  for (uint16_t k = 0; k < layout.blockCount(); ++k) {
    std::array<uint8_t, 2> block{};
    const uint8_t len = fsoeFrameReadBlock(pdu, layout, k, block);
    const uint16_t crc = fsoeCrc(recvCrc0, connId, seqNo, command, k, block, len);
    fsoeFrameWriteCrc(pdu, layout, k, crc);
    if (k == 0) {
      crc0 = crc;
    }
  }
  return crc0;
}

FsoeCheckResult fsoePduCheck(std::span<const uint8_t> pdu, const FsoeFrameLayout& layout,
                             uint16_t recvCrc0, uint16_t seqNo) {
  const uint16_t blocks = layout.blockCount();
  const uint8_t command = fsoeFrameCommand(pdu);
  const uint16_t connId = fsoeFrameConnId(pdu, layout);
  FsoeCheckResult result{.crcOk = blocks != 0 && pdu.size() >= layout.size(), .crc0 = 0};

  for (uint16_t k = 0; k < blocks; ++k) {
    std::array<uint8_t, 2> block{};
    const uint8_t len = fsoeFrameReadBlock(pdu, layout, k, block);
    const uint16_t calc = fsoeCrc(recvCrc0, connId, seqNo, command, k, block, len);
    if (calc != fsoeFrameReadCrc(pdu, layout, k)) {
      result.crcOk = false;
    }
    if (k == 0) {
      result.crc0 = calc;
    }
  }
  return result;
}

}  // namespace mm::etg
