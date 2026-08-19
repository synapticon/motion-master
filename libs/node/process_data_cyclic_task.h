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
  // GameLoop has already entered the cycle, and exchangeProcessData raises the same depth counter
  // again for callers that invoke it directly. The nesting is what that counter is for.
  void execute(const CycleContext&) override { deviceManager_.exchangeProcessData(); }

 private:
  mm::node::DeviceManager& deviceManager_;
};
