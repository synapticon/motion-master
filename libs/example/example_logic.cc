#include "example/example_logic.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <vector>

#include "node/device.h"
#include "node/device_manager.h"

namespace mm::example {

void to_json(nlohmann::json& j, const DeviceSummary& summary) {
  j = nlohmann::json{
      {"position", summary.position},         {"name", summary.name},
      {"vendorId", summary.vendorId},         {"productCode", summary.productCode},
      {"serialNumber", summary.serialNumber},
  };
}

std::vector<DeviceSummary> summarizeDevices(const mm::node::DeviceManager& deviceManager) {
  const auto& devices = deviceManager.devices();
  std::vector<DeviceSummary> summaries;
  summaries.reserve(devices.size());
  std::transform(devices.begin(), devices.end(), std::back_inserter(summaries),
                 [](const mm::node::Device& device) {
                   return DeviceSummary{
                       .position = device.slavePosition(),
                       .name = device.name(),
                       .vendorId = std::format("0x{:08X}", device.vendorId()),
                       .productCode = std::format("0x{:08X}", device.productCode()),
                       .serialNumber = device.serialNumber(),
                   };
                 });
  return summaries;
}

}  // namespace mm::example
