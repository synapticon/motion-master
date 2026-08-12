#pragma once

#include <cstdint>
#include <optional>
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

/// @brief Standard CiA402 object dictionary indices used by the drive profile, ordered by index.
///
/// Simple VAR objects are addressed at subindex 0; the two-element ARRAY objects (0x607B,
/// 0x607D, 0x6091, 0x6092, 0x6099, 0x60FE) at sub-entries 1..2.
enum Object : uint16_t {
  kErrorCode = 0x603F,                ///< UNSIGNED16, ro, TxPDO — code of the last drive fault.
  kControlword = 0x6040,              ///< UNSIGNED16, rw, RxPDO — commands the state machine.
  kStatusword = 0x6041,               ///< UNSIGNED16, ro, TxPDO — reports the state machine.
  kQuickStopOptionCode = 0x605A,      ///< INTEGER16, rw — reaction to a quick stop.
  kModeOfOperation = 0x6060,          ///< INTEGER8, rw, RxPDO — requested operation mode.
  kModeOfOperationDisplay = 0x6061,   ///< INTEGER8, ro, TxPDO — active operation mode.
  kPositionDemandValue = 0x6062,      ///< INTEGER32, ro, TxPDO — trajectory generator demand.
  kPositionActualValue = 0x6064,      ///< INTEGER32, ro, TxPDO — actual position.
  kFollowingErrorWindow = 0x6065,     ///< UNSIGNED32, rw, RxPDO — max tolerated following error.
  kFollowingErrorTimeout = 0x6066,    ///< UNSIGNED16, rw, RxPDO — ms outside window before fault.
  kPositionWindow = 0x6067,           ///< UNSIGNED32, rw, RxPDO — target-reached position window.
  kPositionWindowTime = 0x6068,       ///< UNSIGNED16, rw, RxPDO — ms in window for target reached.
  kVelocityDemandValue = 0x606B,      ///< INTEGER32, ro, TxPDO — ramp generator demand.
  kVelocityActualValue = 0x606C,      ///< INTEGER32, ro, TxPDO — actual velocity.
  kVelocityWindow = 0x606D,           ///< UNSIGNED16, rw, RxPDO — target-reached velocity window.
  kVelocityWindowTime = 0x606E,       ///< UNSIGNED16, rw, RxPDO — ms in window for target reached.
  kVelocityThreshold = 0x606F,        ///< UNSIGNED16, rw, RxPDO — standstill velocity threshold.
  kVelocityThresholdTime = 0x6070,    ///< UNSIGNED16, rw, RxPDO — ms below threshold = standstill.
  kTargetTorque = 0x6071,             ///< INTEGER16, rw, RxPDO — CST/PT setpoint (per-mille).
  kMaxTorque = 0x6072,                ///< UNSIGNED16, rw, RxPDO — torque limit (per-mille).
  kMaxCurrent = 0x6073,               ///< UNSIGNED16, rw, RxPDO — current limit (per-mille).
  kTorqueDemand = 0x6074,             ///< INTEGER16, ro, TxPDO — control loop torque demand.
  kMotorRatedCurrent = 0x6075,        ///< UNSIGNED32, rw — motor rated current (mA).
  kMotorRatedTorque = 0x6076,         ///< UNSIGNED32, rw — motor rated torque (mNm).
  kTorqueActualValue = 0x6077,        ///< INTEGER16, ro, TxPDO — actual torque.
  kDcLinkCircuitVoltage = 0x6079,     ///< UNSIGNED32, ro, TxPDO — DC bus voltage (mV).
  kTargetPosition = 0x607A,           ///< INTEGER32, rw, RxPDO — CSP/PP setpoint.
  kPositionRangeLimit = 0x607B,       ///< 2×INTEGER32, rw — position wrap range (min/max).
  kHomeOffset = 0x607C,               ///< INTEGER32, rw, RxPDO — home offset from machine zero.
  kSoftwarePositionLimit = 0x607D,    ///< 2×INTEGER32, rw — software end stops (min/max).
  kPolarity = 0x607E,                 ///< UNSIGNED8, rw, RxPDO — position/velocity polarity bits.
  kMaxMotorSpeed = 0x6080,            ///< UNSIGNED32, rw, RxPDO — motor speed limit.
  kProfileVelocity = 0x6081,          ///< UNSIGNED32, rw, RxPDO — PP cruise velocity.
  kProfileAcceleration = 0x6083,      ///< UNSIGNED32, rw, RxPDO — profile acceleration.
  kProfileDeceleration = 0x6084,      ///< UNSIGNED32, rw, RxPDO — profile deceleration.
  kQuickStopDeceleration = 0x6085,    ///< UNSIGNED32, rw, RxPDO — deceleration on quick stop.
  kMotionProfileType = 0x6086,        ///< INTEGER16, rw, RxPDO — trajectory shape.
  kTorqueSlope = 0x6087,              ///< UNSIGNED32, rw, RxPDO — PT torque ramp rate.
  kTorqueProfileType = 0x6088,        ///< INTEGER16, rw, RxPDO — torque trajectory shape.
  kGearRatio = 0x6091,                ///< 2×UNSIGNED32, rw — motor/shaft revolutions.
  kFeedConstant = 0x6092,             ///< 2×UNSIGNED32, rw — feed per shaft revolutions.
  kHomingMethod = 0x6098,             ///< INTEGER8, rw, RxPDO — homing method number.
  kHomingSpeeds = 0x6099,             ///< 2×UNSIGNED32, rw — switch/zero search speeds.
  kHomingAcceleration = 0x609A,       ///< UNSIGNED32, rw, RxPDO — homing acceleration.
  kSiUnitVelocity = 0x60A9,           ///< UNSIGNED32, rw — SI unit code of velocity objects.
  kVelocityOffset = 0x60B1,           ///< INTEGER32, rw, RxPDO — CSP/CSV velocity feed-forward.
  kTorqueOffset = 0x60B2,             ///< INTEGER16, rw, RxPDO — torque feed-forward.
  kTouchProbeFunction = 0x60B8,       ///< UNSIGNED16, rw, RxPDO — touch probe arm/config bits.
  kTouchProbeStatus = 0x60B9,         ///< UNSIGNED16, ro, TxPDO — touch probe latch status.
  kTouchProbe1PositiveEdge = 0x60BA,  ///< INTEGER32, ro, TxPDO — position at probe 1 rising edge.
  kTouchProbe1NegativeEdge = 0x60BB,  ///< INTEGER32, ro, TxPDO — position at probe 1 falling
                                      ///< edge.
  kTouchProbeTimeStamp1PositiveValue = 0x60D1,  ///< UNSIGNED32, ro, TxPDO — time stamp at probe 1
                                                ///< rising edge.
  kTouchProbeTimeStamp1NegativeValue = 0x60D2,  ///< UNSIGNED32, ro, TxPDO — time stamp at probe 1
                                                ///< falling edge.
  kPositioningOptionCode = 0x60F2,              ///< UNSIGNED16, rw, PDO — PP positioning options.
  kFollowingErrorActualValue = 0x60F4,          ///< INTEGER32, ro, TxPDO — live following error.
  kControlEffort = 0x60FA,                      ///< INTEGER32, ro, TxPDO — position loop output.
  kPositionDemandInternalValue = 0x60FC,        ///< INTEGER32, ro, TxPDO — demand in internal
                                                ///< increments.
  kDigitalInputs = 0x60FD,        ///< UNSIGNED32, ro, TxPDO — digital input bit field.
  kDigitalOutputs = 0x60FE,       ///< 2×UNSIGNED32, rw — physical outputs + enable mask.
  kTargetVelocity = 0x60FF,       ///< INTEGER32, rw, RxPDO — CSV/PV setpoint.
  kSupportedDriveModes = 0x6502,  ///< UNSIGNED32, ro — capability bit field of supported modes.
};

/// @brief CiA402 operation modes (object 0x6060 / 0x6061 values).
///
/// Every mode the profile defines, whether or not this codebase can drive one: the set is what a
/// device's 0x6502 capability field is read against, so a mode missing here would be a supported
/// mode nothing could name. Mode 5 is absent because the profile reserves it — there is no mode 5.
/// Vendors define their own modes in the negative half, which is a *manufacturer* vocabulary and
/// therefore lives with the vendor (@c somanet::OperationMode), not here.
enum class OperationMode : int8_t {
  kNoMode = 0,                             ///< No mode assigned. Always legal; has no 0x6502 bit.
  kProfilePosition = 1,                    ///< PP — trapezoidal point-to-point positioning.
  kVelocity = 2,                           ///< VL — the frequency-converter velocity mode.
  kProfileVelocity = 3,                    ///< PV — profiled velocity.
  kProfileTorque = 4,                      ///< PT — profiled torque.
  kHoming = 6,                             ///< HM — reference/homing run.
  kInterpolatedPosition = 7,               ///< IP — interpolated position.
  kCyclicSyncPosition = 8,                 ///< CSP — cyclic position (the common SOMANET mode).
  kCyclicSyncVelocity = 9,                 ///< CSV — cyclic velocity.
  kCyclicSyncTorque = 10,                  ///< CST — cyclic torque.
  kCyclicSyncTorqueCommutationAngle = 11,  ///< CSTCA — cyclic torque with commutation angle.
};

/// @brief Maps a raw mode value (as written to 0x6060 / read from 0x6061) to a known operation
///        mode. Rejects values outside the standard set so an API boundary can 400 an unknown mode.
/// @return The mode, or @c std::nullopt if @p value is not a recognised operation mode.
constexpr std::optional<OperationMode> toOperationMode(int value) {
  // 0x6060 is an INTEGER8, so a value that does not fit int8_t is not a valid mode — reject it
  // before the narrowing cast, or e.g. 264 would alias to 8 (CSP) and slip past validation.
  if (value < INT8_MIN || value > INT8_MAX) {
    return std::nullopt;
  }
  switch (static_cast<OperationMode>(static_cast<int8_t>(value))) {
    case OperationMode::kNoMode:
    case OperationMode::kProfilePosition:
    case OperationMode::kVelocity:
    case OperationMode::kProfileVelocity:
    case OperationMode::kProfileTorque:
    case OperationMode::kHoming:
    case OperationMode::kInterpolatedPosition:
    case OperationMode::kCyclicSyncPosition:
    case OperationMode::kCyclicSyncVelocity:
    case OperationMode::kCyclicSyncTorque:
    case OperationMode::kCyclicSyncTorqueCommutationAngle:
      return static_cast<OperationMode>(static_cast<int8_t>(value));
  }
  return std::nullopt;
}

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
    case OperationMode::kVelocity:
      return "Velocity";
    case OperationMode::kProfileVelocity:
      return "ProfileVelocity";
    case OperationMode::kProfileTorque:
      return "ProfileTorque";
    case OperationMode::kHoming:
      return "Homing";
    case OperationMode::kInterpolatedPosition:
      return "InterpolatedPosition";
    case OperationMode::kCyclicSyncPosition:
      return "CyclicSyncPosition";
    case OperationMode::kCyclicSyncVelocity:
      return "CyclicSyncVelocity";
    case OperationMode::kCyclicSyncTorque:
      return "CyclicSyncTorque";
    case OperationMode::kCyclicSyncTorqueCommutationAngle:
      return "CyclicSyncTorqueCommutationAngle";
  }
  return "Unknown";
}

/// @brief One row of the standard operation-mode table — a mode, the 0x6502 bit that advertises it,
///        and the two names the profile gives it.
struct StandardOperationMode {
  OperationMode mode{OperationMode::kNoMode};
  /// The bit of 0x6502 "Supported drive modes" that advertises this mode, or -1 for @c kNoMode,
  /// which the capability field has no bit for because it is always legal.
  int bit = -1;
  std::string_view abbreviation;  ///< The profile's short form: "csp", "hm", …. Empty for kNoMode.
  std::string_view label;         ///< The profile's own wording, for a user-facing list.
};

/// @brief Every standard operation mode, with its 0x6502 bit — ETG.6010 §6.8.1, Figure 15.
///
/// **The bit is listed rather than computed**, though `bit = value - 1` holds for all ten of them.
/// The arithmetic is a coincidence of how the profile happened to allocate the field, not a rule it
/// states, and a future mode that broke it would break silently. Reserved positions (bit 4, and
/// 11-15) are simply absent, as is the manufacturer half (bits 16-31), which no standard table can
/// name.
inline constexpr StandardOperationMode kStandardOperationModes[] = {
    {OperationMode::kNoMode, -1, "", "No mode selected"},
    {OperationMode::kProfilePosition, 0, "pp", "Profile position mode"},
    {OperationMode::kVelocity, 1, "vl", "Velocity mode (frequency converter)"},
    {OperationMode::kProfileVelocity, 2, "pv", "Profile velocity mode"},
    {OperationMode::kProfileTorque, 3, "tq", "Profile torque mode"},
    {OperationMode::kHoming, 5, "hm", "Homing mode"},
    {OperationMode::kInterpolatedPosition, 6, "ip", "Interpolated position mode"},
    {OperationMode::kCyclicSyncPosition, 7, "csp", "Cyclic synchronous position mode"},
    {OperationMode::kCyclicSyncVelocity, 8, "csv", "Cyclic synchronous velocity mode"},
    {OperationMode::kCyclicSyncTorque, 9, "cst", "Cyclic synchronous torque mode"},
    {OperationMode::kCyclicSyncTorqueCommutationAngle, 10, "cstca",
     "Cyclic synchronous torque mode with commutation angle"},
};

/// @brief The first bit of 0x6502 that the profile leaves to the vendor (ETG.6010 Figure 15).
inline constexpr int kFirstManufacturerDriveModeBit = 16;

/// @brief Parses a state name as @c toString spells it. @c std::nullopt for anything else.
constexpr std::optional<State> parseState(std::string_view token) {
  for (const auto state : {State::kNotReadyToSwitchOn, State::kSwitchOnDisabled,
                           State::kReadyToSwitchOn, State::kSwitchedOn, State::kOperationEnabled,
                           State::kQuickStopActive, State::kFaultReactionActive, State::kFault}) {
    if (token == toString(state)) {
      return state;
    }
  }
  return std::nullopt;
}

/// @brief Whether a master can ask a drive to reach @p state.
///
/// Three of the eight cannot be asked for, and each for its own reason: @c kNotReadyToSwitchOn and
/// @c kFaultReactionActive are passed through automatically by the drive and have no command that
/// enters them, and @c kFault is entered by something going wrong rather than by being requested.
constexpr bool isCommandableState(State state) {
  switch (state) {
    case State::kSwitchOnDisabled:
    case State::kReadyToSwitchOn:
    case State::kSwitchedOn:
    case State::kOperationEnabled:
    case State::kQuickStopActive:
      return true;
    case State::kNotReadyToSwitchOn:
    case State::kFaultReactionActive:
    case State::kFault:
      return false;
  }
  return false;
}

/// @brief What a master should do next, having observed one state and wanting another.
enum class FsaAction : uint8_t {
  kArrived,  ///< Already there; nothing to issue.
  kCommand,  ///< Issue @c FsaTransition::command and look again.
  kWait,     ///< The drive is mid-transition on its own; issue nothing and look again.
  /// No path exists — only when the target is not a @c isCommandableState, or when a quick stop
  /// must be overridden and the caller did not permit it.
  kUnreachable,
};

/// @brief The next step of a walk toward a target state.
struct FsaTransition {
  FsaAction action{FsaAction::kUnreachable};
  uint16_t command = 0;  ///< Meaningful only for @c FsaAction::kCommand.
};

/// @brief One step of the walk from @p from toward @p target — the CiA402 state machine as a
///        next-hop table.
///
/// **A table rather than a search**, because the graph is eight states and fits on a page, and
/// because the interesting part is not the pathfinding but the handful of states that behave
/// unlike the rest. Iterating it is what produces a multi-hop walk: each call answers only "what
/// now", so a caller re-reads the drive between steps and never assumes a command took effect.
///
/// The graph is ETG.6010 §5.1 Figure 2, checked against the SOMANET firmware's own
/// @c get_next_state. Four rows are worth knowing:
///
///   - **@c kFault** — every target begins with a fault reset, which reaches @c kSwitchOnDisabled
///     (transition 15) *and only if the cause is gone*; the firmware refuses while the error is
///     still reported, so a caller must not read a reset as progress.
///   - **@c kFaultReactionActive** and **@c kNotReadyToSwitchOn** — no command enters or leaves
///     them; the drive moves on by itself (transitions 14 and 1), so the answer is to wait.
///   - **@c kQuickStopActive** — leaving downward is @c kCmdDisableVoltage (transition 12), and
///     leaving *upward* to @c kOperationEnabled is transition 16, which IEC 61800-7-201 calls not
///     recommended and this firmware implements as "forced". It is offered only when
///     @p allowQuickStopOverride, so that overriding a deliberate stop is a decision a caller
///     makes rather than a side effect of asking to be enabled.
///   - **@c kReadyToSwitchOn → @c kSwitchedOn** uses @c kCmdSwitchOn rather than the combined
///     @c kCmdEnableOperation that ETG.6010 permits: the firmware answers both by moving one
///     state, so the plain command reaches the same place while leaving the intermediate state
///     observable.
///
/// @param from                    The state just read from the drive.
/// @param target                  Where the caller wants it. See @c isCommandableState.
/// @param allowQuickStopOverride  Whether transition 16 may be used.
constexpr FsaTransition nextFsaTransition(State from, State target, bool allowQuickStopOverride) {
  if (!isCommandableState(target)) {
    return {FsaAction::kUnreachable, 0};
  }
  if (from == target) {
    return {FsaAction::kArrived, 0};
  }
  switch (from) {
    case State::kNotReadyToSwitchOn:
    case State::kFaultReactionActive:
      return {FsaAction::kWait, 0};

    case State::kFault:
      return {FsaAction::kCommand, Command::kCmdFaultReset};

    case State::kSwitchOnDisabled:
      // The only way out is upward, whatever the target beyond it.
      return {FsaAction::kCommand, Command::kCmdShutdown};

    case State::kReadyToSwitchOn:
      if (target == State::kSwitchOnDisabled) {
        return {FsaAction::kCommand, Command::kCmdDisableVoltage};
      }
      return {FsaAction::kCommand, Command::kCmdSwitchOn};

    case State::kSwitchedOn:
      if (target == State::kSwitchOnDisabled) {
        return {FsaAction::kCommand, Command::kCmdDisableVoltage};
      }
      if (target == State::kReadyToSwitchOn) {
        return {FsaAction::kCommand, Command::kCmdShutdown};
      }
      // Both kOperationEnabled and kQuickStopActive are reached through it.
      return {FsaAction::kCommand, Command::kCmdEnableOperation};

    case State::kOperationEnabled:
      if (target == State::kSwitchOnDisabled) {
        return {FsaAction::kCommand, Command::kCmdDisableVoltage};
      }
      if (target == State::kReadyToSwitchOn) {
        return {FsaAction::kCommand, Command::kCmdShutdown};
      }
      if (target == State::kSwitchedOn) {
        return {FsaAction::kCommand, Command::kCmdSwitchOn};
      }
      return {FsaAction::kCommand, Command::kCmdQuickStop};

    case State::kQuickStopActive:
      if (target == State::kOperationEnabled) {
        return allowQuickStopOverride
                   ? FsaTransition{FsaAction::kCommand, Command::kCmdEnableOperation}
                   : FsaTransition{FsaAction::kUnreachable, 0};
      }
      // Everything else leaves through SwitchOnDisabled, which then continues the walk.
      return {FsaAction::kCommand, Command::kCmdDisableVoltage};
  }
  return {FsaAction::kUnreachable, 0};
}

}  // namespace mm::node::cia402
