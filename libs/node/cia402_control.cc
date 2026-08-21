#include "node/cia402_control.h"

#include <chrono>
#include <expected>
#include <string>
#include <type_traits>
#include <utility>

namespace mm::node {

namespace {

// The two steps every function here repeats: take a handle on the device, then bind a validated
// CiA402 view to it. What is left at each call site is the operation itself. The handle keeps the
// device alive for the call, so a concurrent rescan cannot pull it out from under the view.
template <typename Fn>
auto withDrive(DeviceManager& deviceManager, uint16_t slavePosition, Fn&& fn)
    -> std::invoke_result_t<Fn, Cia402Drive&> {
  const auto device = deviceManager.deviceAt(slavePosition);
  if (!device) {
    return deviceNotFound(slavePosition);
  }
  auto drive = createCia402Drive(*device);
  if (!drive) {
    return std::unexpected(drive.error());
  }
  return std::forward<Fn>(fn)(*drive);
}

}  // namespace

std::expected<Cia402Status, std::string> cia402Status(DeviceManager& deviceManager,
                                                      uint16_t slavePosition) {
  return withDrive(deviceManager, slavePosition,
                   [](Cia402Drive& drive) { return drive.readStatus(); });
}

std::expected<Cia402Status, std::string> setCia402OperationMode(DeviceManager& deviceManager,
                                                                uint16_t slavePosition, int8_t mode,
                                                                std::chrono::milliseconds timeout) {
  return withDrive(deviceManager, slavePosition,
                   [mode, timeout](Cia402Drive& drive) -> std::expected<Cia402Status, std::string> {
                     if (auto r = drive.applyOperationMode(mode, timeout); !r) {
                       return std::unexpected(r.error());
                     }
                     return drive.readStatus();
                   });
}

std::expected<Cia402Status, std::string> runCia402Command(DeviceManager& deviceManager,
                                                          uint16_t slavePosition,
                                                          Cia402Command command,
                                                          std::chrono::milliseconds timeout) {
  return withDrive(
      deviceManager, slavePosition,
      [command, timeout](Cia402Drive& drive) -> std::expected<Cia402Status, std::string> {
        std::expected<void, std::string> r;
        switch (command) {
          case Cia402Command::kEnable:
            r = drive.enable(timeout);
            break;
          case Cia402Command::kDisable:
            r = drive.disable();
            break;
          case Cia402Command::kQuickStop:
            r = drive.quickStop();
            break;
          case Cia402Command::kFaultReset:
            r = drive.faultReset();
            break;
        }
        if (!r) {
          return std::unexpected(r.error());
        }
        return drive.readStatus();
      });
}

std::expected<Cia402Status, std::string> transitionToCia402State(
    DeviceManager& deviceManager, uint16_t slavePosition, cia402::State target,
    std::chrono::milliseconds timeout) {
  return withDrive(
      deviceManager, slavePosition,
      [target, timeout](Cia402Drive& drive) -> std::expected<Cia402Status, std::string> {
        // The override is on: a caller naming Operation Enabled while the drive is in
        // Quick Stop Active is asking to leave that stop.
        if (auto r = drive.transitionToState(target, timeout, true); !r) {
          return std::unexpected(r.error());
        }
        return drive.readStatus();
      });
}

std::expected<void, std::string> setCia402Target(DeviceManager& deviceManager,
                                                 uint16_t slavePosition, Cia402TargetKind kind,
                                                 int32_t setpoint) {
  return withDrive(deviceManager, slavePosition,
                   [kind, setpoint](Cia402Drive& drive) -> std::expected<void, std::string> {
                     switch (kind) {
                       case Cia402TargetKind::kPosition:
                         return drive.setTargetPosition(setpoint);
                       case Cia402TargetKind::kVelocity:
                         return drive.setTargetVelocity(setpoint);
                       case Cia402TargetKind::kTorque:
                         return drive.setTargetTorque(static_cast<int16_t>(setpoint));
                     }
                     return std::unexpected("invalid target kind");
                   });
}

}  // namespace mm::node
