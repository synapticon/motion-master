#include "example/example_cyclic_task.h"

#include <cstdint>

#include "node/cia402.h"
#include "node/device.h"

namespace mm::example {

using mm::node::Device;
using mm::node::DeviceManager;
namespace cia402 = mm::node::cia402;

ExampleCyclicTask::ExampleCyclicTask(DeviceManager& deviceManager, Config config)
    : deviceManager_(deviceManager), config_(config) {}

void ExampleCyclicTask::execute(const CycleContext& /*ctx*/) {
  // Rule 1 — hold the device set still for this cycle. Falsy means the bus is not activated (no
  // image published) or is being reconfigured; either way there is nothing to drive.
  const DeviceManager::CycleLock cycle(deviceManager_);
  if (!cycle) {
    return;
  }

  // Rule 2 and 3 — resolve every cycle, and treat absence as "not this cycle" rather than an error.
  Device* drive = deviceManager_.findDevice(config_.slavePosition);
  if (drive == nullptr) {
    return;
  }

  // Rule 4 — read through the cell. This is the live statusword: the RT exchange decoded it into
  // the cell earlier in this same cycle. nullopt means the object dictionary has not been
  // enumerated, or this is not a CiA402 drive.
  const auto statusword = drive->value<uint16_t>(cia402::kStatusword, 0);
  if (!statusword) {
    return;
  }

  // Safety gate. Commanding a setpoint at any other state either does nothing or fights whatever is
  // bringing the drive up, and enabling it here would move a machine nobody asked to move.
  if (cia402::decodeState(*statusword) != cia402::State::kOperationEnabled) {
    return;
  }

  // Same for the mode: 0x60FF is only the setpoint in CSV (and PV). Writing it in CSP would be
  // ignored at best.
  const auto mode = drive->value<int8_t>(cia402::kModeOfOperationDisplay, 0);
  if (!mode || cia402::toOperationMode(*mode) != cia402::OperationMode::kCyclicSyncVelocity) {
    return;
  }

  // The identical call, for an object that is almost certainly *not* in the process image — it is
  // polled in the background by the refresher (see MonitoringManager::keepFresh in main.cc). That
  // this line looks like the statusword read above is the whole point of the design.
  const auto temperature =
      drive->value<int16_t>(config_.temperatureIndex, config_.temperatureSubindex);
  if (!temperature) {
    return;  // wrong type, or the object is unknown to this device — do not guess a setpoint
  }

  const int32_t target =
      *temperature > config_.temperatureLimit ? config_.warmVelocity : config_.coolVelocity;

  // Stores into the cell; the RT loop composes it into the wire image and it goes out on the next
  // cycle. A read of 0x60FF now returns what was just set, not the last frame's value.
  drive->setValue<int32_t>(cia402::kTargetVelocity, 0, target);
}

}  // namespace mm::example
