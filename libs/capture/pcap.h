#pragma once

#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

/// @file
/// @brief The pcap file format, as much of it as a capture of Ethernet frames needs.
///
/// Written by hand rather than through libpcap, so the Linux build needs no library for it. The
/// format is a 24-byte file header and a 16-byte header per record. Wireshark and tcpdump read it.

namespace mm::capture {

/// @brief One captured frame.
struct Record {
  int64_t timestampNs = 0;     ///< Nanoseconds since the Unix epoch, from the kernel.
  std::vector<uint8_t> bytes;  ///< The frame from the Ethernet header on.
};

/// @brief Writes records to a pcap file as they arrive.
class PcapWriter {
 public:
  PcapWriter() = default;
  ~PcapWriter();
  PcapWriter(const PcapWriter&) = delete;
  PcapWriter& operator=(const PcapWriter&) = delete;

  /// @brief Creates @p path and writes the file header, with nanosecond timestamps.
  std::expected<void, std::string> open(const std::filesystem::path& path);

  /// @brief Appends one frame.
  std::expected<void, std::string> write(int64_t timestampNs, std::span<const uint8_t> bytes);

  /// @brief Flushes and closes the file. Called by the destructor when not called before.
  void close();

 private:
  std::FILE* file_ = nullptr;
};

/// @brief Reads every record of a pcap file held in memory.
///
/// Accepts microsecond and nanosecond timestamps and both byte orders, which is what tcpdump and
/// Wireshark write. Only Ethernet captures are accepted, because the frame parser starts at the
/// Ethernet header.
std::expected<std::vector<Record>, std::string> parsePcap(std::span<const uint8_t> bytes);

/// @brief Reads every record of a pcap file into memory. See @c parsePcap for what is accepted.
///
std::expected<std::vector<Record>, std::string> readPcap(const std::filesystem::path& path);

}  // namespace mm::capture
