#include "node/profile_device.h"

#include <chrono>
#include <format>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "node/profile_device_objects.h"

namespace mm::node {

namespace {

// ASCII "save" (little-endian byte order 73 61 76 65) — the value 0x1010:01 accepts to trigger a
// store; the device aborts any other write.
constexpr uint32_t kStoreParametersSignature = 0x65766173;

// ASCII "load" (little-endian byte order 6C 6F 61 64) — the value 0x1011:0x accepts to trigger a
// restore of that group's defaults; the device aborts any other write.
constexpr uint32_t kRestoreParametersSignature = 0x64616F6C;

// The save/restore command objects (0x1010:0x / 0x1011:0x) read back 1 once the command has
// completed (CiA301: bit 0 clears while the device is busy, the sub-entry reading 1 signals
// "done").
constexpr uint32_t kCommandComplete = 1;

// Runs a CANopen "write a signature, then poll until it reads back 1" command — the shared shape of
// store parameters (0x1010) and restore default parameters (0x1011). Writes @p signature to
// @p index : @p subindex, waits @p settle for the device to begin, then polls that same sub-entry
// until it reads back @c kCommandComplete, retrying a poll that isn't done yet — a value mismatch
// or a transient mailbox read error while the device is busy, both handled alike — up to @p retries
// times, @p interval apart. @p label names the operation for the failure message.
//
// @p stop abandons the *wait*, which is all it can abandon: the signature is already on the wire
// and the device is already acting on it. It is checked between sleeps, so a cancel lands within
// one settle or interval rather than at the end of the whole budget.
std::expected<void, std::string> runSignatureConfirmCommand(
    Device& device, uint16_t index, uint8_t subindex, uint32_t signature, uint32_t retries,
    std::chrono::milliseconds interval, std::chrono::milliseconds settle,
    const std::stop_token& stop, std::string_view label) {
  const auto cancelled = [label]() {
    return std::unexpected(std::format(
        "{} was cancelled while waiting for the device to confirm it — the command was already "
        "written, so the device may still complete it",
        label));
  };

  if (auto r = device.writeValue(index, subindex, signature); !r) {
    return r;
  }
  // Give the device time to begin before the first read; the sub-entry reads back its pre-command
  // value until the command actually completes.
  std::this_thread::sleep_for(settle);
  std::string lastError;
  for (uint32_t attempt = 0;; ++attempt) {
    if (stop.stop_requested()) {
      return cancelled();
    }
    if (auto value = device.readValue<uint32_t>(index, subindex)) {
      if (*value == kCommandComplete) {
        return {};
      }
      lastError =
          std::format("0x{:04X}:{:02X} read back 0x{:08X}, expected 1", index, subindex, *value);
    } else {
      lastError = value.error();
    }
    if (attempt >= retries) {
      return std::unexpected(
          std::format("{} not confirmed after {} attempt(s): {}", label, retries + 1, lastError));
    }
    std::this_thread::sleep_for(interval);
  }
}

}  // namespace

std::expected<uint32_t, std::string> ProfileDevice::deviceType() const {
  return device_.readCachedValue(profile::objects::kDeviceType);
}

std::expected<uint8_t, std::string> ProfileDevice::errorRegister() const {
  return device_.readValue(profile::objects::kErrorRegister);
}

std::expected<int32_t, std::string> ProfileDevice::cobIdSync() const {
  return device_.readValue(profile::objects::kCOBIDSYNC);
}

std::expected<void, std::string> ProfileDevice::setCobIdSync(int32_t value) {
  return device_.writeValue(profile::objects::kCOBIDSYNC, value);
}

std::expected<int32_t, std::string> ProfileDevice::communicationCyclePeriod() const {
  return device_.readValue(profile::objects::kCommunicationCyclePeriod);
}

std::expected<void, std::string> ProfileDevice::setCommunicationCyclePeriod(int32_t value) {
  return device_.writeValue(profile::objects::kCommunicationCyclePeriod, value);
}

std::expected<std::string, std::string> ProfileDevice::manufacturerDeviceName() const {
  return device_.readCachedValue(profile::objects::kManufacturerDeviceName);
}

std::expected<std::string, std::string> ProfileDevice::manufacturerSoftwareVersion() const {
  return device_.readCachedValue(profile::objects::kManufacturerSoftwareVersion);
}

std::expected<uint16_t, std::string> ProfileDevice::guardTime() const {
  return device_.readValue(profile::objects::kGuardTime);
}

std::expected<void, std::string> ProfileDevice::setGuardTime(uint16_t value) {
  return device_.writeValue(profile::objects::kGuardTime, value);
}

std::expected<uint8_t, std::string> ProfileDevice::lifeTimeFactor() const {
  return device_.readValue(profile::objects::kLifeTimeFactor);
}

std::expected<void, std::string> ProfileDevice::setLifeTimeFactor(uint8_t value) {
  return device_.writeValue(profile::objects::kLifeTimeFactor, value);
}

std::expected<uint32_t, std::string> ProfileDevice::storeParameters() const {
  return device_.readValue(profile::objects::kStoreParametersSubIndex001);
}

std::expected<void, std::string> ProfileDevice::setStoreParameters(uint32_t signature) {
  return device_.writeValue(profile::objects::kStoreParametersSubIndex001, signature);
}

std::expected<void, std::string> ProfileDevice::runStoreParameters(
    const StoreParametersConfig& config) {
  return runSignatureConfirmCommand(device_, profile::objects::kStoreParametersSubIndex001.index,
                                    profile::objects::kStoreParametersSubIndex001.subindex,
                                    kStoreParametersSignature, config.retries, config.interval,
                                    config.settle, config.stop, "store parameters");
}

std::expected<RestoreDefaultParameters, std::string> ProfileDevice::restoreDefaultParameters()
    const {
  auto object = device_.readObject(profile::objects::kRestoreDefaultParameters.index);
  if (!object) {
    return std::unexpected(object.error());
  }
  auto all = object->get<uint32_t>(1);
  if (!all) {
    return std::unexpected(all.error());
  }
  auto communication = object->get<uint32_t>(2);
  if (!communication) {
    return std::unexpected(communication.error());
  }
  auto application = object->get<uint32_t>(3);
  if (!application) {
    return std::unexpected(application.error());
  }
  auto manufacturer = object->get<uint32_t>(4);
  if (!manufacturer) {
    return std::unexpected(manufacturer.error());
  }
  return RestoreDefaultParameters{*all, *communication, *application, *manufacturer};
}

std::expected<void, std::string> ProfileDevice::setRestoreAllDefaultParameters(uint32_t signature) {
  return device_.writeValue(profile::objects::kRestoreDefaultParametersSubIndex001, signature);
}

std::expected<void, std::string> ProfileDevice::setRestoreCommunicationDefaultParameters(
    uint32_t signature) {
  return device_.writeValue(profile::objects::kRestoreDefaultParametersSubIndex002, signature);
}

std::expected<void, std::string> ProfileDevice::setRestoreApplicationDefaultParameters(
    uint32_t signature) {
  return device_.writeValue(profile::objects::kRestoreDefaultParametersSubIndex003, signature);
}

std::expected<void, std::string> ProfileDevice::setRestoreManufacturerDefaultParameters(
    uint32_t signature) {
  return device_.writeValue(profile::objects::kRestoreDefaultParametersSubIndex004, signature);
}

std::expected<void, std::string> ProfileDevice::runRestoreDefaultParameters(
    RestoreGroup group, const RestoreDefaultParametersConfig& config) {
  // The group enum value *is* the 0x1011 sub-entry (kAll=1 … kManufacturer=4).
  return runSignatureConfirmCommand(device_, profile::objects::kRestoreDefaultParameters.index,
                                    static_cast<uint8_t>(group), kRestoreParametersSignature,
                                    config.retries, config.interval, config.settle, config.stop,
                                    "restore default parameters");
}

std::optional<RestoreGroup> parseRestoreGroup(std::string_view token) {
  if (token == "all") {
    return RestoreGroup::kAll;
  }
  if (token == "communication") {
    return RestoreGroup::kCommunication;
  }
  if (token == "application") {
    return RestoreGroup::kApplication;
  }
  if (token == "manufacturer") {
    return RestoreGroup::kManufacturer;
  }
  return std::nullopt;
}

std::expected<int32_t, std::string> ProfileDevice::consumerHeartbeatTime() const {
  return device_.readValue(profile::objects::kConsumerHeartbeatTimeSubIndex001);
}

std::expected<void, std::string> ProfileDevice::setConsumerHeartbeatTime(int32_t value) {
  return device_.writeValue(profile::objects::kConsumerHeartbeatTimeSubIndex001, value);
}

std::expected<uint16_t, std::string> ProfileDevice::producerHeartbeatTime() const {
  return device_.readValue(profile::objects::kProducerHeartbeatTime);
}

std::expected<void, std::string> ProfileDevice::setProducerHeartbeatTime(uint16_t value) {
  return device_.writeValue(profile::objects::kProducerHeartbeatTime, value);
}

std::expected<Identity, std::string> ProfileDevice::identity() const {
  auto vendorId = device_.readCachedValue(profile::objects::kIdentityObjectVendorID);
  if (!vendorId) {
    return std::unexpected(vendorId.error());
  }
  auto productCode = device_.readCachedValue(profile::objects::kIdentityObjectProductCode);
  if (!productCode) {
    return std::unexpected(productCode.error());
  }
  auto revisionNumber = device_.readCachedValue(profile::objects::kIdentityObjectRevisionNumber);
  if (!revisionNumber) {
    return std::unexpected(revisionNumber.error());
  }
  auto serialNumber = device_.readCachedValue(profile::objects::kIdentityObjectSerialNumber);
  if (!serialNumber) {
    return std::unexpected(serialNumber.error());
  }
  return Identity{*vendorId, *productCode, *revisionNumber, *serialNumber};
}

std::expected<uint8_t, std::string> ProfileDevice::synchronousCounterOverflowValue() const {
  return device_.readValue(profile::objects::kSynchronousCounterOverflowValue);
}

std::expected<void, std::string> ProfileDevice::setSynchronousCounterOverflowValue(uint8_t value) {
  return device_.writeValue(profile::objects::kSynchronousCounterOverflowValue, value);
}

std::expected<std::vector<uint8_t>, std::string> ProfileDevice::osCommand() const {
  return device_.readValue<std::vector<uint8_t>>(profile::objects::kOSCommandCommand.index,
                                                 profile::objects::kOSCommandCommand.subindex);
}

std::expected<void, std::string> ProfileDevice::setOsCommand(const std::vector<uint8_t>& command) {
  return device_.writeValue(profile::objects::kOSCommandCommand, command);
}

std::expected<uint8_t, std::string> ProfileDevice::osCommandStatus() const {
  return device_.readValue(profile::objects::kOSCommandStatus);
}

std::expected<std::vector<uint8_t>, std::string> ProfileDevice::osCommandResponse() const {
  return device_.readValue<std::vector<uint8_t>>(profile::objects::kOSCommandResponse.index,
                                                 profile::objects::kOSCommandResponse.subindex);
}

std::expected<void, std::string> ProfileDevice::setOsCommandMode(uint8_t mode) {
  return device_.writeValue(profile::objects::kOSCommandMode, mode);
}

std::expected<ProfileDevice, std::string> createProfileDevice(Device& device) {
  // Device type (0x1000) is a mandatory CANopen object on any CoE device, so its presence in the
  // enumerated parameter map discriminates an un-enumerated device. No bus I/O — works online or
  // off.
  if (!device.parameter(profile::objects::kDeviceType)) {
    return std::unexpected(std::format(
        "device {} has no generic device area (missing 0x1000; initializeParameters first?)",
        device.slavePosition()));
  }
  return ProfileDevice(device);
}

}  // namespace mm::node
