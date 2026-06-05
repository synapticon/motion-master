#pragma once

#include <cstdint>
#include <string_view>

/// @file
/// @brief Pure CiA402 (CANopen drive profile, ETG.6010) vocabulary — object indices,
///        control/status word bit layout, the device state machine, and operation modes.
///
/// This header is deliberately free of any dependency on @c Device: it is the protocol
/// definition, decoupled from the transport, so the bit-twiddling can be unit-tested in
/// isolation and reused by both @c Cia402Drive (the borrowed view) and any cyclic task that
/// composes a controlword by hand. The behaviour that actually touches the bus lives in
/// @c cia402_drive.h.

namespace mm::node::cia402 {

/// @brief Standard CiA402 object dictionary indices used by the drive profile.
///
/// Subindex is always 0 for these (they are simple objects), so callers pass @c 0.
enum Object : uint16_t {
  kControlword = 0x6040,             ///< UNSIGNED16, RxPDO — commands the state machine.
  kStatusword = 0x6041,              ///< UNSIGNED16, TxPDO — reports the state machine.
  kModeOfOperation = 0x6060,         ///< INTEGER8, RxPDO — requested operation mode.
  kModeOfOperationDisplay = 0x6061,  ///< INTEGER8, TxPDO — active operation mode.
  kTargetPosition = 0x607A,          ///< INTEGER32, RxPDO — CSP/PP setpoint.
  kTargetVelocity = 0x60FF,          ///< INTEGER32, RxPDO — CSV/PV setpoint.
  kTargetTorque = 0x6071,            ///< INTEGER16, RxPDO — CST/PT setpoint (per-mille of rated).
  kPositionActualValue = 0x6064,     ///< INTEGER32, TxPDO — actual position.
  kVelocityActualValue = 0x606C,     ///< INTEGER32, TxPDO — actual velocity.
  kTorqueActualValue = 0x6077,       ///< INTEGER16, TxPDO — actual torque.
};

/// @brief CiA402 operation modes (object 0x6060 / 0x6061 values).
enum class OperationMode : int8_t {
  kNoMode = 0,              ///< No mode assigned.
  kProfilePosition = 1,     ///< PP — trapezoidal point-to-point positioning.
  kProfileVelocity = 3,     ///< PV — profiled velocity.
  kProfileTorque = 4,       ///< PT — profiled torque.
  kHoming = 6,              ///< HM — reference/homing run.
  kCyclicSyncPosition = 8,  ///< CSP — interpolated cyclic position (the common SOMANET mode).
  kCyclicSyncVelocity = 9,  ///< CSV — interpolated cyclic velocity.
  kCyclicSyncTorque = 10,   ///< CST — interpolated cyclic torque.
};

/// @brief States of the CiA402 device control state machine (decoded from the statusword).
enum class State : uint8_t {
  kNotReadyToSwitchOn,   ///< Drive initialising; no clear state yet.
  kSwitchOnDisabled,     ///< Powered, holding; the resting state after init.
  kReadyToSwitchOn,      ///< Shutdown command accepted.
  kSwitchedOn,           ///< Switch-on command accepted; power stage on, no motion.
  kOperationEnabled,     ///< Fully enabled; setpoints are followed.
  kQuickStopActive,      ///< Quick stop in progress.
  kFaultReactionActive,  ///< A fault is being reacted to (e.g. controlled stop).
  kFault,                ///< Faulted and stopped; needs a fault reset.
};

/// @brief Controlword (0x6040) command-bit mask — the state-machine bits a transition touches.
///
/// Bits 0 (switch on), 1 (enable voltage), 2 (quick stop), 3 (enable operation), 7 (fault
/// reset). All other bits — mode-specific 4..6, halt 8, and manufacturer 9.. — are preserved
/// across a transition by a read-modify-write masked with this value.
constexpr uint16_t kCommandMask = 0x008F;

/// @brief Canonical controlword command-bit patterns (the value of the bits in @c kCommandMask).
enum Command : uint16_t {
  kCmdShutdown = 0x0006,         ///< → ReadyToSwitchOn (enable voltage + no quick stop).
  kCmdSwitchOn = 0x0007,         ///< → SwitchedOn (also "disable operation" from OperationEnabled).
  kCmdEnableOperation = 0x000F,  ///< → OperationEnabled.
  kCmdDisableVoltage = 0x0000,   ///< → SwitchOnDisabled.
  kCmdQuickStop = 0x0002,        ///< → QuickStopActive.
  kCmdFaultReset = 0x0080,       ///< Rising edge of bit 7 clears a fault.
};

/// @brief Decodes the CiA402 state machine state from a statusword (0x6041) value.
///
/// Pure function of the statusword's defined bits; uses the ETG.6010 mask/match table. An
/// unrecognised pattern maps to @c State::kNotReadyToSwitchOn (the "no clear state" bucket).
constexpr State decodeState(uint16_t statusword) {
  // The two masks the standard uses: 0x4F distinguishes the states that ignore bit 5
  // (quick stop), 0x6F the rest.
  if ((statusword & 0x4F) == 0x40) {
    return State::kSwitchOnDisabled;
  }
  if ((statusword & 0x6F) == 0x21) {
    return State::kReadyToSwitchOn;
  }
  if ((statusword & 0x6F) == 0x23) {
    return State::kSwitchedOn;
  }
  if ((statusword & 0x6F) == 0x27) {
    return State::kOperationEnabled;
  }
  if ((statusword & 0x6F) == 0x07) {
    return State::kQuickStopActive;
  }
  if ((statusword & 0x4F) == 0x0F) {
    return State::kFaultReactionActive;
  }
  if ((statusword & 0x4F) == 0x08) {
    return State::kFault;
  }
  return State::kNotReadyToSwitchOn;
}

/// @brief Whether the statusword's fault bit (bit 3) is set, gated to the faulted states.
constexpr bool isFaulted(uint16_t statusword) {
  const State s = decodeState(statusword);
  return s == State::kFault || s == State::kFaultReactionActive;
}

/// @brief Human-readable name of a state (for logging / JSON). Never returns @c nullptr.
constexpr std::string_view toString(State state) {
  switch (state) {
    case State::kNotReadyToSwitchOn:
      return "NotReadyToSwitchOn";
    case State::kSwitchOnDisabled:
      return "SwitchOnDisabled";
    case State::kReadyToSwitchOn:
      return "ReadyToSwitchOn";
    case State::kSwitchedOn:
      return "SwitchedOn";
    case State::kOperationEnabled:
      return "OperationEnabled";
    case State::kQuickStopActive:
      return "QuickStopActive";
    case State::kFaultReactionActive:
      return "FaultReactionActive";
    case State::kFault:
      return "Fault";
  }
  return "Unknown";
}

/// @brief Human-readable name of an operation mode (for logging / JSON). Never @c nullptr.
constexpr std::string_view toString(OperationMode mode) {
  switch (mode) {
    case OperationMode::kNoMode:
      return "NoMode";
    case OperationMode::kProfilePosition:
      return "ProfilePosition";
    case OperationMode::kProfileVelocity:
      return "ProfileVelocity";
    case OperationMode::kProfileTorque:
      return "ProfileTorque";
    case OperationMode::kHoming:
      return "Homing";
    case OperationMode::kCyclicSyncPosition:
      return "CyclicSyncPosition";
    case OperationMode::kCyclicSyncVelocity:
      return "CyclicSyncVelocity";
    case OperationMode::kCyclicSyncTorque:
      return "CyclicSyncTorque";
  }
  return "Unknown";
}

}  // namespace mm::node::cia402
