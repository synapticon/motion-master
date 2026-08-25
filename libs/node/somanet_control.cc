#include "node/somanet_control.h"

#include <expected>
#include <string>
#include <type_traits>
#include <utility>

#include "node/synapticon.h"

namespace mm::node {

namespace {

// The two steps every function here repeats: take a handle on the device, then bind a validated
// SOMANET view to it. What is left at each call site is the operation itself. The handle keeps the
// device alive for the call, so a concurrent rescan cannot pull it out from under the view.
template <typename Fn>
auto withSomanetDrive(DeviceManager& deviceManager, uint16_t slavePosition, Fn&& fn)
    -> std::invoke_result_t<Fn, SomanetDrive&> {
  const auto device = deviceManager.deviceAt(slavePosition);
  if (!device) {
    return deviceNotFound(slavePosition);
  }
  auto drive = createSomanetDrive(*device);
  if (!drive) {
    return std::unexpected(drive.error());
  }
  return std::forward<Fn>(fn)(*drive);
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

std::expected<void, std::string> removeFile(DeviceManager& deviceManager, uint16_t slavePosition,
                                            const std::string& filename) {
  const auto device = deviceManager.deviceAt(slavePosition);
  if (!device) {
    return deviceNotFound(slavePosition);
  }
  // The vendor id, and not a SomanetDrive: see removeDeviceFile on why a profile view would break
  // removal in BOOT. The vendor id comes from SII and is there as soon as the device is.
  if (device->vendorId() != kSynapticonVendorId) {
    return std::unexpected(std::format("device {} is not a SOMANET drive", slavePosition));
  }
  return removeDeviceFile(*device, filename);
}

std::expected<HardwareDescription, std::string> readHardwareDescription(
    DeviceManager& deviceManager, uint16_t slavePosition) {
  return withSomanetDrive(deviceManager, slavePosition,
                          [](SomanetDrive& drive) { return drive.readHardwareDescription(); });
}

std::expected<std::optional<IntegroVariant>, std::string> readIntegroVariant(
    DeviceManager& deviceManager, uint16_t slavePosition) {
  return withSomanetDrive(deviceManager, slavePosition,
                          [](SomanetDrive& drive) { return drive.readIntegroVariant(); });
}

std::expected<FirmwareCompatibility, std::string> checkFirmwarePackage(
    DeviceManager& deviceManager, uint16_t slavePosition, std::string_view packageFilename) {
  return withSomanetDrive(deviceManager, slavePosition, [packageFilename](SomanetDrive& drive) {
    return drive.checkFirmwarePackage(packageFilename);
  });
}

std::expected<HrdRecording, std::string> readHrdRecording(DeviceManager& deviceManager,
                                                          uint16_t slavePosition,
                                                          somanet::HrdData data) {
  return withSomanetDrive(deviceManager, slavePosition,
                          [data](SomanetDrive& drive) { return drive.readHrdRecording(data); });
}

}  // namespace mm::node
