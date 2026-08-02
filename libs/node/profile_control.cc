#include "node/profile_control.h"

#include <expected>
#include <string>
#include <type_traits>
#include <utility>

namespace mm::node {

namespace {

// The two steps every function here repeats: borrow the device under the bus lock, then bind a
// validated generic-profile view to it. What is left at each call site is the operation itself.
template <typename Fn>
auto withProfile(DeviceManager& deviceManager, uint16_t slavePosition, Fn&& fn)
    -> std::invoke_result_t<Fn, ProfileDevice&> {
  return deviceManager.withDevice(
      slavePosition, [&fn](Device& device) -> std::invoke_result_t<Fn, ProfileDevice&> {
        auto profile = createProfileDevice(device);
        if (!profile) {
          return std::unexpected(profile.error());
        }
        return std::forward<Fn>(fn)(*profile);
      });
}

}  // namespace

std::expected<void, std::string> runStoreParameters(DeviceManager& deviceManager,
                                                    uint16_t slavePosition,
                                                    const StoreParametersConfig& config) {
  return withProfile(deviceManager, slavePosition, [&config](ProfileDevice& profile) {
    return profile.runStoreParameters(config);
  });
}

std::expected<void, std::string> runRestoreDefaultParameters(
    DeviceManager& deviceManager, uint16_t slavePosition, RestoreGroup group,
    const RestoreDefaultParametersConfig& config) {
  return withProfile(deviceManager, slavePosition, [group, &config](ProfileDevice& profile) {
    return profile.runRestoreDefaultParameters(group, config);
  });
}

}  // namespace mm::node
