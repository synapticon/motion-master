#pragma once

#include <chrono>
#include <expected>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "node/cia402.h"
#include "node/profile_device.h"

namespace mm::node {

/// @brief A snapshot of a CiA402 drive's control state — the values a control UI needs to show at a
///        glance: the decoded state machine state, the raw status/control words, the active
///        operation mode (display object 0x6061), and the setpoint currently commanded for that
///        mode. Read in one shot by @c Cia402Drive::readStatus.
struct Cia402Status {
  cia402::State state{};                           ///< Decoded from the statusword (0x6041).
  uint16_t statusword = 0;                         ///< Raw statusword (0x6041).
  uint16_t controlword = 0;                        ///< Last-commanded controlword (0x6040).
  cia402::OperationMode modeOfOperationDisplay{};  ///< Active operation mode (0x6061).
  /// The setpoint object for the active mode — target position 0x607A (PP/CSP), velocity 0x60FF
  /// (PV/CSV), or torque 0x6071 (PT/CST), widened to int32. 0 only when the active mode has no
  /// linear setpoint (NoMode / Homing). Lets a UI seed its target input from the drive.
  int32_t target = 0;
};

/// @brief Serialises a @c Cia402Status. Emits `state`/`modeName` as human-readable strings
///        alongside the numeric `statusword`, `controlword`, and `modeOfOperation`.
void to_json(nlohmann::json& j, const Cia402Status& s);

/// @brief Position range limit (0x607B) — the range position values wrap around in.
struct PositionRangeLimit {
  int32_t min{};  ///< 0x607B:01 — min position range limit.
  int32_t max{};  ///< 0x607B:02 — max position range limit.
};

/// @brief Software position limit (0x607D) — the absolute end stops enforced in software.
struct SoftwarePositionLimit {
  int32_t min{};  ///< 0x607D:01 — min position limit.
  int32_t max{};  ///< 0x607D:02 — max position limit.
};

/// @brief Gear ratio (0x6091) — motor revolutions per driving shaft revolutions.
struct GearRatio {
  uint32_t motorRevolutions{};  ///< 0x6091:01 — numerator.
  uint32_t shaftRevolutions{};  ///< 0x6091:02 — denominator.
};

/// @brief Feed constant (0x6092) — feed (position units) per driving shaft revolutions.
struct FeedConstant {
  uint32_t feed{};              ///< 0x6092:01 — numerator.
  uint32_t shaftRevolutions{};  ///< 0x6092:02 — denominator.
};

/// @brief Homing speeds (0x6099) — the two velocities of a homing run.
struct HomingSpeeds {
  uint32_t switchSearch{};  ///< 0x6099:01 — speed during search for the reference switch.
  uint32_t zeroSearch{};    ///< 0x6099:02 — speed during search for the zero position.
};

/// @brief Digital outputs (0x60FE) — commanded output levels gated by an enable mask.
struct DigitalOutputs {
  uint32_t physicalOutputs{};  ///< 0x60FE:01 — commanded output bit levels.
  uint32_t bitMask{};          ///< 0x60FE:02 — 1 = the corresponding output bit is driven.
};

/// @brief High-level state-machine actions a client can command (the named transitions a UI
///        exposes as buttons). Distinct from @c cia402::Command (raw controlword bit patterns).
enum class Cia402Command { kEnable, kDisable, kQuickStop, kFaultReset };

/// @brief Parses a command token ("enable" / "disable" / "quickStop" / "faultReset").
/// @return The command, or @c std::nullopt if the token is unrecognised.
std::optional<Cia402Command> parseCia402Command(std::string_view token);

/// @brief Which cyclic setpoint a target write addresses. Exactly one is active at a time — the
///        one matching the drive's operation mode (position for PP/CSP, velocity for PV/CSV,
///        torque for PT/CST) — so a target write names its kind rather than setting all three.
enum class Cia402TargetKind { kPosition, kVelocity, kTorque };

/// @brief Parses a target-kind token ("position" / "velocity" / "torque").
/// @return The kind, or @c std::nullopt if the token is unrecognised.
std::optional<Cia402TargetKind> parseCia402TargetKind(std::string_view token);

/// @brief Borrowed view of a CiA402 drive — the device control state machine, operation modes,
///        and the standard cyclic setpoints, expressed over a @c Device's parameter access.
///
/// Every method routes through @c Device::readValue / @c writeValue, so it transparently uses
/// the live PDO image while the device is exchanging (SAFE-OP/OP) and falls back to SDO
/// otherwise — the caller never chooses the transport. The view is stateless (see
/// @c ProfileDevice): the drive's actual state is its statusword, re-read on every query.
///
/// Construct via @c createCia402Drive, which validates that the device actually implements the
/// profile before binding the view. The bare constructor is unchecked — for tests or
/// already-validated paths.
///
/// @note Single-shot, here-and-now operations only. A long-running procedure must re-resolve its
///       @c Device per step rather than cache this view across a bus rescan: command-and-wait
///       procedures (homing runs, calibration, auto-tuning) run off-RT on a background thread,
///       while cycle-locked setpoint generation runs in an RT @c CyclicTask. Either way the view
///       is rebound per step/cycle, never held in a long-lived instance.
class Cia402Drive : public ProfileDevice {
 public:
  /// @brief Binds an (unchecked) CiA402 view to @p device. Prefer @c createCia402Drive.
  explicit Cia402Drive(Device& device) : ProfileDevice(device) {}

  /// @brief Reads and decodes the current state machine state (statusword 0x6041).
  std::expected<cia402::State, std::string> state() const;

  /// @brief Reads the raw statusword (0x6041).
  std::expected<uint16_t, std::string> statusword() const;

  /// @brief Reads the last-commanded controlword (0x6040).
  std::expected<uint16_t, std::string> controlword() const;

  /// @brief Writes the controlword (0x6040) verbatim.
  ///
  /// Sets every bit as given — use when composing a mode-specific controlword (e.g. the
  /// homing-start or new-setpoint bits). For pure state-machine transitions prefer the named
  /// transition methods, which preserve the mode-specific and halt bits.
  std::expected<void, std::string> setControlword(uint16_t value);

  /// @brief Reads the active operation mode (display object 0x6061).
  std::expected<cia402::OperationMode, std::string> operationMode() const;

  /// @brief Requests an operation mode (0x6060). The drive reflects it in 0x6061 once accepted.
  std::expected<void, std::string> setOperationMode(cia402::OperationMode mode);

  /// @brief Reads the requested operation mode (0x6060) as its raw value.
  ///
  /// **The raw pair exists because the enum cannot hold every mode a drive has.** 0x6060 is an
  /// INTEGER8 whose negative half the profile leaves entirely to the vendor, so a drive sitting in
  /// one of those — a SOMANET in diagnostics (-2) — has a mode that @c cia402::OperationMode has no
  /// name for and never will. Use these when the value is being *carried* (saved to put back, taken
  /// from a client, written straight through) and the typed pair when it is being reasoned about.
  std::expected<int8_t, std::string> operationModeValue() const;

  /// @brief Reads the *active* operation mode (0x6061) as its raw value — the display counterpart
  ///        of @c operationModeValue, and the one that says what the drive actually took.
  std::expected<int8_t, std::string> operationModeValueDisplay() const;

  /// @brief Writes a raw operation-mode value to 0x6060 — the counterpart of @c operationModeValue.
  ///
  /// Nothing is checked: whether the drive has this mode is the drive's answer, given by whether
  /// 0x6061 comes to reflect it. Use @c applyOperationMode to wait for that answer.
  std::expected<void, std::string> setOperationModeValue(int8_t mode);

  /// @brief Writes @p mode to 0x6060 and waits until the drive reports it active in 0x6061.
  ///
  /// **A drive is free to ignore a mode request, and refusing is a successful write followed by
  /// nothing happening** — which is indistinguishable from success unless someone looks. SOMANET
  /// firmware refuses a change to a non-"dynamic" mode while in Operation Enabled, and refuses
  /// outright any mode its @c opmode_update does not list (deprecated system identification, -4).
  /// Both raise a warning the drive keeps to itself and answer the SDO with success.
  ///
  /// Mode 0 is exempt: it requests *no* mode, so there is nothing to arrive at, and a display
  /// object still showing the previous mode is not a refusal.
  ///
  /// @param mode     The 0x6060 value to request.
  /// @param timeout  How long to give 0x6061 to catch up.
  /// @return Void once the drive reports @p mode active, otherwise why not — naming the mode it is
  ///         in instead, with its 0x603F error code when that can be read.
  std::expected<void, std::string> applyOperationMode(
      int8_t mode, std::chrono::milliseconds timeout = std::chrono::milliseconds(200));

  /// @brief Reads state, statusword, controlword, and the active mode in one shot.
  std::expected<Cia402Status, std::string> readStatus() const;

  // --- State-machine transitions ------------------------------------------------------------
  // Each issues exactly one controlword edge via a read-modify-write that touches only the
  // command bits (cia402::kCommandMask), preserving mode-specific / halt / manufacturer bits.
  // They command a transition; they do not wait for the drive to reach the target state.

  /// @brief Shutdown: → ReadyToSwitchOn.
  std::expected<void, std::string> shutdown();

  /// @brief Switch on: ReadyToSwitchOn → SwitchedOn (also disables operation from enabled).
  std::expected<void, std::string> switchOn();

  /// @brief Enable operation: SwitchedOn → OperationEnabled.
  std::expected<void, std::string> enableOperation();

  /// @brief Disable voltage: → SwitchOnDisabled.
  std::expected<void, std::string> disableVoltage();

  /// @brief Quick stop: → QuickStopActive.
  std::expected<void, std::string> quickStop();

  /// @brief Fault reset: asserts the rising edge of controlword bit 7 to clear a latched fault.
  ///
  /// Sets bit 7 (preserving the other bits); the drive latches the reset on the 0→1 transition. It
  /// does not clear bit 7 — the next state-machine command does that via @c kCommandMask, re-arming
  /// the reset. (Clearing it here would be lost on the PDO path, where a set+clear issued in one
  /// call collapses to the last staged value and the drive never sees the edge.)
  std::expected<void, std::string> faultReset();

  // --- Convenience walks ---------------------------------------------------------------------

  /// @brief Drives the state machine to @p target, walking every intermediate transition.
  ///
  /// Reads the state, issues the one transition that advances toward @p target
  /// (@c cia402::nextFsaTransition), and repeats until the drive arrives or @p timeout elapses. The
  /// state is re-read every iteration rather than assumed, so a drive that needs an extra cycle,
  /// or that moves on its own — out of @c kFaultReactionActive, or out of @c kQuickStopActive once
  /// its quick-stop ramp finishes — is simply followed.
  ///
  /// Intended for the control-plane (HTTP) thread: it sleeps between polls and must never be
  /// called from the RT loop. **Process data must be exchanging** for the drive to advance at all
  /// — the statusword has to update between polls, and the firmware additionally refuses
  /// @c kOperationEnabled unless master communication is live.
  ///
  /// Three outcomes are worth telling apart in the error, and this does:
  ///   - a **fault that will not clear** — the reset was issued and the drive stayed in
  ///     @c kFault, which the firmware does when the cause is still present. The drive's own error
  ///     report is attached.
  ///   - an **unreachable target** — asking for a state no command enters, or for
  ///     @c kOperationEnabled out of a quick stop without @p allowQuickStopOverride.
  ///   - a plain **timeout**, naming the state it was stuck in.
  ///
  /// **Targeting @c kQuickStopActive may legitimately end in @c kSwitchOnDisabled.** Quick stop
  /// option code 0x605A decides whether the drive stays in that state or passes through it, and
  /// with codes 0-4 it passes through — so this reads the code and, when it says the drive does not
  /// hold, accepts Switch On Disabled as arrival. Without that the walk would see the drive already
  /// past its target, climb back up, quick-stop again, and loop until the timeout.
  ///
  /// @param target                 Where to bring the drive. Must be @c cia402::isCommandableState.
  /// @param timeout                Maximum time to spend walking.
  /// @param allowQuickStopOverride Whether transition 16 may be used to leave @c kQuickStopActive
  ///                               upward. **False for anything that did not explicitly ask for
  ///                               it**: a quick stop is a deliberate act, and a procedure that
  ///                               merely wants an enabled drive must not undo one.
  /// @return Void once @p target is reached, otherwise why not.
  std::expected<void, std::string> transitionToState(
      cia402::State target, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000),
      bool allowQuickStopOverride = false);

  /// @brief Drives the state machine to OperationEnabled.
  ///
  /// @c transitionToState with that target, and **deliberately without the quick-stop override**:
  /// this is what procedures call to prepare a drive, and one that overrode a quick stop on the
  /// way to a measurement would be undoing a stop somebody asked for.
  ///
  /// @param timeout  Maximum time to wait for OperationEnabled.
  /// @return Void once OperationEnabled is reached, or an error string on a bus failure, a
  ///         fault that will not clear, or timeout.
  std::expected<void, std::string> enable(
      std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));

  /// @brief Brings the drive to SwitchOnDisabled (disable voltage). Single transition.
  std::expected<void, std::string> disable();

  // --- Cyclic setpoints (typed convenience over the standard objects) ------------------------

  /// @brief Reads the last-commanded target position (0x607A, INTEGER32).
  std::expected<int32_t, std::string> targetPosition() const;

  /// @brief Writes target position (0x607A, INTEGER32) — CSP / PP.
  std::expected<void, std::string> setTargetPosition(int32_t counts);

  /// @brief Reads the last-commanded target velocity (0x60FF, INTEGER32).
  std::expected<int32_t, std::string> targetVelocity() const;

  /// @brief Writes target velocity (0x60FF, INTEGER32) — CSV / PV.
  std::expected<void, std::string> setTargetVelocity(int32_t value);

  /// @brief Reads the last-commanded target torque (0x6071, INTEGER16, per-mille of rated).
  std::expected<int16_t, std::string> targetTorque() const;

  /// @brief Writes target torque (0x6071, INTEGER16, per-mille of rated) — CST / PT.
  std::expected<void, std::string> setTargetTorque(int16_t perMille);

  /// @brief Reads actual position (0x6064, INTEGER32).
  std::expected<int32_t, std::string> positionActualValue() const;

  /// @brief Reads actual velocity (0x606C, INTEGER32).
  std::expected<int32_t, std::string> velocityActualValue() const;

  /// @brief Reads actual torque (0x6077, INTEGER16, per-mille of rated).
  std::expected<int16_t, std::string> torqueActualValue() const;

  // --- Profile objects (0x603F..0x6502) --------------------------------------------------------
  // Flag-aware accessors over the remaining standard drive-profile objects, ordered by
  // index/subindex. Every getter re-reads the device on each call (these objects are writable or
  // volatile) except supportedDriveModes(), a constant capability field cached after the first
  // read.

  /// @brief Reads the error code (0x603F, UNSIGNED16) — the code of the last drive fault.
  std::expected<uint16_t, std::string> errorCode() const;

  /// @brief Reads the quick stop option code (0x605A, INTEGER16).
  std::expected<int16_t, std::string> quickStopOptionCode() const;

  /// @brief Writes the quick stop option code (0x605A, INTEGER16) — the quick-stop reaction.
  std::expected<void, std::string> setQuickStopOptionCode(int16_t value);

  /// @brief Reads the position demand value (0x6062, INTEGER32) — trajectory generator output.
  std::expected<int32_t, std::string> positionDemandValue() const;

  /// @brief Reads the following error window (0x6065, UNSIGNED32).
  std::expected<uint32_t, std::string> followingErrorWindow() const;

  /// @brief Writes the following error window (0x6065, UNSIGNED32).
  std::expected<void, std::string> setFollowingErrorWindow(uint32_t value);

  /// @brief Reads the following error time out (0x6066, UNSIGNED16, ms).
  std::expected<uint16_t, std::string> followingErrorTimeout() const;

  /// @brief Writes the following error time out (0x6066, UNSIGNED16, ms).
  std::expected<void, std::string> setFollowingErrorTimeout(uint16_t value);

  /// @brief Reads the position window (0x6067, UNSIGNED32) — the target-reached tolerance.
  std::expected<uint32_t, std::string> positionWindow() const;

  /// @brief Writes the position window (0x6067, UNSIGNED32).
  std::expected<void, std::string> setPositionWindow(uint32_t value);

  /// @brief Reads the position window time (0x6068, UNSIGNED16, ms).
  std::expected<uint16_t, std::string> positionWindowTime() const;

  /// @brief Writes the position window time (0x6068, UNSIGNED16, ms).
  std::expected<void, std::string> setPositionWindowTime(uint16_t value);

  /// @brief Reads the velocity demand value (0x606B, INTEGER32) — ramp generator output.
  std::expected<int32_t, std::string> velocityDemandValue() const;

  /// @brief Reads the velocity window (0x606D, UNSIGNED16) — the target-reached tolerance.
  std::expected<uint16_t, std::string> velocityWindow() const;

  /// @brief Writes the velocity window (0x606D, UNSIGNED16).
  std::expected<void, std::string> setVelocityWindow(uint16_t value);

  /// @brief Reads the velocity window time (0x606E, UNSIGNED16, ms).
  std::expected<uint16_t, std::string> velocityWindowTime() const;

  /// @brief Writes the velocity window time (0x606E, UNSIGNED16, ms).
  std::expected<void, std::string> setVelocityWindowTime(uint16_t value);

  /// @brief Reads the velocity threshold (0x606F, UNSIGNED16) — the standstill threshold.
  std::expected<uint16_t, std::string> velocityThreshold() const;

  /// @brief Writes the velocity threshold (0x606F, UNSIGNED16).
  std::expected<void, std::string> setVelocityThreshold(uint16_t value);

  /// @brief Reads the velocity threshold time (0x6070, UNSIGNED16, ms).
  std::expected<uint16_t, std::string> velocityThresholdTime() const;

  /// @brief Writes the velocity threshold time (0x6070, UNSIGNED16, ms).
  std::expected<void, std::string> setVelocityThresholdTime(uint16_t value);

  /// @brief Reads max torque (0x6072, UNSIGNED16, per-mille of rated).
  std::expected<uint16_t, std::string> maxTorque() const;

  /// @brief Writes max torque (0x6072, UNSIGNED16, per-mille of rated).
  std::expected<void, std::string> setMaxTorque(uint16_t perMille);

  /// @brief Reads max current (0x6073, UNSIGNED16, per-mille of rated).
  std::expected<uint16_t, std::string> maxCurrent() const;

  /// @brief Writes max current (0x6073, UNSIGNED16, per-mille of rated).
  std::expected<void, std::string> setMaxCurrent(uint16_t perMille);

  /// @brief Reads the torque demand (0x6074, INTEGER16) — control loop output.
  std::expected<int16_t, std::string> torqueDemand() const;

  /// @brief Reads the motor rated current (0x6075, UNSIGNED32, mA).
  std::expected<uint32_t, std::string> motorRatedCurrent() const;

  /// @brief Writes the motor rated current (0x6075, UNSIGNED32, mA).
  std::expected<void, std::string> setMotorRatedCurrent(uint32_t milliamps);

  /// @brief Reads the motor rated torque (0x6076, UNSIGNED32, mNm).
  std::expected<uint32_t, std::string> motorRatedTorque() const;

  /// @brief Writes the motor rated torque (0x6076, UNSIGNED32, mNm).
  std::expected<void, std::string> setMotorRatedTorque(uint32_t millinewtonMetres);

  /// @brief Reads the DC link circuit voltage (0x6079, UNSIGNED32, mV).
  std::expected<uint32_t, std::string> dcLinkCircuitVoltage() const;

  /// @brief Reads the position range limit (0x607B) — both sub-entries.
  std::expected<PositionRangeLimit, std::string> positionRangeLimit() const;

  /// @brief Writes the position range limit (0x607B) — min then max; aborts on first failure.
  std::expected<void, std::string> setPositionRangeLimit(const PositionRangeLimit& limit);

  /// @brief Reads the home offset (0x607C, INTEGER32).
  std::expected<int32_t, std::string> homeOffset() const;

  /// @brief Writes the home offset (0x607C, INTEGER32).
  std::expected<void, std::string> setHomeOffset(int32_t value);

  /// @brief Reads the software position limit (0x607D) — both sub-entries.
  std::expected<SoftwarePositionLimit, std::string> softwarePositionLimit() const;

  /// @brief Writes the software position limit (0x607D) — min then max; aborts on first failure.
  std::expected<void, std::string> setSoftwarePositionLimit(const SoftwarePositionLimit& limit);

  /// @brief Reads the polarity (0x607E, UNSIGNED8) — position/velocity inversion bits.
  std::expected<uint8_t, std::string> polarity() const;

  /// @brief Writes the polarity (0x607E, UNSIGNED8).
  std::expected<void, std::string> setPolarity(uint8_t value);

  /// @brief Reads the max motor speed (0x6080, UNSIGNED32).
  std::expected<uint32_t, std::string> maxMotorSpeed() const;

  /// @brief Writes the max motor speed (0x6080, UNSIGNED32).
  std::expected<void, std::string> setMaxMotorSpeed(uint32_t value);

  /// @brief Reads the profile velocity (0x6081, UNSIGNED32) — PP cruise velocity.
  std::expected<uint32_t, std::string> profileVelocity() const;

  /// @brief Writes the profile velocity (0x6081, UNSIGNED32).
  std::expected<void, std::string> setProfileVelocity(uint32_t value);

  /// @brief Reads the profile acceleration (0x6083, UNSIGNED32).
  std::expected<uint32_t, std::string> profileAcceleration() const;

  /// @brief Writes the profile acceleration (0x6083, UNSIGNED32).
  std::expected<void, std::string> setProfileAcceleration(uint32_t value);

  /// @brief Reads the profile deceleration (0x6084, UNSIGNED32).
  std::expected<uint32_t, std::string> profileDeceleration() const;

  /// @brief Writes the profile deceleration (0x6084, UNSIGNED32).
  std::expected<void, std::string> setProfileDeceleration(uint32_t value);

  /// @brief Reads the quick stop deceleration (0x6085, UNSIGNED32).
  std::expected<uint32_t, std::string> quickStopDeceleration() const;

  /// @brief Writes the quick stop deceleration (0x6085, UNSIGNED32).
  std::expected<void, std::string> setQuickStopDeceleration(uint32_t value);

  /// @brief Reads the motion profile type (0x6086, INTEGER16).
  std::expected<int16_t, std::string> motionProfileType() const;

  /// @brief Writes the motion profile type (0x6086, INTEGER16).
  std::expected<void, std::string> setMotionProfileType(int16_t value);

  /// @brief Reads the torque slope (0x6087, UNSIGNED32) — PT torque ramp rate.
  std::expected<uint32_t, std::string> torqueSlope() const;

  /// @brief Writes the torque slope (0x6087, UNSIGNED32).
  std::expected<void, std::string> setTorqueSlope(uint32_t value);

  /// @brief Reads the torque profile type (0x6088, INTEGER16).
  std::expected<int16_t, std::string> torqueProfileType() const;

  /// @brief Writes the torque profile type (0x6088, INTEGER16).
  std::expected<void, std::string> setTorqueProfileType(int16_t value);

  /// @brief Reads the gear ratio (0x6091) — both sub-entries.
  std::expected<GearRatio, std::string> gearRatio() const;

  /// @brief Writes the gear ratio (0x6091) — motor then shaft revolutions; aborts on first
  ///        failure.
  std::expected<void, std::string> setGearRatio(const GearRatio& ratio);

  /// @brief Reads the feed constant (0x6092) — both sub-entries.
  std::expected<FeedConstant, std::string> feedConstant() const;

  /// @brief Writes the feed constant (0x6092) — feed then shaft revolutions; aborts on first
  ///        failure.
  std::expected<void, std::string> setFeedConstant(const FeedConstant& constant);

  /// @brief Reads the homing method (0x6098, INTEGER8).
  std::expected<int8_t, std::string> homingMethod() const;

  /// @brief Writes the homing method (0x6098, INTEGER8).
  std::expected<void, std::string> setHomingMethod(int8_t method);

  /// @brief Reads the homing speeds (0x6099) — both sub-entries.
  std::expected<HomingSpeeds, std::string> homingSpeeds() const;

  /// @brief Writes the homing speeds (0x6099) — switch then zero search; aborts on first failure.
  std::expected<void, std::string> setHomingSpeeds(const HomingSpeeds& speeds);

  /// @brief Reads the homing acceleration (0x609A, UNSIGNED32).
  std::expected<uint32_t, std::string> homingAcceleration() const;

  /// @brief Writes the homing acceleration (0x609A, UNSIGNED32).
  std::expected<void, std::string> setHomingAcceleration(uint32_t value);

  /// @brief Reads the SI unit velocity (0x60A9, UNSIGNED32) — the unit code of velocity objects.
  std::expected<uint32_t, std::string> siUnitVelocity() const;

  /// @brief Writes the SI unit velocity (0x60A9, UNSIGNED32).
  std::expected<void, std::string> setSiUnitVelocity(uint32_t value);

  /// @brief Reads the velocity offset (0x60B1, INTEGER32) — CSP/CSV velocity feed-forward.
  std::expected<int32_t, std::string> velocityOffset() const;

  /// @brief Writes the velocity offset (0x60B1, INTEGER32).
  std::expected<void, std::string> setVelocityOffset(int32_t value);

  /// @brief Reads the torque offset (0x60B2, INTEGER16) — torque feed-forward.
  std::expected<int16_t, std::string> torqueOffset() const;

  /// @brief Writes the torque offset (0x60B2, INTEGER16).
  std::expected<void, std::string> setTorqueOffset(int16_t value);

  /// @brief Reads the touch probe function (0x60B8, UNSIGNED16) — arm/config bits.
  std::expected<uint16_t, std::string> touchProbeFunction() const;

  /// @brief Writes the touch probe function (0x60B8, UNSIGNED16).
  std::expected<void, std::string> setTouchProbeFunction(uint16_t value);

  /// @brief Reads the touch probe status (0x60B9, UNSIGNED16) — latch status bits.
  std::expected<uint16_t, std::string> touchProbeStatus() const;

  /// @brief Reads the position latched at touch probe 1's rising edge (0x60BA, INTEGER32).
  std::expected<int32_t, std::string> touchProbe1PositiveEdge() const;

  /// @brief Reads the position latched at touch probe 1's falling edge (0x60BB, INTEGER32).
  std::expected<int32_t, std::string> touchProbe1NegativeEdge() const;

  /// @brief Reads the time stamp of touch probe 1's rising edge (0x60D1, UNSIGNED32).
  std::expected<uint32_t, std::string> touchProbeTimeStamp1PositiveValue() const;

  /// @brief Reads the time stamp of touch probe 1's falling edge (0x60D2, UNSIGNED32).
  std::expected<uint32_t, std::string> touchProbeTimeStamp1NegativeValue() const;

  /// @brief Reads the positioning option code (0x60F2, UNSIGNED16) — PP behaviour options.
  std::expected<uint16_t, std::string> positioningOptionCode() const;

  /// @brief Writes the positioning option code (0x60F2, UNSIGNED16).
  std::expected<void, std::string> setPositioningOptionCode(uint16_t value);

  /// @brief Reads the following error actual value (0x60F4, INTEGER32).
  std::expected<int32_t, std::string> followingErrorActualValue() const;

  /// @brief Reads the control effort (0x60FA, INTEGER32) — position loop output.
  std::expected<int32_t, std::string> controlEffort() const;

  /// @brief Reads the position demand internal value (0x60FC, INTEGER32).
  std::expected<int32_t, std::string> positionDemandInternalValue() const;

  /// @brief Reads the digital inputs (0x60FD, UNSIGNED32) — input bit field.
  std::expected<uint32_t, std::string> digitalInputs() const;

  /// @brief Reads the digital outputs (0x60FE) — both sub-entries.
  std::expected<DigitalOutputs, std::string> digitalOutputs() const;

  /// @brief Writes the digital outputs (0x60FE) — levels first (inert while masked off), then the
  ///        enable mask, so a newly enabled output comes up with its commanded level; aborts on
  ///        first failure.
  std::expected<void, std::string> setDigitalOutputs(const DigitalOutputs& outputs);

  /// @brief Reads the supported drive modes (0x6502, UNSIGNED32) — the capability bit field.
  ///        Constant, so it is cached after the first read.
  std::expected<uint32_t, std::string> supportedDriveModes() const;

 private:
  /// @brief Read-modify-write of the controlword that sets only the @c kCommandMask bits to
  ///        @p command, preserving every other bit.
  std::expected<void, std::string> applyCommand(uint16_t command);
};

/// @brief Validates that @p device implements the CiA402 profile, then binds a view to it.
///
/// The check is offline-safe: it requires the controlword (0x6040) and statusword (0x6041)
/// objects to be present in the device's parameter map (populated by
/// @c Device::initializeParameters). No bus I/O is performed.
///
/// @param device  Device to view. The reference must outlive the returned view.
/// @return A @c Cia402Drive bound to @p device, or an error string if it is not a CiA402 drive
///         (or its parameters are not initialised).
std::expected<Cia402Drive, std::string> createCia402Drive(Device& device);

}  // namespace mm::node
