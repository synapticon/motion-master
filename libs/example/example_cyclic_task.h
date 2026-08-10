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
/// **What it does — a very naive thermal interlock.** It brings one drive into cyclic synchronous
/// velocity mode, enables it, and runs it at a fixed velocity; if the drive's temperature goes over
/// a limit it issues a **quick stop**, and it will not enable again until the temperature falls, at
/// which point it climbs back up by itself. Naive is the word: a real interlock has hysteresis, an
/// alarm path, and a considered answer for every sensor failure mode. What is realistic here is the
/// *shape* — read state, decide, command, once per cycle, with nothing that can block.
///
/// @warning **Uncommenting this in @c main.cc will spin a motor.** It does not wait for anyone to
///          enable the drive; enabling is its job. Check @c Config against your bus first.
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
///    process image or is polled over SDO in the background. Whether the temperature is PDO-mapped
///    is a commissioning decision, and this code does not change if it flips.
///
/// **Driving the CiA402 state machine from here is the right place for it, not a liberty.** Mode of
/// operation, controlword and statusword are all in the process image and all exchanged every
/// cycle, so the climb (Switch On Disabled → Ready To Switch On → Switched On → Operation Enabled)
/// is one write per cycle with the wire doing the waiting — where an off-RT caller has to sleep and
/// poll for the same result. What must stay off the RT thread is the **EtherCAT AL state** (INIT /
/// PRE-OP / SAFE-OP / OP): seconds of mailbox and register traffic, done through the HTTP API. A
/// task runs in OP and never touches it.
///
/// **One value here is not free.** @c Config's temperature object is SDO-only on most drives, so
/// nothing refills it cyclically — @c MonitoringManager::keepFresh has to be called once, off the
/// RT thread, before the loop starts. Without it the read never produces a value, the interlock
/// treats that as unsafe, and the drive never spins. @c main.cc shows the call.
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
    /// read is type-exact and a mismatch reads as nothing at all — which this task treats as too
    /// hot to run.
    uint16_t temperatureIndex = 0x2030;
    uint8_t temperatureSubindex = 0x01;

    /// Above this, the drive is quick-stopped and held until it cools. Units are the object's own.
    int16_t temperatureLimit = 60;

    /// The velocity commanded while the drive is enabled and below the limit, in whatever units
    /// 0x60FF uses on your drive (RPM on a SOMANET drive configured that way). Keep it small while
    /// trying this out.
    int32_t velocity = 100;
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
