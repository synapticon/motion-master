#include "node/profile_device.h"

#include <format>
#include <string>
#include <vector>

namespace mm::node {

std::expected<uint32_t, std::string> ProfileDevice::deviceType() const {
  return device_.readCachedValue<uint32_t>(kDeviceType, 0);
}

std::expected<uint8_t, std::string> ProfileDevice::errorRegister() const {
  return device_.readValue<uint8_t>(kErrorRegister, 0);
}

std::expected<int32_t, std::string> ProfileDevice::cobIdSync() const {
  return device_.readValue<int32_t>(kCobIdSync, 0);
}

std::expected<void, std::string> ProfileDevice::setCobIdSync(int32_t value) {
  return device_.writeValue(kCobIdSync, 0, value);
}

std::expected<int32_t, std::string> ProfileDevice::communicationCyclePeriod() const {
  return device_.readValue<int32_t>(kCommunicationCyclePeriod, 0);
}

std::expected<void, std::string> ProfileDevice::setCommunicationCyclePeriod(int32_t value) {
  return device_.writeValue(kCommunicationCyclePeriod, 0, value);
}

std::expected<std::string, std::string> ProfileDevice::manufacturerDeviceName() const {
  return device_.readCachedValue<std::string>(kManufacturerDeviceName, 0);
}

std::expected<std::string, std::string> ProfileDevice::manufacturerSoftwareVersion() const {
  return device_.readCachedValue<std::string>(kManufacturerSoftwareVersion, 0);
}

std::expected<uint16_t, std::string> ProfileDevice::guardTime() const {
  return device_.readValue<uint16_t>(kGuardTime, 0);
}

std::expected<void, std::string> ProfileDevice::setGuardTime(uint16_t value) {
  return device_.writeValue(kGuardTime, 0, value);
}

std::expected<uint8_t, std::string> ProfileDevice::lifeTimeFactor() const {
  return device_.readValue<uint8_t>(kLifeTimeFactor, 0);
}

std::expected<void, std::string> ProfileDevice::setLifeTimeFactor(uint8_t value) {
  return device_.writeValue(kLifeTimeFactor, 0, value);
}

std::expected<uint32_t, std::string> ProfileDevice::storeParameters() const {
  return device_.readValue<uint32_t>(kStoreParameters, 1);
}

std::expected<void, std::string> ProfileDevice::setStoreParameters(uint32_t signature) {
  return device_.writeValue(kStoreParameters, 1, signature);
}

std::expected<RestoreDefaultParameters, std::string> ProfileDevice::restoreDefaultParameters()
    const {
  auto all = device_.readValue<uint32_t>(kRestoreDefaultParameters, 1);
  if (!all) {
    return std::unexpected(all.error());
  }
  auto communication = device_.readValue<uint32_t>(kRestoreDefaultParameters, 2);
  if (!communication) {
    return std::unexpected(communication.error());
  }
  auto application = device_.readValue<uint32_t>(kRestoreDefaultParameters, 3);
  if (!application) {
    return std::unexpected(application.error());
  }
  auto manufacturer = device_.readValue<uint32_t>(kRestoreDefaultParameters, 4);
  if (!manufacturer) {
    return std::unexpected(manufacturer.error());
  }
  return RestoreDefaultParameters{*all, *communication, *application, *manufacturer};
}

std::expected<void, std::string> ProfileDevice::setRestoreAllDefaultParameters(uint32_t signature) {
  return device_.writeValue(kRestoreDefaultParameters, 1, signature);
}

std::expected<void, std::string> ProfileDevice::setRestoreCommunicationDefaultParameters(
    uint32_t signature) {
  return device_.writeValue(kRestoreDefaultParameters, 2, signature);
}

std::expected<void, std::string> ProfileDevice::setRestoreApplicationDefaultParameters(
    uint32_t signature) {
  return device_.writeValue(kRestoreDefaultParameters, 3, signature);
}

std::expected<void, std::string> ProfileDevice::setRestoreManufacturerDefaultParameters(
    uint32_t signature) {
  return device_.writeValue(kRestoreDefaultParameters, 4, signature);
}

std::expected<uint32_t, std::string> ProfileDevice::consumerHeartbeatTime() const {
  return device_.readValue<uint32_t>(kConsumerHeartbeatTime, 1);
}

std::expected<void, std::string> ProfileDevice::setConsumerHeartbeatTime(uint32_t value) {
  return device_.writeValue(kConsumerHeartbeatTime, 1, value);
}

std::expected<uint16_t, std::string> ProfileDevice::producerHeartbeatTime() const {
  return device_.readValue<uint16_t>(kProducerHeartbeatTime, 0);
}

std::expected<void, std::string> ProfileDevice::setProducerHeartbeatTime(uint16_t value) {
  return device_.writeValue(kProducerHeartbeatTime, 0, value);
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

std::expected<uint8_t, std::string> ProfileDevice::synchronousCounterOverflowValue() const {
  return device_.readValue<uint8_t>(kSynchronousCounterOverflowValue, 0);
}

std::expected<void, std::string> ProfileDevice::setSynchronousCounterOverflowValue(uint8_t value) {
  return device_.writeValue(kSynchronousCounterOverflowValue, 0, value);
}

std::expected<std::vector<uint8_t>, std::string> ProfileDevice::osCommand() const {
  return device_.readValue<std::vector<uint8_t>>(kOsCommand, 1);
}

std::expected<void, std::string> ProfileDevice::setOsCommand(const std::vector<uint8_t>& command) {
  return device_.writeValue(kOsCommand, 1, command);
}

std::expected<uint8_t, std::string> ProfileDevice::osCommandStatus() const {
  return device_.readValue<uint8_t>(kOsCommand, 2);
}

std::expected<std::vector<uint8_t>, std::string> ProfileDevice::osCommandResponse() const {
  return device_.readValue<std::vector<uint8_t>>(kOsCommand, 3);
}

std::expected<void, std::string> ProfileDevice::setOsCommandMode(uint8_t mode) {
  return device_.writeValue(kOsCommandMode, 0, mode);
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
