#include "node/profile_device.h"

#include <format>
#include <string>

namespace mm::node {

std::expected<uint32_t, std::string> ProfileDevice::deviceType() const {
  return device_.readCachedValue<uint32_t>(kDeviceType, 0);
}

std::expected<std::string, std::string> ProfileDevice::manufacturerDeviceName() const {
  return device_.readCachedValue<std::string>(kManufacturerDeviceName, 0);
}

std::expected<std::string, std::string> ProfileDevice::manufacturerSoftwareVersion() const {
  return device_.readCachedValue<std::string>(kManufacturerSoftwareVersion, 0);
}

std::expected<Identity, std::string> ProfileDevice::identity() const {
  auto vendorId = device_.readCachedValue<uint32_t>(kIdentity, 1);
  if (!vendorId) {
    return std::unexpected(vendorId.error());
  }
  auto productCode = device_.readCachedValue<uint32_t>(kIdentity, 2);
  if (!productCode) {
    return std::unexpected(productCode.error());
  }
  auto revisionNumber = device_.readCachedValue<uint32_t>(kIdentity, 3);
  if (!revisionNumber) {
    return std::unexpected(revisionNumber.error());
  }
  auto serialNumber = device_.readCachedValue<uint32_t>(kIdentity, 4);
  if (!serialNumber) {
    return std::unexpected(serialNumber.error());
  }
  return Identity{*vendorId, *productCode, *revisionNumber, *serialNumber};
}

std::expected<ProfileDevice, std::string> createProfileDevice(Device& device) {
  // Device type (0x1000) is a mandatory CANopen object on any CoE device, so its presence in the
  // enumerated parameter map discriminates an un-enumerated device. No bus I/O — works online or
  // off.
  if (device.parameter(kDeviceType, 0) == nullptr) {
    return std::unexpected(std::format(
        "device {} has no generic device area (missing 0x1000; initializeParameters first?)",
        device.slavePosition()));
  }
  return ProfileDevice(device);
}

}  // namespace mm::node
