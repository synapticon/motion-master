// clang-format off
#include <pcap.h>
// clang-format on

#ifndef _WIN32
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "comm/ethernet_socket.h"

namespace mm::comm {

namespace {

// pcap hands over a frame at the latest after this time, so receive() returns about this often
// while nothing arrives.
constexpr int kReadTimeoutMs = 50;

// The three kinds of answer. VLAN last, because the keyword moves the offsets of everything after
// it.
constexpr const char* kFilter =
    "ether proto 0x8892 or ether proto 0x88cc or udp src port 44818 or vlan";

std::optional<Ipv4Interface> readIpv4(const std::string& adapter) {
  std::array<char, PCAP_ERRBUF_SIZE> error{};
  pcap_if_t* devices = nullptr;
  if (pcap_findalldevs(&devices, error.data()) != 0) {
    return std::nullopt;
  }
  std::optional<Ipv4Interface> result;
  for (const pcap_if_t* device = devices; device != nullptr && !result; device = device->next) {
    if (adapter != device->name) {
      continue;
    }
    for (const pcap_addr_t* entry = device->addresses; entry != nullptr; entry = entry->next) {
      if (entry->addr == nullptr || entry->addr->sa_family != AF_INET) {
        continue;
      }
      Ipv4Interface ipv4;
      sockaddr_in address{};
      std::memcpy(&address, entry->addr, sizeof(address));
      std::memcpy(ipv4.address.data(), &address.sin_addr, ipv4.address.size());
      // macOS can hand over a mask without its family and shorter than a sockaddr_in, with the
      // missing bytes meaning zero. So the family is not checked, and only sa_len bytes are read.
      if (entry->netmask != nullptr) {
        sockaddr_in mask{};
#ifdef __APPLE__
        const size_t length = std::min<size_t>(entry->netmask->sa_len, sizeof(mask));
#else
        const size_t length = sizeof(mask);
#endif
        std::memcpy(&mask, entry->netmask, length);
        std::memcpy(ipv4.netmask.data(), &mask.sin_addr, ipv4.netmask.size());
      }
      result = ipv4;
      break;
    }
  }
  pcap_freealldevs(devices);
  return result;
}

}  // namespace

struct EthernetSocket::Handle {
  pcap_t* pcap = nullptr;

  ~Handle() {
    if (pcap != nullptr) {
      pcap_close(pcap);
    }
  }
};

EthernetSocket::EthernetSocket(std::unique_ptr<Handle> handle, std::optional<Ipv4Interface> ipv4)
    : handle_(std::move(handle)), ipv4_(ipv4) {}

EthernetSocket::EthernetSocket(EthernetSocket&& other) noexcept = default;
EthernetSocket& EthernetSocket::operator=(EthernetSocket&& other) noexcept = default;
EthernetSocket::~EthernetSocket() = default;

std::expected<EthernetSocket, std::string> EthernetSocket::open(const std::string& adapter) {
  std::array<char, PCAP_ERRBUF_SIZE> error{};
  auto handle = std::make_unique<Handle>();
  handle->pcap = pcap_create(adapter.c_str(), error.data());
  if (handle->pcap == nullptr) {
    return std::unexpected("cannot open " + adapter + ": " + error.data());
  }
  pcap_set_snaplen(handle->pcap, 2048);
  // Promiscuous, because an adapter can filter multicast addresses that nobody joined, and LLDP
  // goes to one. The filter below keeps every other frame out.
  pcap_set_promisc(handle->pcap, 1);
  pcap_set_timeout(handle->pcap, kReadTimeoutMs);
  pcap_set_immediate_mode(handle->pcap, 1);
  if (const int rc = pcap_activate(handle->pcap); rc < 0) {
    std::string why = pcap_geterr(handle->pcap);
#ifdef __APPLE__
    if (rc == PCAP_ERROR_PERM_DENIED) {
      why += " (read and write access to /dev/bpf* is needed)";
    }
#endif
    return std::unexpected("cannot open " + adapter + ": " + why);
  }
  bpf_program program{};
  if (pcap_compile(handle->pcap, &program, kFilter, 1, PCAP_NETMASK_UNKNOWN) == 0) {
    pcap_setfilter(handle->pcap, &program);
    pcap_freecode(&program);
  }
  return EthernetSocket(std::move(handle), readIpv4(adapter));
}

std::expected<void, std::string> EthernetSocket::send(std::span<const uint8_t> frame) {
  if (pcap_sendpacket(handle_->pcap, frame.data(), static_cast<int>(frame.size())) != 0) {
    return std::unexpected(std::string(pcap_geterr(handle_->pcap)));
  }
  return {};
}

std::expected<std::optional<std::vector<uint8_t>>, std::string> EthernetSocket::receive(
    std::chrono::milliseconds /*timeout*/) {
  // pcap waits at most its read timeout and cannot take another one per call. The caller checks its
  // own deadline after each call, so it waits at most one read timeout longer.
  pcap_pkthdr* header = nullptr;
  const u_char* data = nullptr;
  const int rc = pcap_next_ex(handle_->pcap, &header, &data);
  if (rc < 0) {
    return std::unexpected(std::string(pcap_geterr(handle_->pcap)));
  }
  if (rc == 0) {
    return std::optional<std::vector<uint8_t>>{};
  }
  return std::optional<std::vector<uint8_t>>{std::vector<uint8_t>(data, data + header->caplen)};
}

}  // namespace mm::comm
