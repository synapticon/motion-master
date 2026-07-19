#include "node/cia402_drive.h"

#include <chrono>
#include <format>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace mm::node {

void to_json(nlohmann::json& j, const Cia402Status& s) {
  j = nlohmann::json{
      {"state", cia402::toString(s.state)},
      {"statusword", s.statusword},
      {"controlword", s.controlword},
      {"modeOfOperation", static_cast<int>(s.modeOfOperationDisplay)},
      {"modeName", cia402::toString(s.modeOfOperationDisplay)},
      // 0 only when the active mode has no linear setpoint (NoMode / Homing).
      {"target", s.target},
  };
}

std::optional<Cia402Command> parseCia402Command(std::string_view token) {
  if (token == "enable") {
    return Cia402Command::kEnable;
  }
  if (token == "disable") {
    return Cia402Command::kDisable;
  }
  if (token == "quickStop") {
    return Cia402Command::kQuickStop;
  }
  if (token == "faultReset") {
    return Cia402Command::kFaultReset;
  }
  return std::nullopt;
}

std::optional<Cia402TargetKind> parseCia402TargetKind(std::string_view token) {
  if (token == "position") {
    return Cia402TargetKind::kPosition;
  }
  if (token == "velocity") {
    return Cia402TargetKind::kVelocity;
  }
  if (token == "torque") {
    return Cia402TargetKind::kTorque;
  }
  return std::nullopt;
}

namespace {

using cia402::Command;
using cia402::Object;
using cia402::State;

// Poll cadence for enable()'s state-machine walk. Decoupled from the (configurable) RT period: it
// only bounds the extra latency added on top of each transition's bus round-trip. Each observed
// transition already costs ~2 RT cycles (stage output → RT sends → drive reacts → RT receives new
// statusword), so polling faster just re-reads an unchanged statusword; 1 ms keeps the added
// latency negligible on a fast bus and small on a slow one, without busy-spinning the control
// plane. Not on the RT path — enable() runs on the HTTP thread.
constexpr auto kPollStep = std::chrono::milliseconds(1);

}  // namespace

std::expected<uint16_t, std::string> Cia402Drive::statusword() const {
  return device_.readValue<uint16_t>(Object::kStatusword, 0);
}

std::expected<uint16_t, std::string> Cia402Drive::controlword() const {
  return device_.readValue<uint16_t>(Object::kControlword, 0);
}

std::expected<void, std::string> Cia402Drive::setControlword(uint16_t value) {
  return device_.writeValue(Object::kControlword, 0, value);
}

std::expected<State, std::string> Cia402Drive::state() const {
  return statusword().transform(cia402::decodeState);
}

std::expected<cia402::OperationMode, std::string> Cia402Drive::operationMode() const {
  auto v = device_.readValue<int8_t>(Object::kModeOfOperationDisplay, 0);
  if (!v) {
    return std::unexpected(v.error());
  }
  return static_cast<cia402::OperationMode>(*v);
}

std::expected<void, std::string> Cia402Drive::setOperationMode(cia402::OperationMode mode) {
  return device_.writeValue(Object::kModeOfOperation, 0, static_cast<int8_t>(mode));
}

std::expected<Cia402Status, std::string> Cia402Drive::readStatus() const {
  auto sw = statusword();
  if (!sw) {
    return std::unexpected(sw.error());
  }
  auto cw = controlword();
  if (!cw) {
    return std::unexpected(cw.error());
  }
  auto mode = operationMode();
  if (!mode) {
    return std::unexpected(mode.error());
  }
  // Read the setpoint the active mode actually acts on so a UI can seed its target input from the
  // drive; 0 for modes with no linear setpoint. Torque is INTEGER16, the others INTEGER32.
  int32_t target = 0;
  switch (*mode) {
    case cia402::OperationMode::kCyclicSyncPosition:
    case cia402::OperationMode::kProfilePosition:
      if (auto v = device_.readValue<int32_t>(cia402::Object::kTargetPosition, 0)) {
        target = *v;
      }
      break;
    case cia402::OperationMode::kCyclicSyncVelocity:
    case cia402::OperationMode::kProfileVelocity:
      if (auto v = device_.readValue<int32_t>(cia402::Object::kTargetVelocity, 0)) {
        target = *v;
      }
      break;
    case cia402::OperationMode::kCyclicSyncTorque:
    case cia402::OperationMode::kProfileTorque:
      if (auto v = device_.readValue<int16_t>(cia402::Object::kTargetTorque, 0)) {
        target = *v;
      }
      break;
    case cia402::OperationMode::kNoMode:
    case cia402::OperationMode::kHoming:
      break;
  }
  return Cia402Status{cia402::decodeState(*sw), *sw, *cw, *mode, target};
}

std::expected<void, std::string> Cia402Drive::applyCommand(uint16_t command) {
  auto current = controlword();
  if (!current) {
    return std::unexpected(current.error());
  }
  const uint16_t next =
      static_cast<uint16_t>((*current & ~cia402::kCommandMask) | (command & cia402::kCommandMask));
  return setControlword(next);
}

std::expected<void, std::string> Cia402Drive::shutdown() {
  return applyCommand(Command::kCmdShutdown);
}

std::expected<void, std::string> Cia402Drive::switchOn() {
  return applyCommand(Command::kCmdSwitchOn);
}

std::expected<void, std::string> Cia402Drive::enableOperation() {
  return applyCommand(Command::kCmdEnableOperation);
}

std::expected<void, std::string> Cia402Drive::disableVoltage() {
  return applyCommand(Command::kCmdDisableVoltage);
}

std::expected<void, std::string> Cia402Drive::quickStop() {
  return applyCommand(Command::kCmdQuickStop);
}

std::expected<void, std::string> Cia402Drive::faultReset() {
  // Fault reset (CiA402 transition 15; ETG.6010 5.2, Table 3) is triggered by a *rising* edge of
  // controlword bit 7 — the drive latches on the 0->1 transition. Assert the edge by setting bit 7,
  // preserving every other bit, and do NOT clear it in the same call: on the PDO path each write
  // only stages the value into the output slot, and the RT loop composes one frame per cycle, so a
  // set+clear issued back to back collapses to last-writer-wins — the drive would see only the
  // cleared value and never the edge. Bit 7 is driven low again by the next state-machine command
  // (they all route through applyCommand, whose kCommandMask covers bit 7), which re-arms the reset
  // for a later fault. This assumes bit 7 is currently low, which holds after any normal command (a
  // drive faults from OperationEnabled with controlword 0x000F).
  auto current = controlword();
  if (!current) {
    return std::unexpected(current.error());
  }
  return setControlword(static_cast<uint16_t>(*current | Command::kCmdFaultReset));
}

std::expected<void, std::string> Cia402Drive::disable() { return disableVoltage(); }

std::expected<void, std::string> Cia402Drive::enable(std::chrono::milliseconds timeout) {
  // Walk the state machine toward OperationEnabled, issuing one transition per observed state.
  // We re-read the state each iteration rather than assuming the previous command took effect,
  // so a drive that needs an extra cycle (or rejects a step) is handled by simply re-issuing.
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    auto st = state();
    if (!st) {
      return std::unexpected(st.error());
    }
    switch (*st) {
      case State::kOperationEnabled:
        return {};
      case State::kFault:
        if (auto r = faultReset(); !r) {
          return r;
        }
        break;
      case State::kFaultReactionActive:
        // Wait for the drive to finish reacting and settle into Fault, then reset.
        break;
      case State::kSwitchOnDisabled:
        if (auto r = shutdown(); !r) {
          return r;
        }
        break;
      case State::kReadyToSwitchOn:
        if (auto r = switchOn(); !r) {
          return r;
        }
        break;
      case State::kSwitchedOn:
        if (auto r = enableOperation(); !r) {
          return r;
        }
        break;
      case State::kQuickStopActive:
        // Deliberate safety state — do not auto-override it (CiA402 transition 16, enable-operation
        // from quick stop, is "not recommended" per IEC 61800-7-201 Table 26). Just wait: with
        // quick-stop option code 1-4 the drive auto-transitions to SwitchOnDisabled (transition 12)
        // and the next poll continues the walk; with code 5-8 it holds here until the user releases
        // it, and enable() times out.
        break;
      case State::kNotReadyToSwitchOn:
        // Still initialising; wait.
        break;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return std::unexpected(std::format("enable timed out after {} ms in state {}",
                                         timeout.count(), cia402::toString(*st)));
    }
    std::this_thread::sleep_for(kPollStep);
  }
}

std::expected<void, std::string> Cia402Drive::setTargetPosition(int32_t counts) {
  return device_.writeValue(Object::kTargetPosition, 0, counts);
}

std::expected<void, std::string> Cia402Drive::setTargetVelocity(int32_t value) {
  return device_.writeValue(Object::kTargetVelocity, 0, value);
}

std::expected<void, std::string> Cia402Drive::setTargetTorque(int16_t perMille) {
  return device_.writeValue(Object::kTargetTorque, 0, perMille);
}

std::expected<int32_t, std::string> Cia402Drive::positionActualValue() const {
  return device_.readValue<int32_t>(Object::kPositionActualValue, 0);
}

std::expected<int32_t, std::string> Cia402Drive::velocityActualValue() const {
  return device_.readValue<int32_t>(Object::kVelocityActualValue, 0);
}

std::expected<Cia402Drive, std::string> createCia402Drive(Device& device) {
  // Offline-safe discriminator (Device::isCia402): a CiA402 drive exposes both the controlword
  // and statusword in its object dictionary. Presence in the (already-enumerated) parameter map
  // is enough — no bus I/O, so this works whether the device is online or not.
  if (!device.isCia402()) {
    return std::unexpected(
        std::format("device {} is not a CiA402 drive (missing controlword/statusword; "
                    "initializeParameters first?)",
                    device.slavePosition()));
  }
  return Cia402Drive(device);
}

}  // namespace mm::node
