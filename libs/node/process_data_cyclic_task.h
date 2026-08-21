#pragma once

#include "core/cyclic_task.h"
#include "node/device_manager.h"

namespace mm::node {

/// @brief Game-loop task that exchanges one cycle of process data.
///
/// Delegates to DeviceManager::exchangeProcessData, which is a no-op until a process image has
/// been published (i.e. the devices are mapped and in SAFE-OP or OP). Registering it
/// unconditionally is therefore safe: the RT loop drives PDO automatically as soon as the bus
/// is configured, and stops again when it is torn down — no wiring changes per state.
class ProcessDataCyclicTask : public mm::core::CyclicTask {
 public:
  explicit ProcessDataCyclicTask(DeviceManager& deviceManager) : deviceManager_(deviceManager) {}

  // Exchanges the freshest process data; ignores the timing context — skipped
  // cycles just mean fewer exchanges, which is correct (never a catch-up burst).
  //
  // GameLoop enters the cycle before this call, and exchangeProcessData raises the same depth
  // counter again for callers that invoke it directly. The nesting is what that counter is for.
  void execute(const mm::core::CycleContext&) noexcept override {
    deviceManager_.exchangeProcessData();
  }

 private:
  DeviceManager& deviceManager_;
};

}  // namespace mm::node
