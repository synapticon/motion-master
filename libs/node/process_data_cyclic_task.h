#pragma once

#include "core/cyclic_task.h"
#include "node/device_manager.h"

/// @brief Game-loop task that exchanges one cycle of process data.
///
/// Delegates to DeviceManager::exchangeProcessData, which is a no-op until a process image has
/// been published (i.e. devices have been mapped and brought into SAFE-OP/OP). Registering it
/// unconditionally is therefore safe: the RT loop drives PDO automatically as soon as the bus
/// is configured, and stops again when it is torn down — no wiring changes per state.
class ProcessDataCyclicTask : public CyclicTask {
 public:
  explicit ProcessDataCyclicTask(mm::node::DeviceManager& deviceManager)
      : deviceManager_(deviceManager) {}

  // Exchanges the freshest process data; ignores the timing context — skipped
  // cycles just mean fewer exchanges, which is correct (never a catch-up burst).
  //
  // exchangeProcessData is self-gating (it takes the same lock internally), so the CycleLock here
  // buys nothing for this task in particular. It is written anyway because this file is the
  // template a Tier-3 author copies, and every task that resolves a device of its own needs it —
  // the nesting is exactly what the depth counter behind it is for.
  void execute(const CycleContext&) override {
    const mm::node::DeviceManager::CycleLock cycle(deviceManager_);
    if (!cycle) {
      return;  // no image published — the bus is not activated, or is being reconfigured
    }
    deviceManager_.exchangeProcessData();
  }

 private:
  mm::node::DeviceManager& deviceManager_;
};
