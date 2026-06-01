#pragma once

#include "cyclic_task.h"
#include "node/device_manager.h"

/// @brief Game-loop task that exchanges one cycle of process data.
///
/// Delegates to DeviceManager::exchangeProcessData, which is a no-op until a process image has
/// been published (i.e. devices have been mapped and brought into SAFE-OP/OP). Registering it
/// unconditionally is therefore safe: the RT loop drives PDO automatically as soon as the bus
/// is configured, and stops again when it is torn down — no wiring changes per state.
class ProcessDataTask : public CyclicTask {
 public:
  explicit ProcessDataTask(mm::node::DeviceManager& deviceManager)
      : deviceManager_(deviceManager) {}

  void execute() override { deviceManager_.exchangeProcessData(); }

 private:
  mm::node::DeviceManager& deviceManager_;
};
