#pragma once

#include <cstdint>

#include "core/cyclic_task.h"
#include "node/device_manager.h"

namespace mm::example {

/// @brief Copy-me starter for a **Tier 3** extension: your own code inside the real-time loop.
///
/// This is the counterpart of @c example_routes.h, which extends the HTTP API (Tier 2). Where a
/// route plug-in runs on an HTTP thread and may block, a @c CyclicTask runs on the RT thread, once
/// per cycle, and may not: no locks, no allocation, no bus I/O, and it must return well inside the
/// cycle period (1 ms by default). Everything below honours that, and the rules it follows are the
/// whole point of reading it.
///
/// **What it does.** Watches one drive's temperature and commands a velocity that depends on it —
/// faster when warm, slower when cool — but only while the drive is already enabled and in cyclic
/// synchronous velocity mode. Trivial as control goes; it is here to show the shape.
///
/// **The four rules a cyclic task lives by**, each visible in @c execute:
///
/// 1. **Take a @c DeviceManager::CycleLock first, and do nothing if it is falsy.** It holds the
///    device set still for the body of the cycle. Falsy means the bus is not activated — nobody has
///    brought it to SAFE-OP/OP yet, or a scan/re-map is in progress — so there is nothing to do.
/// 2. **Resolve the device every cycle; never cache the pointer.** A rescan destroys every
///    @c Device, and pointers held across cycles die with them. Resolving costs a walk over a
///    handful of devices.
/// 3. **A device that is not there is not an error.** It simply is not driven this cycle, and the
///    task picks it up again when it returns. That is what lets a machine be powered up in stages.
/// 4. **Read and write values through @c Device::value<T>() / @c setValue<T>().** Both are a hash
///    lookup plus one atomic load or store, and neither can tell whether an object rides the
///    process image or is polled over SDO in the background. Whether the temperature below is
///    PDO-mapped is a commissioning decision, and this code does not change if it flips.
///
/// **What it deliberately does not do: enable the drive.** It commands a setpoint only when the
/// statusword already reports @c kOperationEnabled and the mode is already CSV. Bringing a drive up
/// is a sequence of state transitions belonging off the RT thread (the HTTP API and the console do
/// it); a task that enables a motor as a side effect of being registered is a task that moves a
/// machine nobody asked to move.
///
/// **One value here is not free.** @c Config::temperature is an SDO-only object on most drives, so
/// nothing refills it cyclically — @c MonitoringManager::keepFresh has to be called once, off the
/// RT thread, before the loop starts. Without it the read returns the type's zero forever and the
/// task quietly picks the cool branch. @c main.cc shows the call.
class ExampleCyclicTask : public CyclicTask {
 public:
  /// @brief Everything the task needs to know, so nothing is hard-coded in @c execute.
  struct Config {
    /// 1-based bus position of the drive to watch. Position is not identity: inserting a device
    /// into the chain shifts every position after it, so re-check this after changing the bus.
    uint16_t slavePosition = 1;

    /// The temperature object. **A placeholder — replace it with your drive's.** Read the device's
    /// object dictionary (the console's Parameters page) rather than assuming an index, and if its
    /// data type is not INTEGER16, change the @c value<int16_t> call in @c execute to match: the
    /// read is type-exact and a mismatch reads as nothing at all.
    uint16_t temperatureIndex = 0x2030;
    uint8_t temperatureSubindex = 0x01;

    /// Above this temperature the drive is commanded @c warmVelocity, at or below it
    /// @c coolVelocity. Units are the object's own.
    int16_t temperatureLimit = 55;

    /// Velocity setpoints, in whatever units 0x60FF uses on your drive (RPM on a SOMANET drive
    /// configured that way). Keep them small while trying this out.
    int32_t warmVelocity = 200;
    int32_t coolVelocity = 100;
  };

  /// @brief Binds the task to a device manager. Registers nothing and touches no device — a task is
  ///        constructed before the bus exists and must tolerate that.
  ExampleCyclicTask(mm::node::DeviceManager& deviceManager, Config config);

  /// @brief Runs one cycle. Non-blocking, non-allocating, lock-free.
  void execute(const CycleContext& ctx) override;

 private:
  mm::node::DeviceManager& deviceManager_;
  Config config_;
};

}  // namespace mm::example
