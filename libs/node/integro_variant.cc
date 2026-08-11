#include "node/integro_variant.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mm::node {

namespace {

constexpr size_t kHeaderSize = 128;
constexpr size_t kOptionsOffset = 132;  // Header, plus two reserved bytes and the two-byte count.

constexpr size_t kSignatureOffset = 4;
constexpr size_t kChipIdOffset = 68;
constexpr size_t kSerialNumberOffset = 80;
constexpr size_t kSerialNumberSize = 24;
/// The 8-byte MAC field opens with two zero bytes; the address is the six that follow.
constexpr size_t kMacAddressOffset = 106;
constexpr size_t kCustomerIdOffset = 112;
constexpr size_t kOperationModeOffset = 116;
constexpr size_t kOptionCountOffset = 130;

uint16_t readU16(std::span<const uint8_t> bytes, size_t offset) {
  return static_cast<uint16_t>(static_cast<uint16_t>(bytes[offset]) |
                               static_cast<uint16_t>(bytes[offset + 1]) << 8);
}

uint32_t readU32(std::span<const uint8_t> bytes, size_t offset) {
  return static_cast<uint32_t>(bytes[offset]) | static_cast<uint32_t>(bytes[offset + 1]) << 8 |
         static_cast<uint32_t>(bytes[offset + 2]) << 16 |
         static_cast<uint32_t>(bytes[offset + 3]) << 24;
}

std::string toHex(std::span<const uint8_t> bytes) {
  std::string hex;
  hex.reserve(bytes.size() * 2);
  for (uint8_t byte : bytes) {
    hex += std::format("{:02x}", byte);
  }
  return hex;
}

// ── The option catalogue ───────────────────────────────────────────────────────────────────────
// Every option within a category sets the same SoC values, so each list is named once and shared by
// the rows that use it. Written out per row, two rows of one category could drift apart.

constexpr std::array<std::string_view, 1> kPhaseGainSoc = {"adc_phase_current_sensor_gain"};
constexpr std::array<std::string_view, 15> kCurrentLimitSoc = {
    "drive_max_continuous_current_mA",
    "drive_max_current_mA",
    "drive_max_mechanical_power_watts",
    "protection_hardware_overcurrent_mA",
    "fet_max_accumulated_heat_cold_amp_squared_second",
    "fet_max_accumulated_heat_hot_amp_squared_second",
    "fet_cold_temperature_degree_celsius",
    "fet_hot_temperature_degree_celsius",
    "fet_rated_current_cold_i2t_integration_deci_amp",
    "cool_down_period_fet_overheat_s",
    "cool_down_period_fet_startup_s",
    "nominal_drive_temperature_degree_celsius",
    "DC_current_peak_ripple_32_kHz_threshold_mA",
    "DC_current_peak_ripple_64_kHz_threshold_mA",
    "AC_current_threshold_mA"};
constexpr std::array<std::string_view, 1> kMultiturnSoc = {"is_integrated_encoder_mt_used"};
constexpr std::array<std::string_view, 2> kEncoderResolutionSoc = {
    "integrated_encoder_st_bits_used", "should_integrated_encoder_noise_be_added"};
constexpr std::array<std::string_view, 1> kCyclicModeSoc = {"are_cyclic_opmodes_allowed"};
constexpr std::array<std::string_view, 1> kInputFrequencySoc = {"is_input_frequency_limited"};
constexpr std::array<std::string_view, 1> kAccelerationsSoc = {"are_accelerations_limited"};
constexpr std::array<std::string_view, 1> kDioBitmapSoc = {"dio_allowed_bitmap"};
constexpr std::array<std::string_view, 1> kDigitalInputSoc = {"di_allowed_bitmap"};
constexpr std::array<std::string_view, 1> kDigitalOutputSoc = {"do_allowed_bitmap"};
constexpr std::array<std::string_view, 1> kEncoderAllowedSoc = {"encoder_allowed_bitmap"};
constexpr std::array<std::string_view, 1> kAnalogAllowedSoc = {"analog_inputs_allowed_bitmap"};
constexpr std::array<std::string_view, 1> kDcClockSoc = {"is_dc_clock_disabled"};

// One array per option that has alternatives. Code 15 appears in the encoder-resolution lists and
// has no catalogue entry of its own; the lists are recorded as the source table has them.
constexpr std::array<uint16_t, 4> kNotEthernetIp = {1, 2, 3, 39};
constexpr std::array<uint16_t, 4> kNotEtherCat = {0, 2, 3, 39};
constexpr std::array<uint16_t, 4> kNotProfinet = {0, 1, 3, 39};
constexpr std::array<uint16_t, 4> kNotCan = {0, 1, 2, 39};
constexpr std::array<uint16_t, 4> kNotEthernetIpLxm = {0, 1, 2, 3};
constexpr std::array<uint16_t, 1> kNotGain60 = {5};
constexpr std::array<uint16_t, 1> kNotGain80 = {4};
constexpr std::array<uint16_t, 4> kNotLimit1025 = {5, 7, 8, 9};
constexpr std::array<uint16_t, 4> kNotLimit2060 = {5, 6, 8, 9};
constexpr std::array<uint16_t, 4> kNotLimit2550 = {4, 6, 7, 9};
constexpr std::array<uint16_t, 4> kNotLimit40120 = {4, 6, 7, 8};
constexpr std::array<uint16_t, 1> kNotMultiturn = {11};
constexpr std::array<uint16_t, 1> kNotSingleturn = {10};
constexpr std::array<uint16_t, 4> kNot12Bits = {13, 14, 15, 20};
constexpr std::array<uint16_t, 4> kNot14Bits = {12, 14, 15, 20};
constexpr std::array<uint16_t, 4> kNot18Bits = {12, 13, 15, 20};
constexpr std::array<uint16_t, 4> kNot11Bits = {12, 13, 14, 15};
constexpr std::array<uint16_t, 1> kNotCyclicInactive = {17};
constexpr std::array<uint16_t, 1> kNotCyclicActive = {16};
constexpr std::array<uint16_t, 3> kNotDioNone = {22, 23, 24};
constexpr std::array<uint16_t, 3> kNotDio34 = {21, 23, 24};
constexpr std::array<uint16_t, 3> kNotDio124 = {21, 22, 24};
constexpr std::array<uint16_t, 3> kNotDio123 = {21, 22, 23};

constexpr std::array<std::string_view, 1> kMpnEthernetIp = {"EI"};
constexpr std::array<std::string_view, 1> kMpnEtherCat = {"EC"};
constexpr std::array<std::string_view, 1> kMpnProfinet = {"EP"};
constexpr std::array<std::string_view, 1> kMpnCan = {"CB"};
constexpr std::array<std::string_view, 1> kMpnEthernetIpLxm = {"EI+CSE"};
constexpr std::array<std::string_view, 2> kMpnGain60 = {"025", "060"};
constexpr std::array<std::string_view, 2> kMpnGain80 = {"050", "120"};
constexpr std::array<std::string_view, 1> kMpn025 = {"025"};
constexpr std::array<std::string_view, 1> kMpn060 = {"060"};
constexpr std::array<std::string_view, 1> kMpn050 = {"050"};
constexpr std::array<std::string_view, 1> kMpn120 = {"120"};
constexpr std::array<std::string_view, 4> kMpnMultiturn = {"ML", "MM", "MH", "MX"};
constexpr std::array<std::string_view, 4> kMpnSingleturn = {"SL", "SM", "SH", "SX"};
constexpr std::array<std::string_view, 2> kMpn12Bits = {"ML", "SL"};
constexpr std::array<std::string_view, 2> kMpn14Bits = {"MM", "SM"};
constexpr std::array<std::string_view, 2> kMpn18Bits = {"MH", "SH"};
constexpr std::array<std::string_view, 1> kMpnCyclicInactive = {"00"};
constexpr std::array<std::string_view, 1> kMpnCyclicActive = {"01"};

/// Codes in ascending order. The gaps are real — there is no code 15.
///
/// Add a row here when a firmware release adds an option code; this table is maintained by hand
/// (see @c integroVariantOptions()). Nothing detects that it has gone stale, and it does not need
/// to: an unrecognised code still decodes and is still reported, without a category or meaning.
/// Take the new row from the commissioning tooling's table, which carries the category, SoC
/// variables, alternatives and part-number segments; the firmware's own enum has only the id.
constexpr std::array<IntegroVariantOption, 39> kIntegroVariantOptions = {{
    {0, "Fieldbus Protocol", "EtherNet/IP - CiA 402", {}, kNotEthernetIp, kMpnEthernetIp},
    {1, "Fieldbus Protocol", "EtherCAT", {}, kNotEtherCat, kMpnEtherCat},
    {2, "Fieldbus Protocol", "PROFINET - CiA 402", {}, kNotProfinet, kMpnProfinet},
    {3, "Fieldbus Protocol", "CAN", {}, kNotCan, kMpnCan},
    {4, "Phase current sensor gain", "gain for size 60", kPhaseGainSoc, kNotGain60, kMpnGain60},
    {5, "Phase current sensor gain", "gain for size 80", kPhaseGainSoc, kNotGain80, kMpnGain80},
    {6, "Current Limits", "10/25", kCurrentLimitSoc, kNotLimit1025, kMpn025},
    {7, "Current Limits", "20/60", kCurrentLimitSoc, kNotLimit2060, kMpn060},
    {8, "Current Limits", "25/50", kCurrentLimitSoc, kNotLimit2550, kMpn050},
    {9, "Current Limits", "40/120", kCurrentLimitSoc, kNotLimit40120, kMpn120},
    {10, "Multiturn ability", "MT", kMultiturnSoc, kNotMultiturn, kMpnMultiturn},
    {11, "Multiturn ability", "ST", kMultiturnSoc, kNotSingleturn, kMpnSingleturn},
    {12, "Encoder Resolution", "12 bits", kEncoderResolutionSoc, kNot12Bits, kMpn12Bits},
    {13, "Encoder Resolution", "14 bits", kEncoderResolutionSoc, kNot14Bits, kMpn14Bits},
    {14, "Encoder Resolution", "18 bits", kEncoderResolutionSoc, kNot18Bits, kMpn18Bits},
    {16, "Cyclic mode", "inactive", kCyclicModeSoc, kNotCyclicInactive, kMpnCyclicInactive},
    {17, "Cyclic mode", "active", kCyclicModeSoc, kNotCyclicActive, kMpnCyclicActive},
    {18,
     "Performance Limitation",
     "Input frequency is limited to 4Hz to make the drive unable to follow highly dynamic "
     "trajectories",
     kInputFrequencySoc,
     {},
     {}},
    {19,
     "Performance Limitation",
     "Cyclic accelerations are limited to a low value to make the drive unable to react to large "
     "travel demands quickly",
     kAccelerationsSoc,
     {},
     {}},
    {20, "Encoder Resolution", "11 bits", kEncoderResolutionSoc, kNot11Bits, {}},
    {21, "Digital IOs allowed", "None (0b0000)", kDioBitmapSoc, kNotDioNone, {}},
    {22, "Digital IOs allowed", "DIO3, DIO4 (0b1100)", kDioBitmapSoc, kNotDio34, {}},
    {23, "Digital IOs allowed", "DIO1, DIO2, DIO4 (0b1011)", kDioBitmapSoc, kNotDio124, {}},
    {24, "Digital IOs allowed", "DIO1, DIO2, DIO3 (0b0111)", kDioBitmapSoc, kNotDio123, {}},
    {25, "Encoder connectors allowed", "Only integrated (0b01)", kEncoderAllowedSoc, {}, {}},
    {26,
     "Analog inputs allowed",
     "Only motor temperature / internal (0b001)",
     kAnalogAllowedSoc,
     {},
     {}},
    {27, "DC clock", "Disable the DC clock synchronization", kDcClockSoc, {}, {}},
    {28, "Digital IOs allowed", "Disable DI1", kDigitalInputSoc, {}, {}},
    {29, "Digital IOs allowed", "Disable DO1", kDigitalOutputSoc, {}, {}},
    {30, "Digital IOs allowed", "Disable DI2", kDigitalInputSoc, {}, {}},
    {31, "Digital IOs allowed", "Disable DO2", kDigitalOutputSoc, {}, {}},
    {32, "Digital IOs allowed", "Disable DI3", kDigitalInputSoc, {}, {}},
    {33, "Digital IOs allowed", "Disable DI4", kDigitalInputSoc, {}, {}},
    {34,
     "Encoder connectors allowed",
     "Disable encoder connector 1 (Integrated encoder)",
     kEncoderAllowedSoc,
     {},
     {}},
    {35,
     "Encoder connectors allowed",
     "Disable encoder connector 2 (External encoder)",
     kEncoderAllowedSoc,
     {},
     {}},
    {36,
     "Analog inputs allowed",
     "Disable AI Internal (Motor temperature sensor)",
     kAnalogAllowedSoc,
     {},
     {}},
    {37, "Analog inputs allowed", "Disable AI 1", kAnalogAllowedSoc, {}, {}},
    {38, "Analog inputs allowed", "Disable AI 2", kAnalogAllowedSoc, {}, {}},
    {39, "Fieldbus Protocol", "EtherNet/IP - LXM", {}, kNotEthernetIpLxm, kMpnEthernetIpLxm},
}};

constexpr std::string_view kFieldbusCategory = "Fieldbus Protocol";
constexpr uint16_t kEtherCatOption = 1;

}  // namespace

std::string_view toString(VariantOperationMode mode) {
  switch (mode) {
    case VariantOperationMode::kPassive:
      return "passive";
    case VariantOperationMode::kTrial:
      return "trial";
    case VariantOperationMode::kProduction:
      return "production";
    case VariantOperationMode::kLive:
      return "live";
  }
  return "unknown";
}

std::span<const IntegroVariantOption> integroVariantOptions() { return kIntegroVariantOptions; }

const IntegroVariantOption* integroVariantOption(uint16_t id) {
  const auto at = std::ranges::find(kIntegroVariantOptions, id, &IntegroVariantOption::id);
  return at == kIntegroVariantOptions.end() ? nullptr : &*at;
}

void to_json(nlohmann::json& j, const IntegroVariantOption& option) {
  // The three spans are materialised as owning vectors rather than handed to nlohmann directly: a
  // span of string_view is not one of the container shapes it converts on its own.
  j = nlohmann::json{
      {"id", option.id},
      {"category", option.category},
      {"meaning", option.meaning},
      {"socVariables",
       std::vector<std::string>(option.socVariables.begin(), option.socVariables.end())},
      {"incompatibleOptionIds", std::vector<uint16_t>(option.incompatibleOptionIds.begin(),
                                                      option.incompatibleOptionIds.end())},
      {"mpnSegmentCodes",
       std::vector<std::string>(option.mpnSegmentCodes.begin(), option.mpnSegmentCodes.end())},
  };
}

std::expected<IntegroVariant, std::string> parseIntegroVariant(std::span<const uint8_t> content) {
  if (content.size() < kOptionsOffset) {
    return std::unexpected(std::format(
        "the variant file is {} bytes — too short for its {}-byte header and option count "
        "({} bytes minimum)",
        content.size(), kHeaderSize, kOptionsOffset));
  }

  const uint16_t optionCount = readU16(content, kOptionCountOffset);
  if (optionCount > kMaxVariantOptions) {
    return std::unexpected(
        std::format("the variant file claims {} options, more than the {} a drive accepts",
                    optionCount, kMaxVariantOptions));
  }
  const size_t optionBytes = static_cast<size_t>(optionCount) * 2;
  if (content.size() < kOptionsOffset + optionBytes) {
    return std::unexpected(std::format(
        "the variant file claims {} options but holds only {} bytes after its header, "
        "room for {}",
        optionCount, content.size() - kOptionsOffset, (content.size() - kOptionsOffset) / 2));
  }

  IntegroVariant variant;
  variant.fileVersion = readU32(content, 0);
  std::ranges::copy(content.subspan(kSignatureOffset, variant.signature.size()),
                    variant.signature.begin());
  std::ranges::copy(content.subspan(kChipIdOffset, variant.chipId.size()), variant.chipId.begin());

  const std::span<const uint8_t> serial = content.subspan(kSerialNumberOffset, kSerialNumberSize);
  const auto serialEnd = std::ranges::find(serial, uint8_t{0});
  variant.serialNumber.assign(serial.begin(), serialEnd);

  const std::span<const uint8_t> mac = content.subspan(kMacAddressOffset, 6);
  variant.macAddress = std::format("{:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}", mac[0], mac[1],
                                   mac[2], mac[3], mac[4], mac[5]);

  variant.customerId = readU32(content, kCustomerIdOffset);
  variant.operationMode = readU16(content, kOperationModeOffset);

  variant.optionIds.reserve(optionCount);
  for (uint16_t i = 0; i < optionCount; ++i) {
    variant.optionIds.push_back(readU16(content, kOptionsOffset + static_cast<size_t>(i) * 2));
  }
  return variant;
}

std::optional<uint16_t> variantFieldbusProtocol(const IntegroVariant& variant) {
  std::optional<uint16_t> found;
  for (uint16_t id : variant.optionIds) {
    const IntegroVariantOption* option = integroVariantOption(id);
    if (option == nullptr || option->category != kFieldbusCategory) {
      continue;
    }
    if (id == kEtherCatOption) {
      return id;  // Returned at once, so no later option can displace it.
    }
    if (!found) {
      found = id;
    }
  }
  return found;
}

void to_json(nlohmann::json& j, const IntegroVariant& variant) {
  nlohmann::json options = nlohmann::json::array();
  for (uint16_t id : variant.optionIds) {
    if (const IntegroVariantOption* option = integroVariantOption(id); option != nullptr) {
      options.push_back(*option);
    } else {
      options.push_back(nlohmann::json{{"id", id}});
    }
  }

  j = nlohmann::json{
      {"fileVersion", variant.fileVersion},
      {"signature", toHex(variant.signature)},
      {"chipId", toHex(variant.chipId)},
      {"serialNumber", variant.serialNumber},
      {"macAddress", variant.macAddress},
      {"customerId", variant.customerId},
      {"operationMode", variant.operationMode},
      {"operationModeName", toString(static_cast<VariantOperationMode>(variant.operationMode))},
      {"options", std::move(options)},
  };
  if (const auto fieldbus = variantFieldbusProtocol(variant)) {
    j["fieldbusProtocol"] = *fieldbus;
  }
}

}  // namespace mm::node
