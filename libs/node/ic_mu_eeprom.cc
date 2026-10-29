#include "node/ic_mu_eeprom.h"

#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>
#include <utility>
#include <vector>

namespace mm::node::somanet {

namespace {

constexpr std::array<std::pair<IcMuI2cTarget, std::string_view>, 3> kTargetNames = {{
    {IcMuI2cTarget::kEeprom, "eeprom"},
    {IcMuI2cTarget::kIcPvl, "ic-pvl"},
    {IcMuI2cTarget::kIcPvlStatus, "ic-pvl-status"},
}};

// The first RAM register of the SPO table, and where the EEPROM keeps it.
constexpr uint8_t kSpoBaseRam = 0x52;
constexpr uint8_t kSpoLastRam = 0x59;
constexpr uint8_t kSpoBaseEeprom = 0x19;
constexpr uint8_t kLastConfigurationRam = 0x18;

std::vector<IcMuI2cChunk> chunks(uint8_t first, uint8_t last, uint8_t size, bool aligned) {
  std::vector<IcMuI2cChunk> result;
  unsigned start = first;
  while (start <= last) {
    unsigned end = aligned ? (start / size + 1) * size - 1 : start + size - 1;
    end = std::min<unsigned>(end, last);
    result.push_back({static_cast<uint8_t>(start), static_cast<uint8_t>(end - start + 1)});
    start = end + 1;
  }
  return result;
}

}  // namespace

std::string_view toString(IcMuI2cTarget target) {
  const auto* found =
      std::ranges::find(kTargetNames, target, &std::pair<IcMuI2cTarget, std::string_view>::first);
  return found != kTargetNames.end() ? found->second : "unknown";
}

std::optional<IcMuI2cTarget> parseIcMuI2cTarget(std::string_view name) {
  const auto* found =
      std::ranges::find(kTargetNames, name, &std::pair<IcMuI2cTarget, std::string_view>::second);
  if (found == kTargetNames.end()) {
    return std::nullopt;
  }
  return found->first;
}

uint8_t icMuI2cDeviceId(IcMuI2cTarget target, bool write) {
  uint8_t writeId = 0;
  switch (target) {
    case IcMuI2cTarget::kEeprom:
      writeId = 0xA0;
      break;
    case IcMuI2cTarget::kIcPvl:
      writeId = 0xC2;
      break;
    case IcMuI2cTarget::kIcPvlStatus:
      writeId = 0xC0;
      break;
  }
  return write ? writeId : static_cast<uint8_t>(writeId | 0x01);
}

std::vector<IcMuRegisterWrite> icMuI2cTransferSetup(uint8_t deviceId, uint8_t deviceStart,
                                                    uint8_t count) {
  return {{kIcMuI2cDeviceIdRegister, deviceId},
          {kIcMuI2cRamStartRegister, kIcMuExchangeRegister},
          {kIcMuI2cDeviceStartRegister, deviceStart},
          {kIcMuI2cRamEndRegister, static_cast<uint8_t>(kIcMuExchangeRegister + count - 1)}};
}

std::optional<uint8_t> icMuEepromAddress(uint8_t ramAddress) {
  if (ramAddress <= kLastConfigurationRam) {
    return ramAddress;
  }
  if (ramAddress >= kSpoBaseRam && ramAddress <= kSpoLastRam) {
    return static_cast<uint8_t>(kSpoBaseEeprom + (ramAddress - kSpoBaseRam));
  }
  return std::nullopt;
}

void to_json(nlohmann::json& j, const IcMuKeyRegister& entry) {
  j = nlohmann::json{{"ramAddress", entry.ramAddress},
                     {"eepromAddress", entry.eepromAddress},
                     {"value", entry.value}};
}

void to_json(nlohmann::json& j, const IcMuEepromByte& entry) {
  j = nlohmann::json{{"address", entry.address}, {"value", entry.value}};
}

std::vector<IcMuI2cChunk> icMuEepromReadChunks(uint8_t first, uint8_t last) {
  return chunks(first, last, kIcMuExchangeBytes, false);
}

std::vector<IcMuI2cChunk> icMuEepromWriteChunks(uint8_t first, uint8_t last) {
  return chunks(first, last, kIcMuEepromWritePage, true);
}

}  // namespace mm::node::somanet
