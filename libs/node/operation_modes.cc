#include "node/operation_modes.h"

#include <nlohmann/json.hpp>
#include <string>
#include <utility>

#include "node/cia402.h"
#include "node/cia402_drive.h"
#include "node/somanet_drive.h"
#include "node/synapticon.h"

namespace mm::node {

namespace {

const char* toString(OperationModeKind kind) {
  switch (kind) {
    case OperationModeKind::kStandard:
      return "standard";
    case OperationModeKind::kManufacturer:
      return "manufacturer";
  }
  return "unknown";
}

}  // namespace

void to_json(nlohmann::json& j, const OperationModes& modes) {
  auto rows = nlohmann::json::array();
  for (const auto& mode : modes.modes) {
    nlohmann::json row{
        {"value", mode.value},
        {"name", mode.name},
        {"label", mode.label},
        {"kind", toString(mode.kind)},
        // Null rather than omitted, both of them: a client rendering a table needs the key to
        // exist, and "the drive does not say" is a value — see OperationModeInfo::supported.
        {"bit", mode.bit ? nlohmann::json(*mode.bit) : nlohmann::json(nullptr)},
        {"abbreviation",
         mode.abbreviation.empty() ? nlohmann::json(nullptr) : nlohmann::json(mode.abbreviation)},
        {"supported", mode.supported ? nlohmann::json(*mode.supported) : nlohmann::json(nullptr)},
    };
    if (mode.deprecated) {
      row["deprecated"] = true;
    }
    rows.push_back(std::move(row));
  }
  j = nlohmann::json{
      {"supportedDriveModes", modes.supportedDriveModes},
      {"manufacturerBits", modes.manufacturerBits},
      {"modes", std::move(rows)},
  };
}

std::expected<OperationModes, std::string> deviceOperationModes(Device& device) {
  auto drive = createCia402Drive(device);
  if (!drive) {
    return std::unexpected(drive.error());
  }
  auto supported = drive->supportedDriveModes();
  if (!supported) {
    return std::unexpected(supported.error());
  }

  OperationModes result;
  result.supportedDriveModes = *supported;
  for (int bit = cia402::kFirstManufacturerDriveModeBit; bit < 32; ++bit) {
    if ((*supported & (uint32_t{1} << bit)) != 0) {
      result.manufacturerBits.push_back(bit);
    }
  }

  // Manufacturer modes first, so the list comes out ascending by value without a sort — SOMANET's
  // are all negative and already in order.
  if (device.vendorId() == kSynapticonVendorId) {
    for (const auto mode : somanet::kOperationModes) {
      result.modes.push_back(OperationModeInfo{
          .value = static_cast<int>(mode),
          .name = std::string(somanet::toString(mode)),
          .label = std::string(somanet::describe(mode)),
          .kind = OperationModeKind::kManufacturer,
          .bit = std::nullopt,
          .abbreviation = {},
          .supported = std::nullopt,
          .deprecated = somanet::isDeprecated(mode),
      });
    }
  }

  for (const auto& standard : cia402::kStandardOperationModes) {
    // NoMode has no capability bit and needs none: the profile makes it always legal, so reporting
    // it as unsupported (which a missing bit would) would be wrong about the one mode every drive
    // accepts.
    const bool hasBit = standard.bit >= 0;
    result.modes.push_back(OperationModeInfo{
        .value = static_cast<int>(standard.mode),
        .name = std::string(cia402::toString(standard.mode)),
        .label = std::string(standard.label),
        .kind = OperationModeKind::kStandard,
        .bit = hasBit ? std::optional<int>(standard.bit) : std::nullopt,
        .abbreviation = std::string(standard.abbreviation),
        .supported = hasBit ? ((*supported & (uint32_t{1} << standard.bit)) != 0) : true,
        .deprecated = false,
    });
  }

  return result;
}

std::expected<OperationModes, std::string> operationModes(DeviceManager& deviceManager,
                                                          uint16_t slavePosition) {
  const auto device = deviceManager.deviceAt(slavePosition);
  if (!device) {
    return deviceNotFound(slavePosition);
  }
  return deviceOperationModes(*device);
}

}  // namespace mm::node
