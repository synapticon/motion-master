#pragma once

#include <cstdint>
#include <vector>

/// @file
/// @brief Builds EtherCAT frames for the tests, byte by byte.

namespace mm::capture::test {

struct DatagramSpec {
  uint8_t command;
  uint8_t index;
  uint16_t adp;
  uint16_t ado;
  uint16_t length;
  uint16_t wkc = 0;
};

/// Builds one frame. @p fromMaster clear sets the locally administered bit of the source MAC, the
/// way every ESC does on a frame it processed.
inline std::vector<uint8_t> frame(const std::vector<DatagramSpec>& datagrams, bool fromMaster) {
  std::vector<uint8_t> out;
  for (int i = 0; i < 6; ++i) {
    out.push_back(0xFF);  // destination: broadcast, as SOEM sends
  }
  const uint8_t first = fromMaster ? 0x01 : 0x03;
  out.push_back(first);
  for (int i = 0; i < 5; ++i) {
    out.push_back(0x01);
  }
  out.push_back(0x88);
  out.push_back(0xA4);
  uint16_t length = 0;
  for (const auto& d : datagrams) {
    length = static_cast<uint16_t>(length + 10 + d.length + 2);
  }
  const uint16_t header = static_cast<uint16_t>(length | (1u << 12));
  out.push_back(static_cast<uint8_t>(header));
  out.push_back(static_cast<uint8_t>(header >> 8));
  for (std::size_t n = 0; n < datagrams.size(); ++n) {
    const auto& d = datagrams[n];
    out.push_back(d.command);
    out.push_back(d.index);
    out.push_back(static_cast<uint8_t>(d.adp));
    out.push_back(static_cast<uint8_t>(d.adp >> 8));
    out.push_back(static_cast<uint8_t>(d.ado));
    out.push_back(static_cast<uint8_t>(d.ado >> 8));
    const uint16_t lengthField =
        static_cast<uint16_t>(d.length | (n + 1 < datagrams.size() ? 0x8000u : 0u));
    out.push_back(static_cast<uint8_t>(lengthField));
    out.push_back(static_cast<uint8_t>(lengthField >> 8));
    out.push_back(0);
    out.push_back(0);
    for (uint16_t i = 0; i < d.length; ++i) {
      out.push_back(0);
    }
    out.push_back(static_cast<uint8_t>(d.wkc));
    out.push_back(static_cast<uint8_t>(d.wkc >> 8));
  }
  return out;
}

}  // namespace mm::capture::test
