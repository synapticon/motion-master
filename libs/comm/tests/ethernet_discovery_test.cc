#include "comm/ethernet_discovery.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <vector>

namespace {

using mm::comm::DiscoveryAnswer;
using mm::comm::DiscoveryMethod;
using mm::comm::EthernetFirmware;
using mm::comm::Ipv4Address;
using mm::comm::Ipv4Interface;
using mm::comm::MacAddress;
using std::chrono::milliseconds;

constexpr MacAddress kHost = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
constexpr MacAddress kDrive = {0x40, 0x49, 0x8A, 0x01, 0x29, 0x3C};
constexpr MacAddress kLldpSource = {0x02, 0x00, 0x00, 0x1E, 0x99, 0x01};

void appendBe16(std::vector<uint8_t>& bytes, uint16_t value) {
  bytes.push_back(static_cast<uint8_t>(value >> 8U));
  bytes.push_back(static_cast<uint8_t>(value & 0xFFU));
}

void appendLe16(std::vector<uint8_t>& bytes, uint16_t value) {
  bytes.push_back(static_cast<uint8_t>(value & 0xFFU));
  bytes.push_back(static_cast<uint8_t>(value >> 8U));
}

std::vector<uint8_t> ethernetHeader(const MacAddress& destination, const MacAddress& source,
                                    uint16_t etherType) {
  std::vector<uint8_t> frame(destination.begin(), destination.end());
  frame.insert(frame.end(), source.begin(), source.end());
  appendBe16(frame, etherType);
  return frame;
}

std::vector<uint8_t> dcpResponse(uint32_t xid) {
  auto frame = ethernetHeader(kHost, kDrive, 0x8892);
  appendBe16(frame, 0xFEFF);
  frame.push_back(0x05);  // Identify
  frame.push_back(0x01);  // success
  appendBe16(frame, static_cast<uint16_t>(xid >> 16U));
  appendBe16(frame, static_cast<uint16_t>(xid & 0xFFFFU));
  appendBe16(frame, 0);
  std::vector<uint8_t> blocks;
  // IP parameter: block info, address, mask, gateway.
  blocks.insert(blocks.end(), {0x01, 0x02, 0x00, 14, 0x00, 0x01, 192, 168, 100, 20, 255, 255, 255,
                               0, 0, 0, 0, 0});
  // Name of station "drive-a" (7 bytes), padded to an even length.
  blocks.insert(blocks.end(),
                {0x02, 0x02, 0x00, 9, 0x00, 0x00, 'd', 'r', 'i', 'v', 'e', '-', 'a', 0x00});
  // Device ID: block info, vendor ID, device ID.
  blocks.insert(blocks.end(), {0x02, 0x03, 0x00, 6, 0x00, 0x00, 0x01, 0x2C, 0x00, 0x07});
  appendBe16(frame, static_cast<uint16_t>(blocks.size()));
  frame.insert(frame.end(), blocks.begin(), blocks.end());
  return frame;
}

std::vector<uint8_t> listIdentityResponse() {
  auto frame = ethernetHeader(kHost, kDrive, 0x0800);
  std::vector<uint8_t> identity;
  appendLe16(identity, 1);                                    // item count
  appendLe16(identity, 0x000C);                               // identity item
  appendLe16(identity, 0);                                    // item length, not read
  appendLe16(identity, 1);                                    // protocol version
  identity.insert(identity.end(), 16, 0);                     // socket address
  appendLe16(identity, 1783);                                 // vendor ID
  appendLe16(identity, 12);                                   // device type
  appendLe16(identity, 1025);                                 // product code
  identity.insert(identity.end(), {5, 6});                    // revision
  appendLe16(identity, 0);                                    // status
  identity.insert(identity.end(), {0x92, 0x07, 0x00, 0x00});  // serial number 1938
  identity.push_back(8);
  identity.insert(identity.end(), {'A', 'c', 't', 'i', 'l', 'i', 'n', 'k'});
  identity.push_back(3);  // state

  std::vector<uint8_t> eip;
  appendLe16(eip, 0x0063);
  appendLe16(eip, static_cast<uint16_t>(identity.size()));
  eip.insert(eip.end(), 20, 0);
  eip.insert(eip.end(), identity.begin(), identity.end());

  const auto udpLength = static_cast<uint16_t>(8 + eip.size());
  frame.insert(frame.end(), {0x45, 0x00});
  appendBe16(frame, static_cast<uint16_t>(20 + udpLength));
  frame.insert(frame.end(), {0, 0, 0, 0, 64, 17, 0, 0});
  frame.insert(frame.end(), {192, 168, 100, 17, 192, 168, 100, 1});
  appendBe16(frame, 44818);
  appendBe16(frame, 44818);
  appendBe16(frame, udpLength);
  appendBe16(frame, 0);
  frame.insert(frame.end(), eip.begin(), eip.end());
  return frame;
}

std::vector<uint8_t> lldpFrame(Ipv4Address management) {
  auto frame = ethernetHeader({0x01, 0x80, 0xC2, 0x00, 0x00, 0x0E}, kLldpSource, 0x88CC);
  auto tlv = [&frame](uint8_t type, const std::vector<uint8_t>& value) {
    appendBe16(frame, static_cast<uint16_t>((static_cast<unsigned>(type) << 9U) | value.size()));
    frame.insert(frame.end(), value.begin(), value.end());
  };
  std::vector<uint8_t> chassis = {4};
  chassis.insert(chassis.end(), kDrive.begin(), kDrive.end());
  tlv(1, chassis);
  tlv(2, {7, 'p', '1'});
  tlv(3, {0, 30});
  tlv(5, {'A', 'c', 't', 'i', 'l', 'i', 'n', 'k'});
  std::vector<uint8_t> address = {5, 1};
  address.insert(address.end(), management.begin(), management.end());
  address.insert(address.end(), {2, 0, 0, 0, 1, 0});
  tlv(8, address);
  tlv(0, {});
  return frame;
}

TEST(EthernetDiscoveryTest, DurationMustLeaveTimeForTheAnswerDelay) {
  EXPECT_TRUE(mm::comm::checkDiscoveryDuration(milliseconds(999)));
  EXPECT_FALSE(mm::comm::checkDiscoveryDuration(milliseconds(1000)));
  EXPECT_FALSE(mm::comm::checkDiscoveryDuration(milliseconds(60000)));
  EXPECT_TRUE(mm::comm::checkDiscoveryDuration(milliseconds(60001)));
  EXPECT_EQ(mm::comm::listIdentityAnswerDelay(milliseconds(2000)), milliseconds(1500));
}

TEST(EthernetDiscoveryTest, DcpIdentifyAllGoesToTheProfinetMulticastAddress) {
  const auto frame = mm::comm::makeDcpIdentifyAll(kHost);
  ASSERT_EQ(frame.size(), 60U);
  EXPECT_EQ(std::vector<uint8_t>(frame.begin(), frame.begin() + 6),
            (std::vector<uint8_t>{0x01, 0x0E, 0xCF, 0x00, 0x00, 0x00}));
  EXPECT_EQ(std::vector<uint8_t>(frame.begin() + 6, frame.begin() + 12),
            std::vector<uint8_t>(kHost.begin(), kHost.end()));
  // EtherType, frame ID, service Identify, request.
  EXPECT_EQ(std::vector<uint8_t>(frame.begin() + 12, frame.begin() + 18),
            (std::vector<uint8_t>{0x88, 0x92, 0xFE, 0xFE, 0x05, 0x00}));
  // The all selector.
  EXPECT_EQ(std::vector<uint8_t>(frame.begin() + 26, frame.begin() + 30),
            (std::vector<uint8_t>{0xFF, 0xFF, 0x00, 0x00}));
}

TEST(EthernetDiscoveryTest, ListIdentityCarriesTheAnswerDelayInTheSenderContext) {
  const auto frame = mm::comm::makeListIdentity(kHost, {192, 168, 100, 1}, milliseconds(1500));
  ASSERT_EQ(frame.size(), 66U);
  EXPECT_EQ(frame[12], 0x08);
  EXPECT_EQ(frame[13], 0x00);
  // A valid IPv4 header sums to 0xFFFF with its checksum.
  uint32_t sum = 0;
  for (size_t i = 14; i < 34; i += 2) {
    sum += (static_cast<uint32_t>(frame[i]) << 8U) | frame[i + 1];
  }
  while (sum > 0xFFFFU) {
    sum = (sum & 0xFFFFU) + (sum >> 16U);
  }
  EXPECT_EQ(sum, 0xFFFFU);
  // The sender context starts after the command, the length, the session and the status.
  const size_t context = 14 + 20 + 8 + 12;
  EXPECT_EQ(frame[context], 0xDC);  // 1500 is 0x05DC
  EXPECT_EQ(frame[context + 1], 0x05);
  // The request is not taken for an answer when it comes back.
  EXPECT_FALSE(mm::comm::parseDiscoveryFrame(frame));
}

TEST(EthernetDiscoveryTest, ParsesADcpResponseToItsOwnRequest) {
  const auto answer = mm::comm::parseDiscoveryFrame(dcpResponse(0x4D4D3538));
  ASSERT_TRUE(answer);
  EXPECT_EQ(answer->method, DiscoveryMethod::kDcp);
  EXPECT_EQ(answer->firmware, EthernetFirmware::kProfinet);
  EXPECT_EQ(answer->macAddress, kDrive);
  EXPECT_EQ(answer->ipAddress, (Ipv4Address{192, 168, 100, 20}));
  EXPECT_EQ(answer->name, "drive-a");
  EXPECT_EQ(answer->vendorId, 0x012C);
  EXPECT_EQ(answer->deviceId, 0x0007);
}

TEST(EthernetDiscoveryTest, IgnoresADcpResponseToAnotherRequest) {
  EXPECT_FALSE(mm::comm::parseDiscoveryFrame(dcpResponse(0x12345678)));
}

TEST(EthernetDiscoveryTest, ParsesAListIdentityResponse) {
  const auto answer = mm::comm::parseDiscoveryFrame(listIdentityResponse());
  ASSERT_TRUE(answer);
  EXPECT_EQ(answer->method, DiscoveryMethod::kListIdentity);
  EXPECT_EQ(answer->firmware, EthernetFirmware::kEthernetIp);
  EXPECT_EQ(answer->macAddress, kDrive);
  EXPECT_EQ(answer->ipAddress, (Ipv4Address{192, 168, 100, 17}));
  EXPECT_EQ(answer->name, "Actilink");
  EXPECT_EQ(answer->vendorId, 1783);
  EXPECT_EQ(answer->productCode, 1025);
  EXPECT_EQ(answer->serialNumber, 1938U);
}

TEST(EthernetDiscoveryTest, TakesTheLldpMacAddressFromTheChassisId) {
  const auto answer = mm::comm::parseDiscoveryFrame(lldpFrame({192, 168, 100, 17}));
  ASSERT_TRUE(answer);
  EXPECT_EQ(answer->method, DiscoveryMethod::kLldp);
  EXPECT_EQ(answer->firmware, EthernetFirmware::kUnknown);
  EXPECT_EQ(answer->macAddress, kDrive);
  EXPECT_EQ(answer->ipAddress, (Ipv4Address{192, 168, 100, 17}));
  EXPECT_EQ(answer->name, "Actilink");
}

TEST(EthernetDiscoveryTest, MergesTheAnswersOfOneDevice) {
  std::vector<DiscoveryAnswer> answers;
  answers.push_back(*mm::comm::parseDiscoveryFrame(listIdentityResponse()));
  answers.push_back(*mm::comm::parseDiscoveryFrame(lldpFrame({192, 168, 100, 17})));
  answers.push_back(*mm::comm::parseDiscoveryFrame(lldpFrame({192, 168, 100, 17})));
  const auto devices = mm::comm::mergeDiscoveryAnswers(
      answers, Ipv4Interface{{192, 168, 100, 1}, {255, 255, 255, 0}});
  ASSERT_EQ(devices.size(), 1U);
  EXPECT_EQ(devices[0].firmware, EthernetFirmware::kEthernetIp);
  EXPECT_EQ(devices[0].sameSubnet, true);
  EXPECT_EQ(devices[0].foundBy,
            (std::vector<DiscoveryMethod>{DiscoveryMethod::kListIdentity, DiscoveryMethod::kLldp}));
}

TEST(EthernetDiscoveryTest, MergesADeviceWithoutAnIpAddressByItsMacAddress) {
  std::vector<DiscoveryAnswer> answers;
  answers.push_back(*mm::comm::parseDiscoveryFrame(lldpFrame({0, 0, 0, 0})));
  answers.push_back(*mm::comm::parseDiscoveryFrame(listIdentityResponse()));
  const auto devices = mm::comm::mergeDiscoveryAnswers(answers, std::nullopt);
  ASSERT_EQ(devices.size(), 1U);
  EXPECT_EQ(devices[0].ipAddress, (Ipv4Address{192, 168, 100, 17}));
  EXPECT_FALSE(devices[0].sameSubnet);
}

TEST(EthernetDiscoveryTest, ListsADeviceWithoutAnIpAddressWithNull) {
  std::vector<DiscoveryAnswer> answers;
  answers.push_back(*mm::comm::parseDiscoveryFrame(lldpFrame({0, 0, 0, 0})));
  const auto devices = mm::comm::mergeDiscoveryAnswers(
      answers, Ipv4Interface{{192, 168, 100, 1}, {255, 255, 255, 0}});
  ASSERT_EQ(devices.size(), 1U);
  const nlohmann::json j = devices[0];
  EXPECT_TRUE(j["ipAddress"].is_null());
  EXPECT_TRUE(j["sameSubnet"].is_null());
  EXPECT_TRUE(j["firmware"].is_null());
  EXPECT_EQ(j["macAddress"], "40:49:8a:01:29:3c");
  EXPECT_EQ(j["foundBy"], nlohmann::json::array({"lldp"}));
}

TEST(EthernetDiscoveryTest, MarksADeviceOnAnotherSubnet) {
  std::vector<DiscoveryAnswer> answers;
  answers.push_back(*mm::comm::parseDiscoveryFrame(listIdentityResponse()));
  const auto devices =
      mm::comm::mergeDiscoveryAnswers(answers, Ipv4Interface{{10, 0, 0, 1}, {255, 255, 255, 0}});
  ASSERT_EQ(devices.size(), 1U);
  EXPECT_EQ(devices[0].sameSubnet, false);
}

TEST(EthernetDiscoveryTest, KeepsTwoDevicesApart) {
  auto first = *mm::comm::parseDiscoveryFrame(listIdentityResponse());
  auto second = first;
  second.macAddress = MacAddress{0x40, 0x49, 0x8A, 0x01, 0x29, 0x3D};
  second.ipAddress = Ipv4Address{192, 168, 100, 18};
  const std::vector<DiscoveryAnswer> answers = {first, second};
  const auto devices = mm::comm::mergeDiscoveryAnswers(answers, std::nullopt);
  ASSERT_EQ(devices.size(), 2U);
  EXPECT_EQ(devices[0].ipAddress, (Ipv4Address{192, 168, 100, 17}));
  EXPECT_EQ(devices[1].ipAddress, (Ipv4Address{192, 168, 100, 18}));
}

}  // namespace
