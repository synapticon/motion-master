#include "example/example_cyclic_task.h"

#include <cstdint>
#include <optional>

#include "node/cia402.h"
#include "node/device.h"

namespace mm::example {

using mm::node::Device;
using mm::node::DeviceManager;
namespace cia402 = mm::node::cia402;

namespace {

/// The mode this task drives in. 0x60FF is the setpoint in CSV; in CSP it is ignored.
constexpr cia402::OperationMode kMode = cia402::OperationMode::kCyclicSyncVelocity;

/// @brief Writes @p command into the controlword, preserving every bit outside the command mask.
///
/// A read-modify-write rather than a plain store, so the mode-specific bits (4..6), halt (8) and
/// any manufacturer bits above them survive a transition instead of being zeroed. @p controlword is
/// what we asked for last cycle, read back out of its own cell.
void applyCommand(Device& drive, uint16_t controlword, uint16_t command) {
  const auto next = static_cast<uint16_t>((controlword & ~cia402::kCommandMask) | command);
  drive.setValue<uint16_t>(cia402::kControlword, 0, next);
}

/// @brief The command bits that move @p state one step towards Operation Enabled.
///
/// The CiA402 state machine is a climb — Switch On Disabled → Ready To Switch On → Switched On →
/// Operation Enabled — and each step is one controlword write whose effect shows up in the next
/// statusword. A cyclic task therefore takes one step per cycle and lets the wire do the waiting,
/// which is why this belongs on the RT thread rather than in an off-RT loop that sleeps and polls.
///
/// @return The command bits, or @c nullopt when the right move is to wait.
std::optional<uint16_t> commandTowardsOperationEnabled(cia402::State state, uint16_t controlword) {
  switch (state) {
    case cia402::State::kSwitchOnDisabled:
      return uint16_t{cia402::kCmdShutdown};
    case cia402::State::kReadyToSwitchOn:
      return uint16_t{cia402::kCmdSwitchOn};
    case cia402::State::kSwitchedOn:
      return uint16_t{cia402::kCmdEnableOperation};
    case cia402::State::kQuickStopActive:
      // The only way out of quick stop is down to Switch On Disabled and back up — which is how
      // this task recovers by itself once the temperature falls again.
      return uint16_t{cia402::kCmdDisableVoltage};
    case cia402::State::kFault:
      // Bit 7 clears a fault on its *rising* edge, so it has to be low for one cycle before it goes
      // high. Reading our own controlword back out of its cell is what makes that possible: a write
      // reads back as itself, so the task can see what it asked for last cycle.
      return (controlword & cia402::kCmdFaultReset) != 0 ? uint16_t{0}
                                                         : uint16_t{cia402::kCmdFaultReset};
    case cia402::State::kFaultReactionActive:  // the drive is stopping itself — let it finish
    case cia402::State::kNotReadyToSwitchOn:   // still initialising
    case cia402::State::kOperationEnabled:     // already there
      return std::nullopt;
  }
  return std::nullopt;
}

}  // namespace

ExampleCyclicTask::ExampleCyclicTask(DeviceManager& deviceManager, Config config)
    : deviceManager_(deviceManager), config_(config) {}

void ExampleCyclicTask::execute(const CycleContext& /*ctx*/) noexcept {
  // Rule 1 and 2 — resolve every cycle, and treat absence as "not this cycle" rather than an error.
  Device* drive = deviceManager_.findDevice(config_.slavePosition);
  if (drive == nullptr) {
    return;
  }

  // Rule 3 — read through the cell. This is the live statusword: the RT exchange decoded it into
  // the cell earlier in this same cycle. nullopt means the object dictionary has not been
  // enumerated, or this is not a CiA402 drive — either way there is nothing to command.
  const auto statusword = drive->value<uint16_t>(cia402::kStatusword, 0);
  if (!statusword) {
    return;
  }
  const cia402::State state = cia402::decodeState(*statusword);
  const uint16_t controlword = drive->value<uint16_t>(cia402::kControlword, 0).value_or(0);

  // --- the interlock, before anything that could start motion ------------------------------
  //
  // The temperature read is the identical call to the statusword read above, for an object that is
  // almost certainly *not* in the process image — it is polled in the background by the refresher
  // (MonitoringManager::keepFresh, see main.cc). That these two lines look the same is the point of
  // the design: whether a value rides the cyclic image is a commissioning decision, and it does not
  // reach into the control program.
  //
  // **No reading counts as unsafe.** An interlock that runs the motor whenever it cannot see the
  // temperature is not an interlock. If the drive never spins, that is the first thing to check —
  // most likely keepFresh in main.cc naming a different object than Config::temperature does.
  const auto temperature = drive->value(config_.temperature);
  if (!temperature || *temperature > config_.temperatureLimit) {
    if (state == cia402::State::kOperationEnabled) {
      // Quick stop rather than removing the setpoint: the drive decelerates on its own
      // 0x6085 ramp and holds, instead of coasting because nobody is commanding it any more.
      applyCommand(*drive, controlword, cia402::kCmdQuickStop);
    }
    return;  // and never climb towards enabled while it is too hot
  }

  // --- bring the drive up, one step per cycle ------------------------------------------------

  // The mode first: enabling in the wrong mode only means the setpoint below is ignored. Ask for
  // CSV and come back next cycle — the drive reports the change in 0x6061 once it has taken it, so
  // there is nothing to wait on here.
  const auto mode = drive->value<int8_t>(cia402::kModeOfOperationDisplay, 0);
  if (!mode) {
    return;
  }
  if (cia402::toOperationMode(*mode) != kMode) {
    drive->setValue<int8_t>(cia402::kModeOfOperation, 0, static_cast<int8_t>(kMode));
    return;
  }

  // Then the CiA402 state machine. This is exactly the kind of work a cyclic task exists for:
  // controlword out, statusword in, both in the process image, so the climb costs one write per
  // cycle and no waiting at all. (What must stay off this thread is the *EtherCAT* AL state —
  // INIT/PRE-OP/SAFE-OP/OP — which is seconds of mailbox and register traffic. A task runs in OP
  // and never touches it.)
  if (state != cia402::State::kOperationEnabled) {
    if (const auto command = commandTowardsOperationEnabled(state, controlword)) {
      applyCommand(*drive, controlword, *command);
    }
    return;  // no setpoint until the drive is actually following one
  }

  // --- enabled, in CSV, and cool enough --------------------------------------------------------
  //
  // Stores into the cell; the RT loop composes it into the wire image and it goes out on the next
  // cycle. A read of 0x60FF now returns what was just set, not the last frame's value.
  drive->setValue<int32_t>(cia402::kTargetVelocity, 0, config_.velocity);
}

}  // namespace mm::example
