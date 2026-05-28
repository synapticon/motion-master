#include "node/device.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "node/device_parameter.h"

namespace mm::node {

Device::Device(uint16_t slavePosition, mm::comm::FieldbusDriver& driver)
    : slavePosition_(slavePosition), driver_(driver) {
  auto info = driver_.slaveInfo(slavePosition);
  name_ = std::move(info.name);
  vendorId_ = info.vendorId;
  productCode_ = info.productCode;
  revisionNumber_ = info.revisionNumber;
  serialNumber_ = info.serialNumber;
}

uint16_t Device::slavePosition() const { return slavePosition_; }
const std::string& Device::name() const { return name_; }
uint32_t Device::vendorId() const { return vendorId_; }
uint32_t Device::productCode() const { return productCode_; }
uint32_t Device::revisionNumber() const { return revisionNumber_; }
uint32_t Device::serialNumber() const { return serialNumber_; }

std::expected<std::vector<uint8_t>, std::string> Device::upload(uint16_t index,
                                                                uint8_t subindex) const {
  return driver_.readSdo(slavePosition_, index, subindex);
}

std::expected<std::vector<uint8_t>, std::string> Device::readFile(
    const std::string& filename) const {
  return driver_.readFile(slavePosition_, filename);
}

std::expected<void, std::string> Device::readRegister(uint16_t address,
                                                      std::span<uint8_t> data) const {
  return driver_.readRegister(slavePosition_, address, data);
}

std::expected<void, std::string> Device::writeRegister(uint16_t address,
                                                       std::span<const uint8_t> data) const {
  return driver_.writeRegister(slavePosition_, address, data);
}

std::expected<void, std::string> Device::initializeParameters(bool readValues) {
  auto entries = driver_.readObjectDictionary(slavePosition_);
  if (!entries) {
    return std::unexpected(entries.error());
  }

  parameters_.clear();
  parameters_.reserve(entries->size());

  for (const auto& e : *entries) {
    DeviceParameter p{
        .index = e.index,
        .subindex = e.subindex,
        .name = e.name,
        .objectCode = e.objectCode,
        .dataType = e.dataType,
        .bitLength = e.bitLength,
        .access = e.access,
        .value = defaultValueForDataType(e.dataType),
        .unit = e.unit,
        .defaultValue = std::nullopt,
        .minValue = std::nullopt,
        .maxValue = std::nullopt,
    };

    auto decodeRawBytes =
        [&](const std::optional<std::vector<uint8_t>>& raw) -> std::optional<DeviceParameterValue> {
      if (!raw) {
        return std::nullopt;
      }
      auto decoded = decodeSdoBytes(e.dataType, *raw);
      if (!decoded) {
        spdlog::warn("Device {}: decode 0x{:04X}:{:02X} bound failed: {}", slavePosition_, e.index,
                     e.subindex, decoded.error());
        return std::nullopt;
      }
      return std::move(*decoded);
    };
    p.defaultValue = decodeRawBytes(e.defaultValue);
    p.minValue = decodeRawBytes(e.minValue);
    p.maxValue = decodeRawBytes(e.maxValue);

    if (readValues) {
      auto bytes = driver_.readSdo(slavePosition_, e.index, e.subindex);
      if (bytes) {
        auto decoded = decodeSdoBytes(e.dataType, *bytes);
        if (decoded) {
          p.value = std::move(*decoded);
        } else {
          spdlog::warn("Device {}: decode 0x{:04X}:{:02X} failed: {}", slavePosition_, e.index,
                       e.subindex, decoded.error());
        }
      } else {
        spdlog::warn("Device {}: SDO upload 0x{:04X}:{:02X} failed: {}", slavePosition_, e.index,
                     e.subindex, bytes.error());
      }
    }

    parameters_.emplace(p.key(), std::move(p));
  }

  return {};
}

const std::unordered_map<uint32_t, DeviceParameter>& Device::parameters() const {
  return parameters_;
}

std::vector<DeviceParameter> Device::parametersOrdered() const {
  std::vector<DeviceParameter> ordered;
  ordered.reserve(parameters_.size());
  for (const auto& [key, p] : parameters_) {
    ordered.push_back(p);
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const DeviceParameter& a, const DeviceParameter& b) { return a.key() < b.key(); });
  return ordered;
}

const DeviceParameter* Device::parameter(uint16_t index, uint8_t subindex) const {
  auto it = parameters_.find(makeParameterKey(index, subindex));
  return it != parameters_.end() ? &it->second : nullptr;
}

void to_json(nlohmann::json& j, const Device& d) {
  j = nlohmann::json{
      {"slavePosition", d.slavePosition()},
      {"name", d.name()},
      {"vendorId", d.vendorId()},
      {"productCode", d.productCode()},
      {"revisionNumber", d.revisionNumber()},
      {"serialNumber", d.serialNumber()},
  };
}

}  // namespace mm::node
