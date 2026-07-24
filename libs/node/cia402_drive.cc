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

std::expected<int32_t, std::string> Cia402Drive::targetPosition() const {
  return device_.readValue<int32_t>(Object::kTargetPosition, 0);
}

std::expected<void, std::string> Cia402Drive::setTargetPosition(int32_t counts) {
  return device_.writeValue(Object::kTargetPosition, 0, counts);
}

std::expected<int32_t, std::string> Cia402Drive::targetVelocity() const {
  return device_.readValue<int32_t>(Object::kTargetVelocity, 0);
}

std::expected<void, std::string> Cia402Drive::setTargetVelocity(int32_t value) {
  return device_.writeValue(Object::kTargetVelocity, 0, value);
}

std::expected<int16_t, std::string> Cia402Drive::targetTorque() const {
  return device_.readValue<int16_t>(Object::kTargetTorque, 0);
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

std::expected<int16_t, std::string> Cia402Drive::torqueActualValue() const {
  return device_.readValue<int16_t>(Object::kTorqueActualValue, 0);
}

std::expected<uint16_t, std::string> Cia402Drive::errorCode() const {
  return device_.readValue<uint16_t>(Object::kErrorCode, 0);
}

std::expected<int16_t, std::string> Cia402Drive::quickStopOptionCode() const {
  return device_.readValue<int16_t>(Object::kQuickStopOptionCode, 0);
}

std::expected<void, std::string> Cia402Drive::setQuickStopOptionCode(int16_t value) {
  return device_.writeValue(Object::kQuickStopOptionCode, 0, value);
}

std::expected<int32_t, std::string> Cia402Drive::positionDemandValue() const {
  return device_.readValue<int32_t>(Object::kPositionDemandValue, 0);
}

std::expected<uint32_t, std::string> Cia402Drive::followingErrorWindow() const {
  return device_.readValue<uint32_t>(Object::kFollowingErrorWindow, 0);
}

std::expected<void, std::string> Cia402Drive::setFollowingErrorWindow(uint32_t value) {
  return device_.writeValue(Object::kFollowingErrorWindow, 0, value);
}

std::expected<uint16_t, std::string> Cia402Drive::followingErrorTimeout() const {
  return device_.readValue<uint16_t>(Object::kFollowingErrorTimeout, 0);
}

std::expected<void, std::string> Cia402Drive::setFollowingErrorTimeout(uint16_t value) {
  return device_.writeValue(Object::kFollowingErrorTimeout, 0, value);
}

std::expected<uint32_t, std::string> Cia402Drive::positionWindow() const {
  return device_.readValue<uint32_t>(Object::kPositionWindow, 0);
}

std::expected<void, std::string> Cia402Drive::setPositionWindow(uint32_t value) {
  return device_.writeValue(Object::kPositionWindow, 0, value);
}

std::expected<uint16_t, std::string> Cia402Drive::positionWindowTime() const {
  return device_.readValue<uint16_t>(Object::kPositionWindowTime, 0);
}

std::expected<void, std::string> Cia402Drive::setPositionWindowTime(uint16_t value) {
  return device_.writeValue(Object::kPositionWindowTime, 0, value);
}

std::expected<int32_t, std::string> Cia402Drive::velocityDemandValue() const {
  return device_.readValue<int32_t>(Object::kVelocityDemandValue, 0);
}

std::expected<uint16_t, std::string> Cia402Drive::velocityWindow() const {
  return device_.readValue<uint16_t>(Object::kVelocityWindow, 0);
}

std::expected<void, std::string> Cia402Drive::setVelocityWindow(uint16_t value) {
  return device_.writeValue(Object::kVelocityWindow, 0, value);
}

std::expected<uint16_t, std::string> Cia402Drive::velocityWindowTime() const {
  return device_.readValue<uint16_t>(Object::kVelocityWindowTime, 0);
}

std::expected<void, std::string> Cia402Drive::setVelocityWindowTime(uint16_t value) {
  return device_.writeValue(Object::kVelocityWindowTime, 0, value);
}

std::expected<uint16_t, std::string> Cia402Drive::velocityThreshold() const {
  return device_.readValue<uint16_t>(Object::kVelocityThreshold, 0);
}

std::expected<void, std::string> Cia402Drive::setVelocityThreshold(uint16_t value) {
  return device_.writeValue(Object::kVelocityThreshold, 0, value);
}

std::expected<uint16_t, std::string> Cia402Drive::velocityThresholdTime() const {
  return device_.readValue<uint16_t>(Object::kVelocityThresholdTime, 0);
}

std::expected<void, std::string> Cia402Drive::setVelocityThresholdTime(uint16_t value) {
  return device_.writeValue(Object::kVelocityThresholdTime, 0, value);
}

std::expected<uint16_t, std::string> Cia402Drive::maxTorque() const {
  return device_.readValue<uint16_t>(Object::kMaxTorque, 0);
}

std::expected<void, std::string> Cia402Drive::setMaxTorque(uint16_t perMille) {
  return device_.writeValue(Object::kMaxTorque, 0, perMille);
}

std::expected<uint16_t, std::string> Cia402Drive::maxCurrent() const {
  return device_.readValue<uint16_t>(Object::kMaxCurrent, 0);
}

std::expected<void, std::string> Cia402Drive::setMaxCurrent(uint16_t perMille) {
  return device_.writeValue(Object::kMaxCurrent, 0, perMille);
}

std::expected<int16_t, std::string> Cia402Drive::torqueDemand() const {
  return device_.readValue<int16_t>(Object::kTorqueDemand, 0);
}

std::expected<uint32_t, std::string> Cia402Drive::motorRatedCurrent() const {
  return device_.readValue<uint32_t>(Object::kMotorRatedCurrent, 0);
}

std::expected<void, std::string> Cia402Drive::setMotorRatedCurrent(uint32_t milliamps) {
  return device_.writeValue(Object::kMotorRatedCurrent, 0, milliamps);
}

std::expected<uint32_t, std::string> Cia402Drive::motorRatedTorque() const {
  return device_.readValue<uint32_t>(Object::kMotorRatedTorque, 0);
}

std::expected<void, std::string> Cia402Drive::setMotorRatedTorque(uint32_t millinewtonMetres) {
  return device_.writeValue(Object::kMotorRatedTorque, 0, millinewtonMetres);
}

std::expected<uint32_t, std::string> Cia402Drive::dcLinkCircuitVoltage() const {
  return device_.readValue<uint32_t>(Object::kDcLinkCircuitVoltage, 0);
}

std::expected<PositionRangeLimit, std::string> Cia402Drive::positionRangeLimit() const {
  auto object = device_.readObject(Object::kPositionRangeLimit);
  if (!object) {
    return std::unexpected(object.error());
  }
  auto min = object->get<int32_t>(1);
  if (!min) {
    return std::unexpected(min.error());
  }
  auto max = object->get<int32_t>(2);
  if (!max) {
    return std::unexpected(max.error());
  }
  return PositionRangeLimit{*min, *max};
}

std::expected<void, std::string> Cia402Drive::setPositionRangeLimit(
    const PositionRangeLimit& limit) {
  if (auto r = device_.writeValue(Object::kPositionRangeLimit, 1, limit.min); !r) {
    return r;
  }
  return device_.writeValue(Object::kPositionRangeLimit, 2, limit.max);
}

std::expected<int32_t, std::string> Cia402Drive::homeOffset() const {
  return device_.readValue<int32_t>(Object::kHomeOffset, 0);
}

std::expected<void, std::string> Cia402Drive::setHomeOffset(int32_t value) {
  return device_.writeValue(Object::kHomeOffset, 0, value);
}

std::expected<SoftwarePositionLimit, std::string> Cia402Drive::softwarePositionLimit() const {
  auto object = device_.readObject(Object::kSoftwarePositionLimit);
  if (!object) {
    return std::unexpected(object.error());
  }
  auto min = object->get<int32_t>(1);
  if (!min) {
    return std::unexpected(min.error());
  }
  auto max = object->get<int32_t>(2);
  if (!max) {
    return std::unexpected(max.error());
  }
  return SoftwarePositionLimit{*min, *max};
}

std::expected<void, std::string> Cia402Drive::setSoftwarePositionLimit(
    const SoftwarePositionLimit& limit) {
  if (auto r = device_.writeValue(Object::kSoftwarePositionLimit, 1, limit.min); !r) {
    return r;
  }
  return device_.writeValue(Object::kSoftwarePositionLimit, 2, limit.max);
}

std::expected<uint8_t, std::string> Cia402Drive::polarity() const {
  return device_.readValue<uint8_t>(Object::kPolarity, 0);
}

std::expected<void, std::string> Cia402Drive::setPolarity(uint8_t value) {
  return device_.writeValue(Object::kPolarity, 0, value);
}

std::expected<uint32_t, std::string> Cia402Drive::maxMotorSpeed() const {
  return device_.readValue<uint32_t>(Object::kMaxMotorSpeed, 0);
}

std::expected<void, std::string> Cia402Drive::setMaxMotorSpeed(uint32_t value) {
  return device_.writeValue(Object::kMaxMotorSpeed, 0, value);
}

std::expected<uint32_t, std::string> Cia402Drive::profileVelocity() const {
  return device_.readValue<uint32_t>(Object::kProfileVelocity, 0);
}

std::expected<void, std::string> Cia402Drive::setProfileVelocity(uint32_t value) {
  return device_.writeValue(Object::kProfileVelocity, 0, value);
}

std::expected<uint32_t, std::string> Cia402Drive::profileAcceleration() const {
  return device_.readValue<uint32_t>(Object::kProfileAcceleration, 0);
}

std::expected<void, std::string> Cia402Drive::setProfileAcceleration(uint32_t value) {
  return device_.writeValue(Object::kProfileAcceleration, 0, value);
}

std::expected<uint32_t, std::string> Cia402Drive::profileDeceleration() const {
  return device_.readValue<uint32_t>(Object::kProfileDeceleration, 0);
}

std::expected<void, std::string> Cia402Drive::setProfileDeceleration(uint32_t value) {
  return device_.writeValue(Object::kProfileDeceleration, 0, value);
}

std::expected<uint32_t, std::string> Cia402Drive::quickStopDeceleration() const {
  return device_.readValue<uint32_t>(Object::kQuickStopDeceleration, 0);
}

std::expected<void, std::string> Cia402Drive::setQuickStopDeceleration(uint32_t value) {
  return device_.writeValue(Object::kQuickStopDeceleration, 0, value);
}

std::expected<int16_t, std::string> Cia402Drive::motionProfileType() const {
  return device_.readValue<int16_t>(Object::kMotionProfileType, 0);
}

std::expected<void, std::string> Cia402Drive::setMotionProfileType(int16_t value) {
  return device_.writeValue(Object::kMotionProfileType, 0, value);
}

std::expected<uint32_t, std::string> Cia402Drive::torqueSlope() const {
  return device_.readValue<uint32_t>(Object::kTorqueSlope, 0);
}

std::expected<void, std::string> Cia402Drive::setTorqueSlope(uint32_t value) {
  return device_.writeValue(Object::kTorqueSlope, 0, value);
}

std::expected<int16_t, std::string> Cia402Drive::torqueProfileType() const {
  return device_.readValue<int16_t>(Object::kTorqueProfileType, 0);
}

std::expected<void, std::string> Cia402Drive::setTorqueProfileType(int16_t value) {
  return device_.writeValue(Object::kTorqueProfileType, 0, value);
}

std::expected<GearRatio, std::string> Cia402Drive::gearRatio() const {
  auto object = device_.readObject(Object::kGearRatio);
  if (!object) {
    return std::unexpected(object.error());
  }
  auto motorRevolutions = object->get<uint32_t>(1);
  if (!motorRevolutions) {
    return std::unexpected(motorRevolutions.error());
  }
  auto shaftRevolutions = object->get<uint32_t>(2);
  if (!shaftRevolutions) {
    return std::unexpected(shaftRevolutions.error());
  }
  return GearRatio{*motorRevolutions, *shaftRevolutions};
}

std::expected<void, std::string> Cia402Drive::setGearRatio(const GearRatio& ratio) {
  if (auto r = device_.writeValue(Object::kGearRatio, 1, ratio.motorRevolutions); !r) {
    return r;
  }
  return device_.writeValue(Object::kGearRatio, 2, ratio.shaftRevolutions);
}

std::expected<FeedConstant, std::string> Cia402Drive::feedConstant() const {
  auto object = device_.readObject(Object::kFeedConstant);
  if (!object) {
    return std::unexpected(object.error());
  }
  auto feed = object->get<uint32_t>(1);
  if (!feed) {
    return std::unexpected(feed.error());
  }
  auto shaftRevolutions = object->get<uint32_t>(2);
  if (!shaftRevolutions) {
    return std::unexpected(shaftRevolutions.error());
  }
  return FeedConstant{*feed, *shaftRevolutions};
}

std::expected<void, std::string> Cia402Drive::setFeedConstant(const FeedConstant& constant) {
  if (auto r = device_.writeValue(Object::kFeedConstant, 1, constant.feed); !r) {
    return r;
  }
  return device_.writeValue(Object::kFeedConstant, 2, constant.shaftRevolutions);
}

std::expected<int8_t, std::string> Cia402Drive::homingMethod() const {
  return device_.readValue<int8_t>(Object::kHomingMethod, 0);
}

std::expected<void, std::string> Cia402Drive::setHomingMethod(int8_t method) {
  return device_.writeValue(Object::kHomingMethod, 0, method);
}

std::expected<HomingSpeeds, std::string> Cia402Drive::homingSpeeds() const {
  auto object = device_.readObject(Object::kHomingSpeeds);
  if (!object) {
    return std::unexpected(object.error());
  }
  auto switchSearch = object->get<uint32_t>(1);
  if (!switchSearch) {
    return std::unexpected(switchSearch.error());
  }
  auto zeroSearch = object->get<uint32_t>(2);
  if (!zeroSearch) {
    return std::unexpected(zeroSearch.error());
  }
  return HomingSpeeds{*switchSearch, *zeroSearch};
}

std::expected<void, std::string> Cia402Drive::setHomingSpeeds(const HomingSpeeds& speeds) {
  if (auto r = device_.writeValue(Object::kHomingSpeeds, 1, speeds.switchSearch); !r) {
    return r;
  }
  return device_.writeValue(Object::kHomingSpeeds, 2, speeds.zeroSearch);
}

std::expected<uint32_t, std::string> Cia402Drive::homingAcceleration() const {
  return device_.readValue<uint32_t>(Object::kHomingAcceleration, 0);
}

std::expected<void, std::string> Cia402Drive::setHomingAcceleration(uint32_t value) {
  return device_.writeValue(Object::kHomingAcceleration, 0, value);
}

std::expected<uint32_t, std::string> Cia402Drive::siUnitVelocity() const {
  return device_.readValue<uint32_t>(Object::kSiUnitVelocity, 0);
}

std::expected<void, std::string> Cia402Drive::setSiUnitVelocity(uint32_t value) {
  return device_.writeValue(Object::kSiUnitVelocity, 0, value);
}

std::expected<int32_t, std::string> Cia402Drive::velocityOffset() const {
  return device_.readValue<int32_t>(Object::kVelocityOffset, 0);
}

std::expected<void, std::string> Cia402Drive::setVelocityOffset(int32_t value) {
  return device_.writeValue(Object::kVelocityOffset, 0, value);
}

std::expected<int16_t, std::string> Cia402Drive::torqueOffset() const {
  return device_.readValue<int16_t>(Object::kTorqueOffset, 0);
}

std::expected<void, std::string> Cia402Drive::setTorqueOffset(int16_t value) {
  return device_.writeValue(Object::kTorqueOffset, 0, value);
}

std::expected<uint16_t, std::string> Cia402Drive::touchProbeFunction() const {
  return device_.readValue<uint16_t>(Object::kTouchProbeFunction, 0);
}

std::expected<void, std::string> Cia402Drive::setTouchProbeFunction(uint16_t value) {
  return device_.writeValue(Object::kTouchProbeFunction, 0, value);
}

std::expected<uint16_t, std::string> Cia402Drive::touchProbeStatus() const {
  return device_.readValue<uint16_t>(Object::kTouchProbeStatus, 0);
}

std::expected<int32_t, std::string> Cia402Drive::touchProbe1PositiveEdge() const {
  return device_.readValue<int32_t>(Object::kTouchProbe1PositiveEdge, 0);
}

std::expected<int32_t, std::string> Cia402Drive::touchProbe1NegativeEdge() const {
  return device_.readValue<int32_t>(Object::kTouchProbe1NegativeEdge, 0);
}

std::expected<uint32_t, std::string> Cia402Drive::touchProbeTimeStamp1PositiveValue() const {
  return device_.readValue<uint32_t>(Object::kTouchProbeTimeStamp1PositiveValue, 0);
}

std::expected<uint32_t, std::string> Cia402Drive::touchProbeTimeStamp1NegativeValue() const {
  return device_.readValue<uint32_t>(Object::kTouchProbeTimeStamp1NegativeValue, 0);
}

std::expected<uint16_t, std::string> Cia402Drive::positioningOptionCode() const {
  return device_.readValue<uint16_t>(Object::kPositioningOptionCode, 0);
}

std::expected<void, std::string> Cia402Drive::setPositioningOptionCode(uint16_t value) {
  return device_.writeValue(Object::kPositioningOptionCode, 0, value);
}

std::expected<int32_t, std::string> Cia402Drive::followingErrorActualValue() const {
  return device_.readValue<int32_t>(Object::kFollowingErrorActualValue, 0);
}

std::expected<int32_t, std::string> Cia402Drive::controlEffort() const {
  return device_.readValue<int32_t>(Object::kControlEffort, 0);
}

std::expected<int32_t, std::string> Cia402Drive::positionDemandInternalValue() const {
  return device_.readValue<int32_t>(Object::kPositionDemandInternalValue, 0);
}

std::expected<uint32_t, std::string> Cia402Drive::digitalInputs() const {
  return device_.readValue<uint32_t>(Object::kDigitalInputs, 0);
}

std::expected<DigitalOutputs, std::string> Cia402Drive::digitalOutputs() const {
  auto object = device_.readObject(Object::kDigitalOutputs);
  if (!object) {
    return std::unexpected(object.error());
  }
  auto physicalOutputs = object->get<uint32_t>(1);
  if (!physicalOutputs) {
    return std::unexpected(physicalOutputs.error());
  }
  auto bitMask = object->get<uint32_t>(2);
  if (!bitMask) {
    return std::unexpected(bitMask.error());
  }
  return DigitalOutputs{*physicalOutputs, *bitMask};
}

std::expected<void, std::string> Cia402Drive::setDigitalOutputs(const DigitalOutputs& outputs) {
  // Levels first (inert while masked off), mask second — so a newly enabled output comes up with
  // its commanded level instead of briefly driving a stale one.
  if (auto r = device_.writeValue(Object::kDigitalOutputs, 1, outputs.physicalOutputs); !r) {
    return r;
  }
  return device_.writeValue(Object::kDigitalOutputs, 2, outputs.bitMask);
}

std::expected<uint32_t, std::string> Cia402Drive::supportedDriveModes() const {
  return device_.readCachedValue<uint32_t>(Object::kSupportedDriveModes, 0);
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
