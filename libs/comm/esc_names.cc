#include "comm/esc_names.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <string>
#include <vector>

namespace mm::comm {

namespace {

struct BitName {
  uint16_t bit = 0;
  std::string_view name;
};

constexpr std::array kMailboxProtocols{
    BitName{0x01, "AoE"}, BitName{0x02, "EoE"}, BitName{0x04, "CoE"},
    BitName{0x08, "FoE"}, BitName{0x10, "SoE"}, BitName{0x20, "VoE"},
};

constexpr std::array kCoeDetails{
    BitName{0x01, "SDO"},        BitName{0x02, "SDO Info"}, BitName{0x04, "PDO Assign"},
    BitName{0x08, "PDO Config"}, BitName{0x10, "Upload"},   BitName{0x20, "Complete Access"},
};

template <std::size_t N>
std::vector<std::string_view> namesOfSetBits(uint16_t bits, const std::array<BitName, N>& table) {
  std::vector<std::string_view> names;
  for (const auto& entry : table) {
    if ((bits & entry.bit) != 0) {
      names.push_back(entry.name);
    }
  }
  return names;
}

// The configuration registers of the sync managers, 8 bytes each from 0x0800.
constexpr uint16_t kSyncManagerRegistersStart = 0x0800;
constexpr uint16_t kSyncManagerRegistersEnd = 0x0880;

}  // namespace

std::vector<std::string_view> mailboxProtocolNames(uint16_t protocols) {
  return namesOfSetBits(protocols, kMailboxProtocols);
}

std::vector<std::string_view> coeDetailNames(uint8_t coeDetails) {
  return namesOfSetBits(coeDetails, kCoeDetails);
}

std::optional<std::string_view> syncManagerTypeName(uint8_t type) {
  switch (type) {
    case 0:
      return "Unused";
    case 1:
      return "Mailbox Out";
    case 2:
      return "Mailbox In";
    case 3:
      return "Outputs";
    case 4:
      return "Inputs";
    case 5:
      return "Dynamic outputs";
    case 6:
      return "Dynamic inputs";
    default:
      return std::nullopt;
  }
}

std::optional<std::string_view> fmmuUsageName(uint8_t usage) {
  switch (usage) {
    case 0x00:
    case 0xFF:
      return "Unused";
    case 0x01:
      return "Outputs";
    case 0x02:
      return "Inputs";
    case 0x03:
      return "SyncM status";
    case 0x04:
      return "Dynamic outputs";
    case 0x05:
      return "Dynamic inputs";
    default:
      return std::nullopt;
  }
}

std::optional<std::string_view> fmmuTypeName(uint8_t type, uint16_t physicalStart) {
  if (physicalStart >= kSyncManagerRegistersStart && physicalStart < kSyncManagerRegistersEnd) {
    return "Mailbox state";
  }
  switch (type) {
    case 0:
      return "Unused";
    case 1:
      return "Inputs";
    case 2:
      return "Outputs";
    case 3:
      return "Inputs and outputs";
    default:
      return std::nullopt;
  }
}

nlohmann::json nameJson(std::optional<std::string_view> name) {
  return name ? nlohmann::json(std::string(*name)) : nlohmann::json(nullptr);
}

nlohmann::json namesJson(const std::vector<std::string_view>& names) {
  nlohmann::json array = nlohmann::json::array();
  std::transform(names.begin(), names.end(), std::back_inserter(array),
                 [](std::string_view name) { return std::string(name); });
  return array;
}

}  // namespace mm::comm
