#include "node/somanet_control.h"

#include <expected>
#include <string>
#include <type_traits>
#include <utility>

namespace mm::node {

namespace {

// The two steps every function here repeats: borrow the device under the bus lock, then bind a
// validated SOMANET view to it. What is left at each call site is the operation itself.
template <typename Fn>
auto withSomanetDrive(DeviceManager& deviceManager, uint16_t slavePosition, Fn&& fn)
    -> std::invoke_result_t<Fn, SomanetDrive&> {
  return deviceManager.withDevice(slavePosition,
                                  [&fn](Device& device) -> std::invoke_result_t<Fn, SomanetDrive&> {
                                    auto drive = createSomanetDrive(device);
                                    if (!drive) {
                                      return std::unexpected(drive.error());
                                    }
                                    return std::forward<Fn>(fn)(*drive);
                                  });
}

}  // namespace

std::expected<BrakeState, std::string> brakeState(DeviceManager& deviceManager,
                                                  uint16_t slavePosition) {
  return withSomanetDrive(deviceManager, slavePosition,
                          [](SomanetDrive& drive) { return drive.brakeState(); });
}

std::expected<BrakeState, std::string> releaseBrake(DeviceManager& deviceManager,
                                                    uint16_t slavePosition,
                                                    std::chrono::milliseconds settle) {
  return withSomanetDrive(deviceManager, slavePosition,
                          [settle](SomanetDrive& drive) { return drive.releaseBrake(settle); });
}

std::expected<BrakeState, std::string> engageBrake(DeviceManager& deviceManager,
                                                   uint16_t slavePosition,
                                                   std::chrono::milliseconds settle) {
  return withSomanetDrive(deviceManager, slavePosition,
                          [settle](SomanetDrive& drive) { return drive.engageBrake(settle); });
}

std::expected<std::vector<DeviceFile>, std::string> readFileList(DeviceManager& deviceManager,
                                                                 uint16_t slavePosition) {
  return withSomanetDrive(deviceManager, slavePosition,
                          [](SomanetDrive& drive) { return drive.readFileList(); });
}

std::expected<HrdRecording, std::string> readHrdRecording(DeviceManager& deviceManager,
                                                          uint16_t slavePosition,
                                                          somanet::HrdData data) {
  return withSomanetDrive(deviceManager, slavePosition,
                          [data](SomanetDrive& drive) { return drive.readHrdRecording(data); });
}

}  // namespace mm::node
