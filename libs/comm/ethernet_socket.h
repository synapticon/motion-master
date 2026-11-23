#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "comm/ethernet_discovery.h"

namespace mm::comm {

/// @brief Raw access to one Ethernet adapter: send a whole frame, receive whole frames.
///
/// Linux uses an AF_PACKET socket, which needs CAP_NET_RAW. Windows and macOS use pcap: Npcap on
/// Windows, and the libpcap of macOS, which needs read and write access to /dev/bpf*. The socket
/// receives every frame on the adapter, also one that the operating system would drop, such as a
/// UDP answer from a device on another subnet. Move-only.
class EthernetSocket {
 public:
  /// @brief Opens @p adapter, an interface name as SOEM takes it: `eth0` on Linux, `en5` on macOS,
  /// `\Device\NPF_{GUID}` on Windows.
  static std::expected<EthernetSocket, std::string> open(const std::string& adapter);

  EthernetSocket(EthernetSocket&& other) noexcept;
  EthernetSocket& operator=(EthernetSocket&& other) noexcept;
  EthernetSocket(const EthernetSocket&) = delete;
  EthernetSocket& operator=(const EthernetSocket&) = delete;
  ~EthernetSocket();

  /// @brief Sends @p frame, a whole Ethernet frame from the destination address on.
  std::expected<void, std::string> send(std::span<const uint8_t> frame);

  /// @brief Waits up to @p timeout for one frame. Returns no frame when the time ran out.
  std::expected<std::optional<std::vector<uint8_t>>, std::string> receive(
      std::chrono::milliseconds timeout);

  /// @brief The IPv4 address and mask of the adapter, or nothing when it has no IPv4 address.
  const std::optional<Ipv4Interface>& ipv4() const { return ipv4_; }

 private:
  struct Handle;
  EthernetSocket(std::unique_ptr<Handle> handle, std::optional<Ipv4Interface> ipv4);

  std::unique_ptr<Handle> handle_;
  std::optional<Ipv4Interface> ipv4_;
};

}  // namespace mm::comm
