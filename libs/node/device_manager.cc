#include "node/device_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace mm::node {

std::expected<void, std::string> DeviceManager::init(
    std::unique_ptr<mm::comm::FieldbusDriver> driver) {
  driver_ = std::move(driver);
  auto result = driver_->init();
  if (!result) {
    spdlog::error("FieldbusDriver init failed: {}", result.error());
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

void DeviceManager::pdoExchange() {
  if (driver_) {
    driver_->exchangeProcessData();
  }
}

std::expected<void, std::string> DeviceManager::transitionToState(
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
  driver_->transitionToState(targets, std::nullopt, targetState, timeout);
  return {};
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
    const auto& raw_state = (*raw)[i];
    result.push_back({
        .slavePosition = targets[i],
        .alStatus = raw_state.alStatus,
        .alState = static_cast<uint16_t>(raw_state.alStatus & 0x000Fu),
        .error = !!(raw_state.alStatus & 0x0010u),
        .alStatusCode = raw_state.alStatusCode,
    });
  }
  return result;
}

void to_json(nlohmann::json& j, const DeviceManager& dm) { j = dm.devices(); }

}  // namespace mm::node
