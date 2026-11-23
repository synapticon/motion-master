#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mm::comm {

using MacAddress = std::array<uint8_t, 6>;
using Ipv4Address = std::array<uint8_t, 4>;

/// @brief A way to find a device on an Ethernet adapter without a connection to it.
enum class DiscoveryMethod : uint8_t {
  kDcp,           ///< PROFINET DCP Identify All. The device answers with a raw Ethernet frame.
  kListIdentity,  ///< EtherNet/IP ListIdentity. The device answers over UDP.
  kLldp,          ///< LLDP. The device sends the frame by itself, about every 30 s.
};

/// @brief The name of @p method in the API: `dcp`, `list-identity` or `lldp`.
std::string_view toString(DiscoveryMethod method);

/// @brief The firmware of a device, as far as an answer tells it.
enum class EthernetFirmware : uint8_t {
  kUnknown,
  kEthernetIp,
  kProfinet,
};

/// @brief The name of @p firmware in the API: `ethernet-ip` or `profinet`. Empty for kUnknown.
std::string_view toString(EthernetFirmware firmware);

/// @brief The IPv4 address and network mask of an adapter.
struct Ipv4Interface {
  Ipv4Address address{};
  Ipv4Address netmask{};
};

/// @brief One answer to one discovery method, decoded from one frame.
///
/// A field that the method does not carry is empty.
struct DiscoveryAnswer {
  DiscoveryMethod method = DiscoveryMethod::kDcp;
  EthernetFirmware firmware = EthernetFirmware::kUnknown;
  /// The MAC address of the device. The source address of the frame for DCP and ListIdentity, and
  /// the chassis ID for LLDP. A device sends its LLDP frames from another MAC address than its
  /// other frames, so the source address of an LLDP frame is never used.
  std::optional<MacAddress> macAddress;
  /// The IPv4 address of the device. 0.0.0.0 when the device reports that it has none.
  std::optional<Ipv4Address> ipAddress;
  std::string name;
  std::optional<uint16_t> vendorId;
  std::optional<uint16_t> deviceId;
  std::optional<uint16_t> productCode;
  std::optional<uint32_t> serialNumber;
};

/// @brief One device, merged from every answer that belongs to it.
struct DiscoveredEthernetDevice {
  std::optional<Ipv4Address> ipAddress;  ///< Empty when the device has no IP address.
  std::optional<MacAddress> macAddress;
  std::string name;
  EthernetFirmware firmware = EthernetFirmware::kUnknown;
  std::optional<uint16_t> vendorId;
  std::optional<uint16_t> deviceId;
  std::optional<uint16_t> productCode;
  std::optional<uint32_t> serialNumber;
  /// Whether the device is on the subnet of the adapter. Empty when the device or the adapter has
  /// no IP address.
  std::optional<bool> sameSubnet;
  std::vector<DiscoveryMethod> foundBy;  ///< Every method that found the device, without repeats.
};

/// @brief The result of one discovery on one adapter.
struct EthernetDiscoveryResult {
  std::string adapter;
  int64_t durationMs = 0;
  int64_t answerDelayMs = 0;
  std::vector<DiscoveredEthernetDevice> devices;
  /// What the discovery could not do, such as a ListIdentity request that was not sent because the
  /// adapter has no IPv4 address.
  std::vector<std::string> warnings;
};

void to_json(nlohmann::json& j, const DiscoveredEthernetDevice& device);
void to_json(nlohmann::json& j, const EthernetDiscoveryResult& result);

/// The shortest discovery time, exclusive. The answer delay of ListIdentity is the discovery time
/// minus this margin, which covers the trip back and the time the device needs for the request.
inline constexpr std::chrono::milliseconds kDiscoveryAnswerMargin{500};
/// The longest discovery time. The answer delay of ListIdentity is a 16-bit number of ms.
inline constexpr std::chrono::milliseconds kMaxDiscoveryDuration{60000};
/// The discovery time of a request that does not set one.
inline constexpr std::chrono::milliseconds kDefaultDiscoveryDuration{2000};

/// @brief Checks a discovery time. Returns the reason when it is out of range.
std::optional<std::string> checkDiscoveryDuration(std::chrono::milliseconds duration);

/// @brief The longest answer delay a ListIdentity request asks for, for a discovery of @p duration.
std::chrono::milliseconds listIdentityAnswerDelay(std::chrono::milliseconds duration);

/// @brief A DCP Identify All request from @p source, padded to the shortest Ethernet frame.
std::vector<uint8_t> makeDcpIdentifyAll(const MacAddress& source);

/// @brief A ListIdentity request as a whole Ethernet frame: a UDP broadcast from @p sourceIp to
/// port 44818, which asks every device to answer within @p answerDelay.
///
/// The frame is built by hand, so it leaves through this adapter only, whatever the routing table
/// says about 255.255.255.255.
std::vector<uint8_t> makeListIdentity(const MacAddress& source, const Ipv4Address& sourceIp,
                                      std::chrono::milliseconds answerDelay);

/// @brief Decodes one received Ethernet frame. Returns an answer when the frame is a DCP Identify
/// response to a request of this program, a ListIdentity response, or an LLDP frame.
std::optional<DiscoveryAnswer> parseDiscoveryFrame(std::span<const uint8_t> frame);

/// @brief Merges answers into one row per device.
///
/// Two answers belong to one device when they carry the same IP address, other than 0.0.0.0, or
/// the same MAC address. The rows keep the order in which their devices first answered.
std::vector<DiscoveredEthernetDevice> mergeDiscoveryAnswers(
    std::span<const DiscoveryAnswer> answers, const std::optional<Ipv4Interface>& adapter);

/// @brief Finds the devices on @p adapter.
///
/// Sends DCP Identify All and ListIdentity at the same time, collects the answers for
/// @p duration, and listens for LLDP during that time. Opens no connection to a device. Needs raw
/// access to the adapter: CAP_NET_RAW on Linux, Npcap on Windows, and read and write access to
/// /dev/bpf* on macOS.
/// @param adapter  The name of the adapter, or its MAC address, as for the SOEM driver.
/// @param duration How long to collect answers. See checkDiscoveryDuration.
std::expected<EthernetDiscoveryResult, std::string> discoverEthernetDevices(
    const std::string& adapter, std::chrono::milliseconds duration);

}  // namespace mm::comm
