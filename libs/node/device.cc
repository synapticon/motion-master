#include "node/device.h"

#include <nlohmann/json.hpp>
#include <string>
#include <utility>

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

std::expected<void, std::string> Device::readRegister(uint16_t address, std::span<uint8_t> data) {
  return driver_.readRegister(slavePosition_, address, data);
}

std::expected<void, std::string> Device::writeRegister(uint16_t address,
                                                        std::span<const uint8_t> data) {
  return driver_.writeRegister(slavePosition_, address, data);
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
