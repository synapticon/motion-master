#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "comm/ethernet_socket.h"
#include "core/platform.h"

namespace mm::comm {

namespace {

constexpr std::array<uint8_t, 6> kLldpMulticast = {0x01, 0x80, 0xC2, 0x00, 0x00, 0x0E};

std::optional<Ipv4Interface> readIpv4(const std::string& adapter) {
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return std::nullopt;
  }
  ifreq request{};
  std::strncpy(request.ifr_name, adapter.c_str(), IFNAMSIZ - 1);
  std::optional<Ipv4Interface> result;
  if (ioctl(fd, SIOCGIFADDR, &request) == 0) {
    sockaddr_in address{};
    std::memcpy(&address, &request.ifr_addr, sizeof(address));
    Ipv4Interface ipv4;
    std::memcpy(ipv4.address.data(), &address.sin_addr.s_addr, ipv4.address.size());
    if (ioctl(fd, SIOCGIFNETMASK, &request) == 0) {
      sockaddr_in mask{};
      std::memcpy(&mask, &request.ifr_netmask, sizeof(mask));
      std::memcpy(ipv4.netmask.data(), &mask.sin_addr.s_addr, ipv4.netmask.size());
    }
    result = ipv4;
  }
  close(fd);
  return result;
}

}  // namespace

struct EthernetSocket::Handle {
  int fd = -1;
  int ifindex = 0;

  ~Handle() {
    if (fd >= 0) {
      close(fd);
    }
  }
};

EthernetSocket::EthernetSocket(std::unique_ptr<Handle> handle, std::optional<Ipv4Interface> ipv4)
    : handle_(std::move(handle)), ipv4_(ipv4) {}

EthernetSocket::EthernetSocket(EthernetSocket&& other) noexcept = default;
EthernetSocket& EthernetSocket::operator=(EthernetSocket&& other) noexcept = default;
EthernetSocket::~EthernetSocket() = default;

std::expected<EthernetSocket, std::string> EthernetSocket::open(const std::string& adapter) {
  auto handle = std::make_unique<Handle>();
  handle->ifindex = static_cast<int>(if_nametoindex(adapter.c_str()));
  if (handle->ifindex == 0) {
    return std::unexpected("no adapter named " + adapter);
  }
  // A socket for every Ethernet type, because the answers come in three types: PROFINET, IPv4 and
  // LLDP.
  handle->fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
  if (handle->fd < 0) {
    return std::unexpected("cannot open a raw socket (CAP_NET_RAW is needed): " +
                           mm::core::errnoMessage(errno));
  }
  sockaddr_ll address{};
  address.sll_family = AF_PACKET;
  address.sll_protocol = htons(ETH_P_ALL);
  address.sll_ifindex = handle->ifindex;
  if (bind(handle->fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    return std::unexpected("cannot bind to " + adapter + ": " + mm::core::errnoMessage(errno));
  }
  // An adapter can filter multicast addresses that nobody joined, and LLDP goes to one. A failure
  // costs the LLDP answers only, so the discovery goes on without them.
  packet_mreq membership{};
  membership.mr_ifindex = handle->ifindex;
  membership.mr_type = PACKET_MR_MULTICAST;
  membership.mr_alen = kLldpMulticast.size();
  std::memcpy(membership.mr_address, kLldpMulticast.data(), kLldpMulticast.size());
  setsockopt(handle->fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &membership, sizeof(membership));
  return EthernetSocket(std::move(handle), readIpv4(adapter));
}

std::expected<void, std::string> EthernetSocket::send(std::span<const uint8_t> frame) {
  sockaddr_ll destination{};
  destination.sll_family = AF_PACKET;
  destination.sll_ifindex = handle_->ifindex;
  destination.sll_halen = 6;
  std::memcpy(destination.sll_addr, frame.data(), 6);
  const ssize_t sent = sendto(handle_->fd, frame.data(), frame.size(), 0,
                              reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
  if (sent != static_cast<ssize_t>(frame.size())) {
    return std::unexpected(mm::core::errnoMessage(errno));
  }
  return {};
}

std::expected<std::optional<std::vector<uint8_t>>, std::string> EthernetSocket::receive(
    std::chrono::milliseconds timeout) {
  pollfd pending{handle_->fd, POLLIN, 0};
  const int ready = poll(&pending, 1, static_cast<int>(timeout.count()));
  if (ready < 0) {
    if (errno == EINTR) {
      return std::optional<std::vector<uint8_t>>{};
    }
    return std::unexpected(mm::core::errnoMessage(errno));
  }
  if (ready == 0) {
    return std::optional<std::vector<uint8_t>>{};
  }
  std::vector<uint8_t> frame(2048);
  sockaddr_ll from{};
  socklen_t fromLength = sizeof(from);
  const ssize_t received = recvfrom(handle_->fd, frame.data(), frame.size(), 0,
                                    reinterpret_cast<sockaddr*>(&from), &fromLength);
  if (received < 0) {
    return std::unexpected(mm::core::errnoMessage(errno));
  }
  // A packet socket for every type also gets the frames this host sends.
  if (from.sll_pkttype == PACKET_OUTGOING) {
    return std::optional<std::vector<uint8_t>>{};
  }
  frame.resize(static_cast<size_t>(received));
  return std::optional<std::vector<uint8_t>>{std::move(frame)};
}

}  // namespace mm::comm
