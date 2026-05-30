#include "node/device_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace mm::node {

namespace {

/// Decodes a raw AL Status read-back into the API-facing DeviceStateInfo:
/// bits 3:0 are the state, bit 4 is the error indicator.
DeviceStateInfo decodeState(uint16_t slavePosition,
                            const mm::comm::FieldbusDriver::SlaveStateRaw& raw) {
  return {
      .slavePosition = slavePosition,
      .alStatus = raw.alStatus,
      .alState = static_cast<uint16_t>(raw.alStatus & 0x000Fu),
      .error = !!(raw.alStatus & 0x0010u),
      .alStatusCode = raw.alStatusCode,
  };
}

}  // namespace

std::expected<void, std::string> DeviceManager::init(
    std::unique_ptr<mm::comm::FieldbusDriver> driver) {
  // init() is a one-shot: replacing a live driver would destroy it while the
  // Devices in devices_ still hold a FieldbusDriver& to it, leaving every Device
  // with a dangling reference. Require an explicit reset() between inits instead.
  if (driver_) {
    return std::unexpected("already initialised — call reset() before init()");
  }
  driver_ = std::move(driver);
  auto result = driver_->init();
  if (!result) {
    // A failed init must leave us uninitialised — not holding a driver whose
    // context never opened. Otherwise initialised() would report true and the
    // next scan()/SDO call would dereference a null context. Dropping it here
    // also lets the caller simply retry init() without an intervening reset().
    spdlog::error("FieldbusDriver init failed: {}", result.error());
    driver_.reset();
  } else {
    spdlog::debug("FieldbusDriver initialised");
  }
  return result;
}

std::expected<int, std::string> DeviceManager::scan() {
  if (!driver_) {
    spdlog::error("scan() called with no driver — call init() first");
    return std::unexpected("no driver — call init() first");
  }
  auto result = driver_->scan();
  if (!result) {
    spdlog::error("FieldbusDriver scan failed: {}", result.error());
    return std::unexpected(result.error());
  }
  devices_.clear();
  for (uint16_t pos = 1; pos <= static_cast<uint16_t>(*result); ++pos) {
    devices_.emplace_back(pos, *driver_);
  }
  spdlog::info("Found {} slave(s)", *result);
  for (const auto& device : devices_) {
    spdlog::info("  [{:2}] {} — vendor: {:#010x}  product: {:#010x}  rev: {:#010x}  serial: {}",
                 device.slavePosition(), device.name(), device.vendorId(), device.productCode(),
                 device.revisionNumber(), device.serialNumber());
  }
  return *result;
}

void DeviceManager::reset() {
  devices_.clear();  // drop device references to driver before stopping
  if (driver_) {
    driver_->stop();
    driver_.reset();
    spdlog::info("DeviceManager reset");
  }
}

const std::vector<Device>& DeviceManager::devices() const { return devices_; }

const Device* DeviceManager::findDevice(uint16_t slavePosition) const {
  auto it = std::find_if(devices_.begin(), devices_.end(), [slavePosition](const Device& d) {
    return d.slavePosition() == slavePosition;
  });
  return it != devices_.end() ? &*it : nullptr;
}

Device* DeviceManager::findDevice(uint16_t slavePosition) {
  auto it = std::find_if(devices_.begin(), devices_.end(), [slavePosition](const Device& d) {
    return d.slavePosition() == slavePosition;
  });
  return it != devices_.end() ? &*it : nullptr;
}

void DeviceManager::updateOnline(const DeviceStateInfo& info) {
  Device* device = findDevice(info.slavePosition);
  if (!device) {
    return;
  }
  using mm::comm::EtherCatState;
  const bool mailbox =
      !info.error && (info.alState == static_cast<uint16_t>(EtherCatState::PreOp) ||
                      info.alState == static_cast<uint16_t>(EtherCatState::SafeOp) ||
                      info.alState == static_cast<uint16_t>(EtherCatState::Op));
  device->setOnline(mailbox);
}

void DeviceManager::pdoExchange() {
  if (driver_) {
    driver_->exchangeProcessData();
  }
}

std::expected<std::vector<DeviceStateInfo>, std::string> DeviceManager::transitionToState(
    const std::vector<uint16_t>& positions, mm::comm::EtherCatState targetState,
    std::chrono::steady_clock::duration timeout) {
  if (!driver_) {
    return std::unexpected("no driver — call init() first");
  }
  if (devices_.empty()) {
    return std::unexpected("no devices — call scan() first");
  }
  std::vector<uint16_t> targets = positions;
  if (targets.empty()) {
    targets.reserve(devices_.size());
    std::transform(devices_.begin(), devices_.end(), std::back_inserter(targets),
                   [](const Device& d) { return d.slavePosition(); });
  }
  spdlog::debug("transitionToState -> 0x{:02X} for {} device(s)", static_cast<int>(targetState),
                targets.size());
  driver_->transitionToState(targets, std::nullopt, targetState, timeout);

  // The driver call only logs failures, so read the settled state back and return it.
  // Callers derive "reached the target" as (!error && alState == targetState) per device.
  auto raw = driver_->readStates(targets);
  if (!raw) {
    return std::unexpected(raw.error());
  }
  std::vector<DeviceStateInfo> result;
  result.reserve(targets.size());
  for (std::size_t i = 0; i < targets.size(); ++i) {
    result.push_back(decodeState(targets[i], (*raw)[i]));
    updateOnline(result.back());
  }

  // Reconcile module idents once a device reaches PRE-OP. A modular drive whose
  // Configured Module Ident List (0xF030) disagrees with its Detected list (0xF050)
  // reports a mismatch and refuses to leave PRE-OP; copying detected into configured
  // clears it. SDO mailbox is only available in PRE-OP+, so this is the earliest point
  // the write is possible. Best-effort: failures are logged, never fatal to the transition.
  if (targetState == mm::comm::EtherCatState::PreOp) {
    for (const auto& info : result) {
      if (info.error || info.alState != static_cast<uint16_t>(mm::comm::EtherCatState::PreOp)) {
        continue;
      }
      const Device* device = findDevice(info.slavePosition);
      if (!device) {
        continue;
      }
      auto reconciled = reconcileDetectedModules(*device);
      if (!reconciled) {
        spdlog::warn("Device {}: module ident reconcile failed: {}", info.slavePosition,
                     reconciled.error());
      } else if (*reconciled > 0) {
        spdlog::info("Device {}: reconciled {} module slot(s)", info.slavePosition, *reconciled);
      }
    }
  }

  return result;
}

void to_json(nlohmann::json& j, const DeviceStateInfo& info) {
  j = {{"slavePosition", info.slavePosition},
       {"alStatus", info.alStatus},
       {"alState", info.alState},
       {"error", info.error},
       {"alStatusCode", info.alStatusCode}};
}

std::expected<std::vector<DeviceStateInfo>, std::string> DeviceManager::getDeviceStates(
    const std::vector<uint16_t>& positions) {
  if (!driver_) {
    return std::unexpected("no driver — call init() first");
  }
  std::vector<uint16_t> targets = positions;
  if (targets.empty()) {
    targets.reserve(devices_.size());
    std::transform(devices_.begin(), devices_.end(), std::back_inserter(targets),
                   [](const Device& d) { return d.slavePosition(); });
  }
  auto raw = driver_->readStates(targets);
  if (!raw) {
    return std::unexpected(raw.error());
  }
  std::vector<DeviceStateInfo> result;
  result.reserve(targets.size());
  for (std::size_t i = 0; i < targets.size(); ++i) {
    result.push_back(decodeState(targets[i], (*raw)[i]));
    updateOnline(result.back());
  }
  return result;
}

std::expected<bool, std::string> DeviceManager::isDeviceOnline(uint16_t slavePosition) {
  if (!driver_) {
    return std::unexpected("no driver — call init() first");
  }
  const Device* device = findDevice(slavePosition);
  if (!device) {
    return std::unexpected("device " + std::to_string(slavePosition) + " not found");
  }
  // Reading the single position keeps the cached online flag in sync (via updateOnline)
  // and reuses the one place that defines "online", rather than duplicating the rule here.
  if (auto states = getDeviceStates({slavePosition}); !states) {
    return std::unexpected(states.error());
  }
  return device->online();
}

std::expected<void, std::string> DeviceManager::initializeDeviceParameters(uint16_t slavePosition,
                                                                           bool readValues) {
  auto it = std::find_if(devices_.begin(), devices_.end(), [slavePosition](const Device& d) {
    return d.slavePosition() == slavePosition;
  });
  if (it == devices_.end()) {
    return std::unexpected("device " + std::to_string(slavePosition) + " not found");
  }
  return it->initializeParameters(readValues);
}

std::expected<DeviceParameterValue, std::string> DeviceManager::readDeviceParameter(
    uint16_t slavePosition, uint16_t index, uint8_t subindex) {
  Device* device = findDevice(slavePosition);
  if (!device) {
    return std::unexpected("device " + std::to_string(slavePosition) + " not found");
  }
  return device->readParameter(index, subindex);
}

std::expected<void, std::string> DeviceManager::writeDeviceParameter(uint16_t slavePosition,
                                                                     uint16_t index,
                                                                     uint8_t subindex,
                                                                     DeviceParameterValue value) {
  Device* device = findDevice(slavePosition);
  if (!device) {
    return std::unexpected("device " + std::to_string(slavePosition) + " not found");
  }
  return device->writeParameter(index, subindex, std::move(value));
}

void to_json(nlohmann::json& j, const DeviceManager& dm) { j = dm.devices(); }

}  // namespace mm::node
