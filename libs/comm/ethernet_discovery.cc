#include "comm/ethernet_discovery.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#include "comm/base.h"
#include "comm/ethernet_socket.h"

namespace mm::comm {

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint16_t kEtherTypeIpv4 = 0x0800;
constexpr uint16_t kEtherTypeVlan = 0x8100;
constexpr uint16_t kEtherTypeProfinet = 0x8892;
constexpr uint16_t kEtherTypeLldp = 0x88CC;

constexpr MacAddress kDcpMulticast = {0x01, 0x0E, 0xCF, 0x00, 0x00, 0x00};
constexpr MacAddress kBroadcast = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

constexpr uint16_t kDcpIdentifyRequest = 0xFEFE;
constexpr uint16_t kDcpIdentifyResponse = 0xFEFF;
constexpr uint8_t kDcpServiceIdentify = 0x05;
constexpr uint8_t kDcpServiceTypeSuccess = 0x01;
// The transaction ID of every DCP request of Motion Master. A response carries it back, so an
// answer to the request of another master is not taken.
constexpr uint32_t kDcpXid = 0x4D4D3538;

constexpr uint8_t kIpProtocolUdp = 17;
constexpr uint16_t kEipPort = 44818;
constexpr uint16_t kEipListIdentity = 0x0063;
constexpr uint16_t kEipIdentityItem = 0x000C;
constexpr size_t kEipHeaderSize = 24;
constexpr size_t kIpHeaderSize = 20;
constexpr size_t kUdpHeaderSize = 8;

constexpr size_t kEthernetHeaderSize = 14;
constexpr size_t kMinFrameSize = 60;

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

Ipv4Address toIpv4(std::span<const uint8_t> bytes) {
  Ipv4Address address{};
  std::copy_n(bytes.begin(), address.size(), address.begin());
  return address;
}

MacAddress toMac(std::span<const uint8_t> bytes) {
  MacAddress mac{};
  std::copy_n(bytes.begin(), mac.size(), mac.begin());
  return mac;
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

bool isZero(const Ipv4Address& address) {
  return std::ranges::all_of(address, [](uint8_t byte) { return byte == 0; });
}

std::optional<DiscoveryAnswer> parseDcp(std::span<const uint8_t> payload) {
  constexpr size_t kHeaderSize = 12;
  if (payload.size() < kHeaderSize || readBe16(payload, 0) != kDcpIdentifyResponse ||
      payload[2] != kDcpServiceIdentify || payload[3] != kDcpServiceTypeSuccess) {
    return std::nullopt;
  }
  const uint32_t xid = (static_cast<uint32_t>(readBe16(payload, 4)) << 16U) | readBe16(payload, 6);
  if (xid != kDcpXid) {
    return std::nullopt;
  }
  DiscoveryAnswer answer;
  answer.method = DiscoveryMethod::kDcp;
  answer.firmware = EthernetFirmware::kProfinet;
  const size_t end = std::min(payload.size(), kHeaderSize + readBe16(payload, 10));
  size_t offset = kHeaderSize;
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
      answer.ipAddress = toIpv4(value.subspan(2, 4));
    } else if (option == 0x02 && suboption == 0x02 && value.size() > 2) {
      answer.name = printable(value.subspan(2));
    } else if (option == 0x02 && suboption == 0x03 && value.size() >= 6) {
      answer.vendorId = readBe16(value, 2);
      answer.deviceId = readBe16(value, 4);
    }
    offset = valueStart + length + (length % 2);
  }
  return answer;
}

std::optional<DiscoveryAnswer> parseListIdentity(std::span<const uint8_t> payload) {
  if (payload.size() < kIpHeaderSize || (payload[0] >> 4U) != 4 || payload[9] != kIpProtocolUdp) {
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
  // A length of 0 is a request, for example the request of another host or this one.
  if (readLe16(eip, 0) != kEipListIdentity || readLe16(eip, 2) == 0) {
    return std::nullopt;
  }
  DiscoveryAnswer answer;
  answer.method = DiscoveryMethod::kListIdentity;
  answer.firmware = EthernetFirmware::kEthernetIp;
  answer.ipAddress = toIpv4(payload.subspan(12, 4));
  const auto data = eip.subspan(kEipHeaderSize);
  // Item count (2), item type (2), item length (2), protocol version (2), socket address (16),
  // vendor (2), device type (2), product code (2), revision (2), status (2), serial (4), name.
  constexpr size_t kNameOffset = 38;
  if (data.size() < kNameOffset + 1 || readLe16(data, 0) == 0 ||
      readLe16(data, 2) != kEipIdentityItem) {
    return answer;
  }
  answer.vendorId = readLe16(data, 24);
  answer.productCode = readLe16(data, 28);
  answer.serialNumber = readLe32(data, 34);
  const size_t nameLength = data[kNameOffset];
  if (kNameOffset + 1 + nameLength <= data.size()) {
    answer.name = printable(data.subspan(kNameOffset + 1, nameLength));
  }
  return answer;
}

std::optional<DiscoveryAnswer> parseLldp(std::span<const uint8_t> payload) {
  DiscoveryAnswer answer;
  answer.method = DiscoveryMethod::kLldp;
  size_t offset = 0;
  while (offset + 2 <= payload.size()) {
    const uint16_t header = readBe16(payload, offset);
    const auto type = static_cast<uint8_t>(header >> 9U);
    const size_t length = header & 0x01FFU;
    if (type == 0 || offset + 2 + length > payload.size()) {
      break;
    }
    const auto value = payload.subspan(offset + 2, length);
    if (type == 1 && value.size() == 7 && value[0] == 4) {
      // Chassis ID, subtype 4: a MAC address.
      answer.macAddress = toMac(value.subspan(1));
    } else if (type == 5) {
      answer.name = printable(value);
    } else if (type == 8 && value.size() >= 6 && value[0] == 5 && value[1] == 1) {
      // Management address: 5 bytes of address string, subtype 1 is IPv4.
      answer.ipAddress = toIpv4(value.subspan(2, 4));
    }
    offset += 2 + length;
  }
  return answer;
}

template <typename T>
void fill(std::optional<T>& target, const std::optional<T>& source) {
  if (!target && source) {
    target = source;
  }
}

std::string formatMac(const MacAddress& mac) {
  return std::format("{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}", mac[0], mac[1], mac[2], mac[3],
                     mac[4], mac[5]);
}

std::string formatIpv4(const Ipv4Address& address) {
  return std::format("{}.{}.{}.{}", address[0], address[1], address[2], address[3]);
}

template <typename T>
nlohmann::json orNull(const std::optional<T>& value) {
  return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
}

}  // namespace

std::string_view toString(DiscoveryMethod method) {
  switch (method) {
    case DiscoveryMethod::kDcp:
      return "dcp";
    case DiscoveryMethod::kListIdentity:
      return "list-identity";
    case DiscoveryMethod::kLldp:
      return "lldp";
  }
  return "";
}

std::string_view toString(EthernetFirmware firmware) {
  switch (firmware) {
    case EthernetFirmware::kEthernetIp:
      return "ethernet-ip";
    case EthernetFirmware::kProfinet:
      return "profinet";
    case EthernetFirmware::kUnknown:
      return "";
  }
  return "";
}

void to_json(nlohmann::json& j, const DiscoveredEthernetDevice& device) {
  std::vector<std::string_view> foundBy;
  std::ranges::transform(device.foundBy, std::back_inserter(foundBy),
                         [](DiscoveryMethod method) { return toString(method); });
  j = nlohmann::json{
      {"ipAddress",
       device.ipAddress ? nlohmann::json(formatIpv4(*device.ipAddress)) : nlohmann::json(nullptr)},
      {"macAddress",
       device.macAddress ? nlohmann::json(formatMac(*device.macAddress)) : nlohmann::json(nullptr)},
      {"name", device.name},
      {"firmware", device.firmware == EthernetFirmware::kUnknown
                       ? nlohmann::json(nullptr)
                       : nlohmann::json(toString(device.firmware))},
      {"vendorId", orNull(device.vendorId)},
      {"deviceId", orNull(device.deviceId)},
      {"productCode", orNull(device.productCode)},
      {"serialNumber", orNull(device.serialNumber)},
      {"sameSubnet", orNull(device.sameSubnet)},
      {"foundBy", std::move(foundBy)},
  };
}

void to_json(nlohmann::json& j, const EthernetDiscoveryResult& result) {
  j = nlohmann::json{
      {"networkAdapter", result.networkAdapter},
      {"durationMs", result.durationMs},
      {"answerDelayMs", result.answerDelayMs},
      {"devices", result.devices},
      {"warnings", result.warnings},
  };
}

std::optional<std::string> checkDiscoveryDuration(std::chrono::milliseconds duration) {
  if (duration < kMinDiscoveryDuration || duration > kMaxDiscoveryDuration) {
    return std::format("the discovery time must be at least {} ms and at most {} ms",
                       kMinDiscoveryDuration.count(), kMaxDiscoveryDuration.count());
  }
  return std::nullopt;
}

std::chrono::milliseconds listIdentityAnswerDelay(std::chrono::milliseconds duration) {
  return duration - kDiscoveryAnswerMargin;
}

std::vector<uint8_t> makeDcpIdentifyAll(const MacAddress& source) {
  std::vector<uint8_t> frame;
  appendEthernetHeader(frame, kDcpMulticast, source, kEtherTypeProfinet);
  appendBe16(frame, kDcpIdentifyRequest);
  frame.push_back(kDcpServiceIdentify);
  frame.push_back(0x00);  // request
  appendBe16(frame, static_cast<uint16_t>(kDcpXid >> 16U));
  appendBe16(frame, static_cast<uint16_t>(kDcpXid & 0xFFFFU));
  // A response delay factor of 1 asks every device to answer at once. The unit of a larger factor
  // is not checked against IEC 61158-6-10, so the answers are not spread.
  appendBe16(frame, 0x0001);
  appendBe16(frame, 4);  // DCP data length: one block of 4 bytes.
  // Option 0xFF, suboption 0xFF: the "all selector" block, which every device matches.
  frame.push_back(0xFF);
  frame.push_back(0xFF);
  appendBe16(frame, 0x0000);
  padFrame(frame);
  return frame;
}

std::vector<uint8_t> makeListIdentity(const MacAddress& source, const Ipv4Address& sourceIp,
                                      std::chrono::milliseconds answerDelay) {
  std::vector<uint8_t> frame;
  appendEthernetHeader(frame, kBroadcast, source, kEtherTypeIpv4);
  const size_t ipStart = frame.size();
  frame.push_back(0x45);  // IPv4, header of 5 words
  frame.push_back(0x00);
  appendBe16(frame, static_cast<uint16_t>(kIpHeaderSize + kUdpHeaderSize + kEipHeaderSize));
  appendBe16(frame, 0x0000);  // identification
  appendBe16(frame, 0x0000);  // flags and fragment offset
  frame.push_back(64);        // TTL
  frame.push_back(kIpProtocolUdp);
  appendBe16(frame, 0x0000);  // checksum, set below
  frame.insert(frame.end(), sourceIp.begin(), sourceIp.end());
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
  // The first 2 bytes of the sender context are the longest random delay of the answer in ms. A
  // drive with EtherNet/IP firmware answered a context that started with the text "sp" after
  // 13.2 s.
  appendLe16(frame, static_cast<uint16_t>(answerDelay.count()));
  frame.insert(frame.end(), 6, 0);
  frame.insert(frame.end(), 4, 0);  // options
  padFrame(frame);
  return frame;
}

std::optional<DiscoveryAnswer> parseDiscoveryFrame(std::span<const uint8_t> frame) {
  if (frame.size() < kEthernetHeaderSize) {
    return std::nullopt;
  }
  size_t typeOffset = 12;
  if (readBe16(frame, typeOffset) == kEtherTypeVlan && frame.size() >= kEthernetHeaderSize + 4) {
    typeOffset += 4;
  }
  const uint16_t etherType = readBe16(frame, typeOffset);
  const auto payload = frame.subspan(typeOffset + 2);
  std::optional<DiscoveryAnswer> answer;
  if (etherType == kEtherTypeProfinet) {
    answer = parseDcp(payload);
  } else if (etherType == kEtherTypeIpv4) {
    answer = parseListIdentity(payload);
  } else if (etherType == kEtherTypeLldp) {
    answer = parseLldp(payload);
  }
  // The source address of a DCP or ListIdentity answer is the device. LLDP has its chassis ID.
  if (answer && answer->method != DiscoveryMethod::kLldp) {
    answer->macAddress = toMac(frame.subspan(6, 6));
  }
  return answer;
}

std::vector<DiscoveredEthernetDevice> mergeDiscoveryAnswers(
    std::span<const DiscoveryAnswer> answers, const std::optional<Ipv4Interface>& networkAdapter) {
  std::vector<DiscoveredEthernetDevice> devices;
  for (const auto& answer : answers) {
    std::optional<Ipv4Address> ip;
    if (answer.ipAddress && !isZero(*answer.ipAddress)) {
      ip = answer.ipAddress;
    }
    auto found = std::ranges::find_if(devices, [&](const DiscoveredEthernetDevice& device) {
      return (ip && device.ipAddress == ip) ||
             (answer.macAddress && device.macAddress == answer.macAddress);
    });
    if (found == devices.end()) {
      devices.emplace_back();
      found = std::prev(devices.end());
    }
    DiscoveredEthernetDevice& device = *found;
    fill(device.ipAddress, ip);
    fill(device.macAddress, answer.macAddress);
    fill(device.vendorId, answer.vendorId);
    fill(device.deviceId, answer.deviceId);
    fill(device.productCode, answer.productCode);
    fill(device.serialNumber, answer.serialNumber);
    if (device.name.empty()) {
      device.name = answer.name;
    }
    if (device.firmware == EthernetFirmware::kUnknown) {
      device.firmware = answer.firmware;
    }
    if (std::ranges::find(device.foundBy, answer.method) == device.foundBy.end()) {
      device.foundBy.push_back(answer.method);
    }
  }
  for (auto& device : devices) {
    if (!device.ipAddress || !networkAdapter || isZero(networkAdapter->netmask)) {
      continue;
    }
    bool same = true;
    for (size_t i = 0; i < 4; ++i) {
      same = same && (((*device.ipAddress)[i] & networkAdapter->netmask[i]) ==
                      (networkAdapter->address[i] & networkAdapter->netmask[i]));
    }
    device.sameSubnet = same;
  }
  return devices;
}

std::expected<EthernetDiscoveryResult, std::string> discoverEthernetDevices(
    const std::string& networkAdapter, std::chrono::milliseconds duration) {
  if (auto why = checkDiscoveryDuration(duration)) {
    return std::unexpected(*why);
  }
  auto resolved = resolveNetworkAdapter(networkAdapter);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }
  const auto mac = parseMac(resolved->macLinux);
  if (!mac) {
    return std::unexpected("the network adapter " + resolved->name + " has no MAC address");
  }
  auto socket = EthernetSocket::open(resolved->name);
  if (!socket) {
    return std::unexpected(socket.error());
  }

  EthernetDiscoveryResult result;
  result.networkAdapter = resolved->name;
  result.durationMs = duration.count();
  result.answerDelayMs = listIdentityAnswerDelay(duration).count();

  const auto start = Clock::now();
  if (auto sent = socket->send(makeDcpIdentifyAll(*mac)); !sent) {
    result.warnings.push_back("cannot send DCP Identify All: " + sent.error());
  }
  if (!socket->ipv4()) {
    result.warnings.emplace_back(
        "ListIdentity was not sent, because the network adapter has no IPv4 address. A drive with "
        "EtherNet/IP firmware answers only over IP.");
  } else if (auto sent = socket->send(makeListIdentity(*mac, socket->ipv4()->address,
                                                       listIdentityAnswerDelay(duration)));
             !sent) {
    result.warnings.push_back("cannot send ListIdentity: " + sent.error());
  }

  std::vector<DiscoveryAnswer> answers;
  const auto deadline = start + duration;
  for (auto now = Clock::now(); now < deadline; now = Clock::now()) {
    auto frame = socket->receive(std::chrono::ceil<std::chrono::milliseconds>(deadline - now));
    if (!frame) {
      return std::unexpected("cannot receive on " + resolved->name + ": " + frame.error());
    }
    if (!*frame) {
      continue;
    }
    if (auto answer = parseDiscoveryFrame(**frame)) {
      answers.push_back(std::move(*answer));
    }
  }
  result.devices = mergeDiscoveryAnswers(answers, socket->ipv4());
  return result;
}

}  // namespace mm::comm
