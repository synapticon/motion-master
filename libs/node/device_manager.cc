#include "node/device_manager.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace mm::node {

DeviceManager::DeviceManager(mm::comm::FieldbusDriver& driver) : driver_(driver) {}

std::expected<void, std::string> DeviceManager::init() { return driver_.init(); }

std::expected<int, std::string> DeviceManager::configure() {
  auto result = driver_.configure();
  if (!result) {
    return std::unexpected(result.error());
  }
  devices_.clear();
  for (uint16_t pos = 1; pos <= static_cast<uint16_t>(*result); ++pos) {
    devices_.emplace_back(pos, driver_);
  }
  return *result;
}

const std::vector<Device>& DeviceManager::devices() const { return devices_; }

void DeviceManager::pdoExchange() { driver_.exchangeProcessData(); }

void to_json(nlohmann::json& j, const DeviceManager& dm) { j = dm.devices(); }

}  // namespace mm::node
