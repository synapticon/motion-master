#include "node/profile_device.h"

#include <chrono>
#include <format>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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
// @p stop abandons the *wait*, which is all it can abandon: the signature has already been written
// and the device is already acting on it. It is checked between sleeps, so a cancel lands within
// one settle or interval rather than at the end of the whole budget.
std::expected<void, std::string> runSignatureConfirmCommand(
    Device& device, uint16_t index, uint8_t subindex, uint32_t signature, uint32_t retries,
    std::chrono::milliseconds interval, std::chrono::milliseconds settle, std::stop_token stop,
    std::string_view label) {
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

std::expected<void, std::string> ProfileDevice::runStoreParameters(
    const StoreParametersConfig& config) {
  return runSignatureConfirmCommand(device_, kStoreParameters, 1, kStoreParametersSignature,
                                    config.retries, config.interval, config.settle, config.stop,
                                    "store parameters");
}

std::expected<RestoreDefaultParameters, std::string> ProfileDevice::restoreDefaultParameters()
    const {
  auto object = device_.readObject(kRestoreDefaultParameters);
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

std::expected<void, std::string> ProfileDevice::runRestoreDefaultParameters(
    RestoreGroup group, const RestoreDefaultParametersConfig& config) {
  // The group enum value *is* the 0x1011 sub-entry (kAll=1 … kManufacturer=4).
  return runSignatureConfirmCommand(device_, kRestoreDefaultParameters, static_cast<uint8_t>(group),
                                    kRestoreParametersSignature, config.retries, config.interval,
                                    config.settle, config.stop, "restore default parameters");
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
