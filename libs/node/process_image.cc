#include "node/process_image.h"

#include <algorithm>
#include <format>
#include <string>
#include <unordered_map>
#include <vector>

#include "node/device.h"

namespace mm::node {

namespace {

// Appends one direction's mapped objects to @p out, converting each entry's window-relative
// bit offset to an absolute offset within the direction's image. Padding entries (index 0)
// are skipped. Returns an error if the mapping overflows the window the driver reserved.
std::expected<void, std::string> appendEntries(std::vector<ProcessImageEntry>& out,
                                               uint16_t slavePosition,
                                               const std::vector<PdoMappingEntry>& entries,
                                               uint32_t mappedBits, uint32_t windowOffsetBytes,
                                               uint32_t windowBytes, const char* direction) {
  if (mappedBits > windowBytes * 8u) {
    return std::unexpected(
        std::format("device {}: {} mapping is {} bits but the driver reserved only {} bytes",
                    slavePosition, direction, mappedBits, windowBytes));
  }
  const uint32_t windowOffsetBits = windowOffsetBytes * 8u;
  for (const auto& e : entries) {
    if (e.index == 0) {
      continue;  // alignment gap — binds to no object
    }
    out.push_back(ProcessImageEntry{
        .slavePosition = slavePosition,
        .index = e.index,
        .subindex = e.subindex,
        .bitLength = e.bitLength,
        .bitOffset = windowOffsetBits + e.bitOffset,
    });
  }
  return {};
}

}  // namespace

std::expected<ProcessImage, std::string> buildProcessImage(const mm::comm::PdoLayout& layout,
                                                           const std::vector<Device>& devices) {
  std::unordered_map<uint16_t, const mm::comm::SlaveIo*> windows;
  windows.reserve(layout.slaves.size());
  for (const auto& s : layout.slaves) {
    windows.emplace(s.slavePosition, &s);
  }

  ProcessImage image;
  image.outputBytes = layout.outputBytes;
  image.inputBytes = layout.inputBytes;
  image.expectedWkc = layout.expectedWkc;

  for (const auto& device : devices) {
    const auto& mappings = device.pdoMappings();
    if (mappings.outputs.empty() && mappings.inputs.empty()) {
      continue;  // device contributes no process data
    }
    auto it = windows.find(device.slavePosition());
    if (it == windows.end()) {
      return std::unexpected(std::format(
          "device {} has a PDO mapping but no process-image window in the driver layout",
          device.slavePosition()));
    }
    const mm::comm::SlaveIo& io = *it->second;
    if (auto r = appendEntries(image.outputs, device.slavePosition(), mappings.outputs,
                               mappings.outputBits, io.outputOffset, io.outputBytes, "output");
        !r) {
      return std::unexpected(r.error());
    }
    if (auto r = appendEntries(image.inputs, device.slavePosition(), mappings.inputs,
                               mappings.inputBits, io.inputOffset, io.inputBytes, "input");
        !r) {
      return std::unexpected(r.error());
    }
  }
  return image;
}

std::optional<ProcessImage::Location> ProcessImage::find(uint16_t slavePosition, uint16_t index,
                                                         uint8_t subindex) const {
  const auto match = [&](const ProcessImageEntry& e) {
    return e.slavePosition == slavePosition && e.index == index && e.subindex == subindex;
  };
  if (auto it = std::ranges::find_if(outputs, match); it != outputs.end()) {
    return Location{.bitOffset = it->bitOffset, .bitLength = it->bitLength, .isOutput = true};
  }
  if (auto it = std::ranges::find_if(inputs, match); it != inputs.end()) {
    return Location{.bitOffset = it->bitOffset, .bitLength = it->bitLength, .isOutput = false};
  }
  return std::nullopt;
}

std::vector<uint8_t> extractBits(std::span<const uint8_t> src, uint32_t bitOffset,
                                 uint16_t bitLength) {
  std::vector<uint8_t> out((bitLength + 7u) / 8u, 0u);
  if ((bitOffset % 8u) == 0u && (bitLength % 8u) == 0u) {
    const uint32_t byteOffset = bitOffset / 8u;
    for (uint32_t i = 0; i < out.size() && byteOffset + i < src.size(); ++i) {
      out[i] = src[byteOffset + i];
    }
    return out;
  }
  for (uint16_t i = 0; i < bitLength; ++i) {
    const uint32_t srcBit = bitOffset + i;
    const uint32_t srcByte = srcBit / 8u;
    if (srcByte >= src.size()) {
      break;
    }
    const uint8_t bit = (src[srcByte] >> (srcBit % 8u)) & 1u;
    out[i / 8u] |= static_cast<uint8_t>(bit << (i % 8u));
  }
  return out;
}

void insertBits(std::span<uint8_t> dst, uint32_t bitOffset, uint16_t bitLength,
                std::span<const uint8_t> value) {
  if ((bitOffset % 8u) == 0u && (bitLength % 8u) == 0u) {
    const uint32_t byteOffset = bitOffset / 8u;
    const uint32_t n = bitLength / 8u;
    for (uint32_t i = 0; i < n && byteOffset + i < dst.size(); ++i) {
      dst[byteOffset + i] = i < value.size() ? value[i] : uint8_t{0};
    }
    return;
  }
  for (uint16_t i = 0; i < bitLength; ++i) {
    const uint32_t dstBit = bitOffset + i;
    const uint32_t dstByte = dstBit / 8u;
    if (dstByte >= dst.size()) {
      break;
    }
    const uint8_t bit =
        (i / 8u) < value.size() ? static_cast<uint8_t>((value[i / 8u] >> (i % 8u)) & 1u) : 0u;
    const uint8_t mask = static_cast<uint8_t>(1u << (dstBit % 8u));
    dst[dstByte] = static_cast<uint8_t>((dst[dstByte] & ~mask) | (bit ? mask : 0u));
  }
}

}  // namespace mm::node
