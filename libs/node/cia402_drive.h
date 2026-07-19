#pragma once

#include <chrono>
#include <expected>
#include <string>

#include "node/cia402.h"
#include "node/profile_device.h"

namespace mm::node {

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

  /// @brief Drives the state machine to OperationEnabled, walking every intermediate transition.
  ///
  /// Reads the state, issues the single transition that advances toward OperationEnabled
  /// (resetting first if faulted), and polls until the drive reaches it or @p timeout elapses.
  /// Intended for the control-plane (HTTP) thread — it briefly sleeps between polls and must
  /// never be called from the RT loop. For the drive to actually advance, process-data exchange
  /// must be running (the statusword has to update between polls).
  ///
  /// @param timeout  Maximum time to wait for OperationEnabled.
  /// @return Void once OperationEnabled is reached, or an error string on a bus failure, a
  ///         fault that will not clear, or timeout.
  std::expected<void, std::string> enable(
      std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));

  /// @brief Brings the drive to SwitchOnDisabled (disable voltage). Single transition.
  std::expected<void, std::string> disable();

  // --- Cyclic setpoints (typed convenience over the standard objects) ------------------------

  /// @brief Writes target position (0x607A, INTEGER32) — CSP / PP.
  std::expected<void, std::string> setTargetPosition(int32_t counts);

  /// @brief Writes target velocity (0x60FF, INTEGER32) — CSV / PV.
  std::expected<void, std::string> setTargetVelocity(int32_t value);

  /// @brief Writes target torque (0x6071, INTEGER16, per-mille of rated) — CST / PT.
  std::expected<void, std::string> setTargetTorque(int16_t perMille);

  /// @brief Reads actual position (0x6064, INTEGER32).
  std::expected<int32_t, std::string> positionActualValue() const;

  /// @brief Reads actual velocity (0x606C, INTEGER32).
  std::expected<int32_t, std::string> velocityActualValue() const;

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
///         (or its parameters have not been initialised).
std::expected<Cia402Drive, std::string> createCia402Drive(Device& device);

}  // namespace mm::node
