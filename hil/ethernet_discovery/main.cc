// Shows which discovery methods find a SOMANET drive that runs EtherNet/IP or PROFINET firmware.
//
// Issue #58 needs a way to find the drives on an Ethernet adapter and their IP addresses, without a
// connection to a drive. A drive serves one SPoE client at a time, so a connection could disturb
// the client that works with it. This program tries three methods on one adapter and prints every
// answer with the time it arrived:
//
//   dcp   PROFINET DCP Identify All. A multicast request to 01:0e:cf:00:00:00. A PROFINET device
//         answers with its IP address, its name of station and its vendor and device IDs.
//   eip   EtherNet/IP ListIdentity. A UDP broadcast to port 44818. An EtherNet/IP device answers
//         with its IP address, vendor ID, product code, serial number and product name.
//   lldp  LLDP. The program only listens. A device sends LLDP frames by itself, at an interval it
//         chooses, so the listen window must be longer than that interval to see every device.
//
// Every frame is sent and received on a raw socket. So the program also sees an EtherNet/IP answer
// that the kernel drops, for example from a drive on a different subnet than the adapter. The
// requests change nothing on a device. The program never opens a TCP connection.
//
// The summary groups the answers by IP address. It does not group them by MAC address, because a
// device can send each kind of frame from a different MAC address.

#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/platform.h"

namespace {

using Clock = std::chrono::steady_clock;
using MacAddress = std::array<uint8_t, 6>;

constexpr uint16_t kEtherTypeIpv4 = 0x0800;
constexpr uint16_t kEtherTypeVlan = 0x8100;
constexpr uint16_t kEtherTypeProfinet = 0x8892;
constexpr uint16_t kEtherTypeLldp = 0x88CC;

constexpr MacAddress kDcpMulticast = {0x01, 0x0E, 0xCF, 0x00, 0x00, 0x00};
constexpr MacAddress kLldpMulticast = {0x01, 0x80, 0xC2, 0x00, 0x00, 0x0E};
constexpr MacAddress kBroadcast = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

constexpr uint16_t kDcpIdentifyRequest = 0xFEFE;
constexpr uint16_t kDcpIdentifyResponse = 0xFEFF;
constexpr uint8_t kDcpServiceIdentify = 0x05;
constexpr uint32_t kDcpXid = 0x4D4D3538;  // "MM58"

constexpr uint16_t kEipPort = 44818;
constexpr uint16_t kEipListIdentity = 0x0063;
constexpr uint16_t kEipIdentityItem = 0x000C;
constexpr size_t kEipHeaderSize = 24;
constexpr uint16_t kEipMaxResponseDelayMs = 500;

constexpr size_t kMinFrameSize = 60;

struct Options {
  std::string ifname;
  int durationS = 35;
  std::optional<in_addr> sourceIp;
  bool dcp = true;
  bool eip = true;
  bool lldp = true;
};

struct Adapter {
  int index = 0;
  MacAddress mac{};
  std::optional<in_addr> ip;
  std::optional<in_addr> netmask;
};

// One answer to one method. Empty fields mean that the method does not carry the value.
struct Finding {
  std::string method;
  int64_t timeMs = 0;
  MacAddress sourceMac{};
  std::string ip;
  std::string netmask;
  std::string name;
  std::string details;
};

void printUsage() {
  std::cerr
      << "usage: ethernet_discovery --ifname <interface> [--duration <s>] [--source-ip <ipv4>]\n"
         "                      [--methods dcp,eip,lldp]\n";
}

bool parseMethods(std::string_view list, Options& options) {
  options.dcp = false;
  options.eip = false;
  options.lldp = false;
  while (!list.empty()) {
    const size_t comma = list.find(',');
    const std::string_view method = list.substr(0, comma);
    if (method == "dcp") {
      options.dcp = true;
    } else if (method == "eip") {
      options.eip = true;
    } else if (method == "lldp") {
      options.lldp = true;
    } else {
      return false;
    }
    if (comma == std::string_view::npos) {
      break;
    }
    list.remove_prefix(comma + 1);
  }
  return options.dcp || options.eip || options.lldp;
}

bool parseOptions(int argc, char** argv, Options& options) {
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string_view arg = argv[i];
    if (arg == "--ifname") {
      options.ifname = argv[i + 1];
    } else if (arg == "--duration") {
      const std::string_view text = argv[i + 1];
      const auto [end, error] =
          std::from_chars(text.data(), text.data() + text.size(), options.durationS);
      if (error != std::errc() || end != text.data() + text.size()) {
        return false;
      }
    } else if (arg == "--source-ip") {
      in_addr address{};
      if (inet_pton(AF_INET, argv[i + 1], &address) != 1) {
        return false;
      }
      options.sourceIp = address;
    } else if (arg == "--methods") {
      if (!parseMethods(argv[i + 1], options)) {
        return false;
      }
    } else {
      return false;
    }
  }
  return !options.ifname.empty() && options.durationS > 0 && argc % 2 == 1;
}

uint16_t readBe16(std::span<const uint8_t> bytes, size_t offset) {
  return static_cast<uint16_t>((static_cast<unsigned>(bytes[offset]) << 8U) |
                               static_cast<unsigned>(bytes[offset + 1]));
}

uint16_t readLe16(std::span<const uint8_t> bytes, size_t offset) {
  return static_cast<uint16_t>(static_cast<unsigned>(bytes[offset]) |
                               (static_cast<unsigned>(bytes[offset + 1]) << 8U));
}

uint32_t readLe32(std::span<const uint8_t> bytes, size_t offset) {
  return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8U) |
         (static_cast<uint32_t>(bytes[offset + 2]) << 16U) |
         (static_cast<uint32_t>(bytes[offset + 3]) << 24U);
}

void appendBe16(std::vector<uint8_t>& frame, uint16_t value) {
  frame.push_back(static_cast<uint8_t>(value >> 8U));
  frame.push_back(static_cast<uint8_t>(value & 0xFFU));
}

void appendLe16(std::vector<uint8_t>& frame, uint16_t value) {
  frame.push_back(static_cast<uint8_t>(value & 0xFFU));
  frame.push_back(static_cast<uint8_t>(value >> 8U));
}

std::string formatMac(std::span<const uint8_t> mac) {
  return std::format("{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}", mac[0], mac[1], mac[2], mac[3],
                     mac[4], mac[5]);
}

std::string formatIpv4(std::span<const uint8_t> bytes) {
  return std::format("{}.{}.{}.{}", bytes[0], bytes[1], bytes[2], bytes[3]);
}

std::string formatIpv4(in_addr address) {
  std::array<uint8_t, 4> bytes{};
  std::memcpy(bytes.data(), &address.s_addr, bytes.size());
  return formatIpv4(bytes);
}

// Keeps printable ASCII and replaces every other byte with '.'. A device name is not trusted.
std::string printable(std::span<const uint8_t> bytes) {
  std::string text;
  for (const uint8_t byte : bytes) {
    if (byte == 0) {
      break;
    }
    text.push_back(byte >= 0x20 && byte < 0x7F ? static_cast<char>(byte) : '.');
  }
  return text;
}

std::optional<Adapter> openAdapterInfo(const std::string& ifname) {
  Adapter adapter;
  adapter.index = static_cast<int>(if_nametoindex(ifname.c_str()));
  if (adapter.index == 0) {
    return std::nullopt;
  }
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return std::nullopt;
  }
  ifreq request{};
  std::strncpy(request.ifr_name, ifname.c_str(), IFNAMSIZ - 1);
  if (ioctl(fd, SIOCGIFHWADDR, &request) == 0) {
    std::memcpy(adapter.mac.data(), request.ifr_hwaddr.sa_data, adapter.mac.size());
  }
  if (ioctl(fd, SIOCGIFADDR, &request) == 0) {
    sockaddr_in address{};
    std::memcpy(&address, &request.ifr_addr, sizeof(address));
    adapter.ip = address.sin_addr;
  }
  if (ioctl(fd, SIOCGIFNETMASK, &request) == 0) {
    sockaddr_in mask{};
    std::memcpy(&mask, &request.ifr_netmask, sizeof(mask));
    adapter.netmask = mask.sin_addr;
  }
  close(fd);
  return adapter;
}

void appendEthernetHeader(std::vector<uint8_t>& frame, const MacAddress& destination,
                          const MacAddress& source, uint16_t etherType) {
  frame.insert(frame.end(), destination.begin(), destination.end());
  frame.insert(frame.end(), source.begin(), source.end());
  appendBe16(frame, etherType);
}

void padFrame(std::vector<uint8_t>& frame) {
  if (frame.size() < kMinFrameSize) {
    frame.resize(kMinFrameSize, 0);
  }
}

std::vector<uint8_t> makeDcpIdentifyAll(const MacAddress& source) {
  std::vector<uint8_t> frame;
  appendEthernetHeader(frame, kDcpMulticast, source, kEtherTypeProfinet);
  appendBe16(frame, kDcpIdentifyRequest);
  frame.push_back(kDcpServiceIdentify);
  frame.push_back(0x00);  // request
  appendBe16(frame, static_cast<uint16_t>(kDcpXid >> 16U));
  appendBe16(frame, static_cast<uint16_t>(kDcpXid & 0xFFFFU));
  // A response delay factor of 1 asks every device to answer at once, without a spread.
  appendBe16(frame, 0x0001);
  appendBe16(frame, 4);  // DCP data length: one block of 4 bytes.
  // Option 0xFF, suboption 0xFF: the "all selector" block, which every device matches.
  frame.push_back(0xFF);
  frame.push_back(0xFF);
  appendBe16(frame, 0x0000);
  padFrame(frame);
  return frame;
}

uint16_t ipv4Checksum(std::span<const uint8_t> header) {
  uint32_t sum = 0;
  for (size_t i = 0; i + 1 < header.size(); i += 2) {
    sum += readBe16(header, i);
  }
  while (sum > 0xFFFFU) {
    sum = (sum & 0xFFFFU) + (sum >> 16U);
  }
  return static_cast<uint16_t>(~sum);
}

// Builds the whole UDP datagram by hand, so the request goes out of this adapter only, whatever
// the routing table says about 255.255.255.255.
std::vector<uint8_t> makeEipListIdentity(const MacAddress& source, in_addr sourceIp) {
  constexpr size_t kIpHeaderSize = 20;
  constexpr size_t kUdpHeaderSize = 8;
  std::vector<uint8_t> frame;
  appendEthernetHeader(frame, kBroadcast, source, kEtherTypeIpv4);
  const size_t ipStart = frame.size();
  frame.push_back(0x45);  // IPv4, header of 5 words
  frame.push_back(0x00);
  appendBe16(frame, static_cast<uint16_t>(kIpHeaderSize + kUdpHeaderSize + kEipHeaderSize));
  appendBe16(frame, 0x0000);  // identification
  appendBe16(frame, 0x0000);  // flags and fragment offset
  frame.push_back(64);        // TTL
  frame.push_back(IPPROTO_UDP);
  appendBe16(frame, 0x0000);  // checksum, set below
  std::array<uint8_t, 4> sourceBytes{};
  std::memcpy(sourceBytes.data(), &sourceIp.s_addr, sourceBytes.size());
  frame.insert(frame.end(), sourceBytes.begin(), sourceBytes.end());
  frame.insert(frame.end(), {0xFF, 0xFF, 0xFF, 0xFF});
  const uint16_t checksum =
      ipv4Checksum(std::span<const uint8_t>(frame).subspan(ipStart, kIpHeaderSize));
  frame[ipStart + 10] = static_cast<uint8_t>(checksum >> 8U);
  frame[ipStart + 11] = static_cast<uint8_t>(checksum & 0xFFU);

  appendBe16(frame, kEipPort);  // source port
  appendBe16(frame, kEipPort);  // destination port
  appendBe16(frame, static_cast<uint16_t>(kUdpHeaderSize + kEipHeaderSize));
  appendBe16(frame, 0x0000);  // no UDP checksum, which IPv4 permits

  appendLe16(frame, kEipListIdentity);
  appendLe16(frame, 0);             // length of the command data
  frame.insert(frame.end(), 4, 0);  // session handle
  frame.insert(frame.end(), 4, 0);  // status
  // The sender context of a ListIdentity request carries the longest random delay of the answer in
  // ms, in its first 2 bytes. A drive with EtherNet/IP firmware answered a context that started
  // with the text "sp" after 13.2 s, which fits a delay of up to 28787 ms.
  appendLe16(frame, kEipMaxResponseDelayMs);
  frame.insert(frame.end(), 6, 0);
  frame.insert(frame.end(), 4, 0);  // options
  padFrame(frame);
  return frame;
}

std::optional<Finding> parseDcp(std::span<const uint8_t> payload) {
  constexpr size_t kHeaderSize = 12;
  if (payload.size() < kHeaderSize || readBe16(payload, 0) != kDcpIdentifyResponse ||
      payload[2] != kDcpServiceIdentify) {
    return std::nullopt;
  }
  const uint32_t xid = (static_cast<uint32_t>(readBe16(payload, 4)) << 16U) | readBe16(payload, 6);
  if (xid != kDcpXid) {
    return std::nullopt;
  }
  Finding finding;
  finding.method = "dcp";
  if (payload[3] != 0x01) {
    finding.details = std::format("service type 0x{:02X} (not success)", payload[3]);
  }
  const size_t end = std::min(payload.size(), kHeaderSize + readBe16(payload, 10));
  size_t offset = kHeaderSize;
  std::string typeOfStation;
  while (offset + 4 <= end) {
    const uint8_t option = payload[offset];
    const uint8_t suboption = payload[offset + 1];
    const size_t length = readBe16(payload, offset + 2);
    const size_t valueStart = offset + 4;
    if (valueStart + length > end) {
      break;
    }
    // In a response every block starts with 2 bytes of block info.
    const auto value = payload.subspan(valueStart, length);
    if (option == 0x01 && suboption == 0x02 && value.size() >= 14) {
      finding.ip = formatIpv4(value.subspan(2, 4));
      finding.netmask = formatIpv4(value.subspan(6, 4));
      finding.details += std::format(" gateway={} ipBlockInfo=0x{:04X}",
                                     formatIpv4(value.subspan(10, 4)), readBe16(value, 0));
    } else if (option == 0x02 && suboption == 0x01 && value.size() > 2) {
      typeOfStation = printable(value.subspan(2));
    } else if (option == 0x02 && suboption == 0x02 && value.size() > 2) {
      finding.name = printable(value.subspan(2));
    } else if (option == 0x02 && suboption == 0x03 && value.size() >= 6) {
      finding.details += std::format(" vendorId=0x{:04X} deviceId=0x{:04X}", readBe16(value, 2),
                                     readBe16(value, 4));
    }
    offset = valueStart + length + (length % 2);
  }
  if (!typeOfStation.empty()) {
    finding.details += std::format(" typeOfStation=\"{}\"", typeOfStation);
  }
  return finding;
}

std::optional<Finding> parseEip(std::span<const uint8_t> payload) {
  constexpr size_t kIpHeaderMin = 20;
  constexpr size_t kUdpHeaderSize = 8;
  if (payload.size() < kIpHeaderMin || (payload[0] >> 4U) != 4 || payload[9] != IPPROTO_UDP) {
    return std::nullopt;
  }
  const size_t ipHeaderSize = static_cast<size_t>(payload[0] & 0x0FU) * 4;
  if (payload.size() < ipHeaderSize + kUdpHeaderSize + kEipHeaderSize) {
    return std::nullopt;
  }
  const auto udp = payload.subspan(ipHeaderSize);
  if (readBe16(udp, 0) != kEipPort) {
    return std::nullopt;
  }
  const auto eip = udp.subspan(kUdpHeaderSize);
  const uint16_t dataLength = readLe16(eip, 2);
  // A length of 0 is a request, for example the request of another host.
  if (readLe16(eip, 0) != kEipListIdentity || dataLength == 0) {
    return std::nullopt;
  }
  Finding finding;
  finding.method = "eip";
  finding.ip = formatIpv4(payload.subspan(12, 4));
  const auto data = eip.subspan(kEipHeaderSize);
  // Item count (2), item type (2), item length (2), protocol version (2), socket address (16),
  // vendor (2), device type (2), product code (2), revision (2), status (2), serial (4), name.
  constexpr size_t kNameOffset = 38;
  if (data.size() < kNameOffset + 1 || readLe16(data, 0) == 0 ||
      readLe16(data, 2) != kEipIdentityItem) {
    finding.details = std::format("unexpected identity reply of {} bytes", data.size());
    return finding;
  }
  const std::string socketIp = formatIpv4(data.subspan(12, 4));
  if (socketIp != finding.ip) {
    finding.details += std::format(" socketAddress={}", socketIp);
  }
  finding.details += std::format(
      " vendorId={} deviceType={} productCode={} revision={}.{} status=0x{:04X} serial=0x{:08X}",
      readLe16(data, 24), readLe16(data, 26), readLe16(data, 28), data[30], data[31],
      readLe16(data, 32), readLe32(data, 34));
  const size_t nameLength = data[kNameOffset];
  if (kNameOffset + 1 + nameLength <= data.size()) {
    finding.name = printable(data.subspan(kNameOffset + 1, nameLength));
  }
  return finding;
}

std::optional<Finding> parseLldp(std::span<const uint8_t> payload) {
  Finding finding;
  finding.method = "lldp";
  std::set<std::string> organisations;
  size_t offset = 0;
  while (offset + 2 <= payload.size()) {
    const uint16_t header = readBe16(payload, offset);
    const uint8_t type = static_cast<uint8_t>(header >> 9U);
    const size_t length = header & 0x01FFU;
    if (type == 0 || offset + 2 + length > payload.size()) {
      break;
    }
    const auto value = payload.subspan(offset + 2, length);
    if (type == 1 && value.size() == 7 && value[0] == 4) {
      finding.details += std::format(" chassis={}", formatMac(value.subspan(1)));
    } else if (type == 2 && value.size() > 1) {
      finding.details += std::format(" port=\"{}\"", printable(value.subspan(1)));
    } else if (type == 5) {
      finding.name = printable(value);
    } else if (type == 8 && value.size() >= 6 && value[0] == 5 && value[1] == 1) {
      finding.ip = formatIpv4(value.subspan(2, 4));
    } else if (type == 127 && value.size() >= 4) {
      organisations.insert(
          std::format("{:02x}{:02x}{:02x}/{}", value[0], value[1], value[2], value[3]));
    }
    offset += 2 + length;
  }
  if (!organisations.empty()) {
    finding.details += " orgTlvs=";
    for (const auto& organisation : organisations) {
      finding.details += organisation + ",";
    }
    finding.details.pop_back();
  }
  return finding;
}

std::string subnetNote(const std::string& ip, const Adapter& adapter) {
  in_addr address{};
  if (ip.empty() || !adapter.ip || !adapter.netmask ||
      inet_pton(AF_INET, ip.c_str(), &address) != 1) {
    return "";
  }
  if (address.s_addr == 0) {
    return " (no address)";
  }
  const bool same =
      (address.s_addr & adapter.netmask->s_addr) == (adapter.ip->s_addr & adapter.netmask->s_addr);
  return same ? "" : " (other subnet)";
}

void printFinding(const Finding& finding, const Adapter& adapter) {
  std::cout << std::format("{:>6} ms  {:<4}  {}  ip={}{}", finding.timeMs, finding.method,
                           formatMac(finding.sourceMac), finding.ip.empty() ? "-" : finding.ip,
                           subnetNote(finding.ip, adapter));
  if (!finding.netmask.empty()) {
    std::cout << " mask=" << finding.netmask;
  }
  if (!finding.name.empty()) {
    std::cout << " name=\"" << finding.name << '"';
  }
  std::cout << finding.details << '\n';
}

void printSummary(const std::vector<Finding>& findings, const Adapter& adapter) {
  // A set of source MAC addresses per method, because a device repeats its LLDP frames.
  std::map<std::string, std::map<std::string, std::set<std::string>>> byIp;
  for (const auto& finding : findings) {
    const std::string key = finding.ip.empty() ? "(no IP address)" : finding.ip;
    byIp[key][finding.method].insert(formatMac(finding.sourceMac));
  }
  std::cout << "\nSummary by IP address:\n";
  if (byIp.empty()) {
    std::cout << "  no answers\n";
    return;
  }
  for (const auto& [ip, methods] : byIp) {
    std::cout << "  " << ip << subnetNote(ip, adapter) << '\n';
    for (const std::string method : {"dcp", "eip", "lldp"}) {
      const auto found = methods.find(method);
      std::cout << std::format("    {:<4}  ", method);
      if (found == methods.end()) {
        std::cout << "no answer\n";
        continue;
      }
      std::string macs;
      for (const auto& mac : found->second) {
        macs += mac + " ";
      }
      std::cout << "from " << macs << '\n';
    }
  }
}

bool sendFrame(int fd, const Adapter& adapter, const std::vector<uint8_t>& frame) {
  sockaddr_ll destination{};
  destination.sll_family = AF_PACKET;
  destination.sll_ifindex = adapter.index;
  destination.sll_halen = 6;
  std::memcpy(destination.sll_addr, frame.data(), 6);
  const ssize_t sent = sendto(fd, frame.data(), frame.size(), 0,
                              reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
  return sent == static_cast<ssize_t>(frame.size());
}

}  // namespace

// std::format checks every format string at compile time, so its format_error cannot occur here.
// An allocation can still raise bad_alloc, and the end of the program is the right outcome then.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char** argv) {
  Options options;
  if (!parseOptions(argc, argv, options)) {
    printUsage();
    return EXIT_FAILURE;
  }
  const auto adapter = openAdapterInfo(options.ifname);
  if (!adapter) {
    std::cerr << "no adapter named " << options.ifname << '\n';
    return EXIT_FAILURE;
  }

  const int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
  if (fd < 0) {
    std::cerr << "cannot open a raw socket: " << mm::core::errnoMessage(errno)
              << ". Run with sudo or grant cap_net_raw.\n";
    return EXIT_FAILURE;
  }
  sockaddr_ll bindAddress{};
  bindAddress.sll_family = AF_PACKET;
  bindAddress.sll_protocol = htons(ETH_P_ALL);
  bindAddress.sll_ifindex = adapter->index;
  if (bind(fd, reinterpret_cast<const sockaddr*>(&bindAddress), sizeof(bindAddress)) != 0) {
    std::cerr << "cannot bind to " << options.ifname << ": " << mm::core::errnoMessage(errno)
              << '\n';
    close(fd);
    return EXIT_FAILURE;
  }
  if (options.lldp) {
    packet_mreq membership{};
    membership.mr_ifindex = adapter->index;
    membership.mr_type = PACKET_MR_MULTICAST;
    membership.mr_alen = 6;
    std::memcpy(membership.mr_address, kLldpMulticast.data(), kLldpMulticast.size());
    if (setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &membership, sizeof(membership)) != 0) {
      std::cerr << "cannot join the LLDP multicast group: " << mm::core::errnoMessage(errno)
                << '\n';
    }
  }

  std::cout << std::format("adapter {}  mac={}  ip={}  mask={}\n", options.ifname,
                           formatMac(adapter->mac), adapter->ip ? formatIpv4(*adapter->ip) : "-",
                           adapter->netmask ? formatIpv4(*adapter->netmask) : "-");

  const auto start = Clock::now();
  if (options.dcp) {
    if (sendFrame(fd, *adapter, makeDcpIdentifyAll(adapter->mac))) {
      std::cout << "sent DCP Identify All\n";
    } else {
      std::cerr << "cannot send DCP Identify All: " << mm::core::errnoMessage(errno) << '\n';
    }
  }
  if (options.eip) {
    const auto sourceIp = options.sourceIp ? options.sourceIp : adapter->ip;
    if (!sourceIp) {
      std::cerr << "skipped ListIdentity: the adapter has no IPv4 address. Use --source-ip.\n";
    } else if (sendFrame(fd, *adapter, makeEipListIdentity(adapter->mac, *sourceIp))) {
      std::cout << "sent ListIdentity from " << formatIpv4(*sourceIp) << '\n';
    } else {
      std::cerr << "cannot send ListIdentity: " << mm::core::errnoMessage(errno) << '\n';
    }
  }
  std::cout << std::format("listening for {} s\n\n", options.durationS);

  std::vector<Finding> findings;
  const auto deadline = start + std::chrono::seconds(options.durationS);
  std::array<uint8_t, 2048> buffer{};
  while (true) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (remaining <= 0) {
      break;
    }
    pollfd pending{fd, POLLIN, 0};
    if (poll(&pending, 1, static_cast<int>(remaining)) <= 0) {
      continue;
    }
    sockaddr_ll from{};
    socklen_t fromLength = sizeof(from);
    const ssize_t received = recvfrom(fd, buffer.data(), buffer.size(), 0,
                                      reinterpret_cast<sockaddr*>(&from), &fromLength);
    if (received < 14 || from.sll_pkttype == PACKET_OUTGOING) {
      continue;
    }
    auto frame = std::span<const uint8_t>(buffer.data(), static_cast<size_t>(received));
    size_t typeOffset = 12;
    if (readBe16(frame, typeOffset) == kEtherTypeVlan && frame.size() >= 18) {
      typeOffset += 4;
    }
    const uint16_t etherType = readBe16(frame, typeOffset);
    const auto payload = frame.subspan(typeOffset + 2);
    std::optional<Finding> finding;
    if (options.dcp && etherType == kEtherTypeProfinet) {
      finding = parseDcp(payload);
    } else if (options.eip && etherType == kEtherTypeIpv4) {
      finding = parseEip(payload);
    } else if (options.lldp && etherType == kEtherTypeLldp) {
      finding = parseLldp(payload);
    }
    if (!finding) {
      continue;
    }
    std::copy_n(frame.begin() + 6, 6, finding->sourceMac.begin());
    finding->timeMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    printFinding(*finding, *adapter);
    findings.push_back(*finding);
  }
  close(fd);
  printSummary(findings, *adapter);
  return EXIT_SUCCESS;
}
