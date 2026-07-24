#include "node/cia402_drive.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <map>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "comm/fieldbus_driver.h"
#include "comm/object_data_types.h"
#include "node/cia402.h"
#include "node/device.h"

namespace {

using mm::comm::EtherCatState;
using mm::comm::FieldbusDriver;
using mm::comm::ObjectDataType;
using mm::comm::OdEntry;
using mm::comm::SlaveInfo;
using mm::node::Cia402Drive;
using mm::node::createCia402Drive;
using mm::node::Device;
using mm::node::cia402::Object;
using mm::node::cia402::OperationMode;
using mm::node::cia402::State;

constexpr uint16_t kPreOp = static_cast<uint16_t>(EtherCatState::PreOp);

std::vector<uint8_t> u16le(uint16_t v) {
  return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
}
std::vector<uint8_t> u32le(uint32_t v) {
  return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
          static_cast<uint8_t>(v >> 24)};
}
std::vector<uint8_t> i32le(int32_t v) { return u32le(static_cast<uint32_t>(v)); }

// The statusword a drive reports for each state (canonical bit patterns).
uint16_t statuswordFor(State s) {
  switch (s) {
    case State::kNotReadyToSwitchOn:
      return 0x0000;
    case State::kSwitchOnDisabled:
      return 0x0040;
    case State::kReadyToSwitchOn:
      return 0x0021;
    case State::kSwitchedOn:
      return 0x0023;
    case State::kOperationEnabled:
      return 0x0027;
    case State::kQuickStopActive:
      return 0x0007;
    case State::kFaultReactionActive:
      return 0x000F;
    case State::kFault:
      return 0x0008;
  }
  return 0x0000;
}

/// FieldbusDriver test double that models the CiA402 device-control state machine: writing the
/// controlword (0x6040) advances an internal state and the statusword (0x6041) it reports tracks
/// it. SDO reads/writes hit a small object store; all downloads are also recorded for assertions.
class Cia402FakeDriver : public FieldbusDriver {
 public:
  struct Write {
    uint16_t index;
    uint8_t subindex;
    std::vector<uint8_t> data;
  };

  std::map<uint32_t, std::vector<uint8_t>> store;
  std::vector<Write> writes;
  std::vector<OdEntry> ods;
  uint16_t alStatus = kPreOp;
  uint32_t vendorId = 0;
  State machineState = State::kSwitchOnDisabled;

  static uint32_t key(uint16_t index, uint8_t subindex) {
    return (static_cast<uint32_t>(index) << 8) | subindex;
  }

  void programObject(uint16_t index, uint8_t subindex, ObjectDataType type,
                     std::vector<uint8_t> initial) {
    OdEntry e{};
    e.index = index;
    e.subindex = subindex;
    e.dataType = static_cast<uint16_t>(type);
    ods.push_back(e);
    store[key(index, subindex)] = std::move(initial);
  }

  // Seeds the standard CiA402 objects and parks the machine at SwitchOnDisabled.
  void programCia402Objects() {
    programObject(Object::kControlword, 0, ObjectDataType::UNSIGNED16, u16le(0));
    programObject(Object::kStatusword, 0, ObjectDataType::UNSIGNED16,
                  u16le(statuswordFor(State::kSwitchOnDisabled)));
    programObject(Object::kModeOfOperation, 0, ObjectDataType::INTEGER8, {0});
    programObject(Object::kModeOfOperationDisplay, 0, ObjectDataType::INTEGER8, {0});
    programObject(Object::kTargetPosition, 0, ObjectDataType::INTEGER32, i32le(0));
    programObject(Object::kTargetVelocity, 0, ObjectDataType::INTEGER32, i32le(0));
    programObject(Object::kTargetTorque, 0, ObjectDataType::INTEGER16, u16le(0));
    programObject(Object::kPositionActualValue, 0, ObjectDataType::INTEGER32, i32le(0));
    programObject(Object::kVelocityActualValue, 0, ObjectDataType::INTEGER32, i32le(0));
  }

  // Applies one controlword edge to the modelled state machine.
  void advance(uint16_t controlword) {
    const uint16_t cmd = controlword & mm::node::cia402::kCommandMask;
    if ((controlword & 0x0080) &&
        (machineState == State::kFault || machineState == State::kFaultReactionActive)) {
      machineState = State::kSwitchOnDisabled;
    } else if (cmd == 0x0000 || cmd == 0x0002) {
      machineState = (cmd == 0x0002) ? State::kQuickStopActive : State::kSwitchOnDisabled;
    } else if ((cmd & 0x000F) == 0x0006) {  // shutdown
      machineState = State::kReadyToSwitchOn;
    } else if ((cmd & 0x000F) == 0x0007) {  // switch on / disable operation
      if (machineState == State::kOperationEnabled) {
        machineState = State::kSwitchedOn;
      } else if (machineState == State::kReadyToSwitchOn) {
        machineState = State::kSwitchedOn;
      }
    } else if ((cmd & 0x000F) == 0x000F) {  // enable operation
      if (machineState == State::kSwitchedOn || machineState == State::kReadyToSwitchOn) {
        machineState = State::kOperationEnabled;
      }
    }
    store[key(Object::kStatusword, 0)] = u16le(statuswordFor(machineState));
  }

  std::expected<std::vector<uint8_t>, std::string> readSdo(uint16_t, uint16_t index,
                                                           uint8_t subindex) override {
    auto it = store.find(key(index, subindex));
    if (it == store.end()) {
      return std::unexpected("no such object");
    }
    return it->second;
  }

  std::expected<void, std::string> writeSdo(uint16_t, uint16_t index, uint8_t subindex,
                                            std::span<const uint8_t> data) override {
    std::vector<uint8_t> bytes(data.begin(), data.end());
    writes.push_back({index, subindex, bytes});
    store[key(index, subindex)] = bytes;
    if (index == Object::kControlword && bytes.size() >= 2) {
      advance(static_cast<uint16_t>(bytes[0] | (bytes[1] << 8)));
    }
    return {};
  }

  std::expected<std::vector<OdEntry>, std::string> readObjectDictionary(uint16_t) override {
    return ods;
  }

  // --- unused stubs ---------------------------------------------------------
  std::expected<void, std::string> init() override { return {}; }
  std::expected<int, std::string> scan() override { return 0; }
  SlaveInfo slaveInfo(uint16_t) const override {
    SlaveInfo info{};
    info.vendorId = vendorId;
    return info;
  }
  uint16_t slaveState(uint16_t) const override { return alStatus; }
  // CoE-capable stand-in: parameters are enumerated over the object dictionary, not SII.
  uint16_t mailboxProtocols(uint16_t) const override {
    return mm::comm::MailboxConfig::kProtocolCoe;
  }
  std::expected<void, std::string> configureProcessData() override { return {}; }
  mm::comm::PdoLayout processDataLayout() override { return {}; }
  int exchangeProcessData(std::span<const uint8_t>, std::span<uint8_t>) override { return 0; }
  void stop() override {}
  std::expected<std::vector<SlaveStateRaw>, std::string> readStates(
      const std::vector<uint16_t>& positions) override {
    return std::vector<SlaveStateRaw>(positions.size(), SlaveStateRaw{});
  }
  std::expected<std::vector<uint8_t>, std::string> readFile(uint16_t, const std::string&) override {
    return std::vector<uint8_t>{};
  }
  std::expected<void, std::string> writeFile(uint16_t, const std::string&,
                                             std::span<const uint8_t>) override {
    return {};
  }
  std::expected<void, std::string> readRegister(uint16_t, uint16_t, std::span<uint8_t>) override {
    return {};
  }
  std::expected<void, std::string> writeRegister(uint16_t, uint16_t,
                                                 std::span<const uint8_t>) override {
    return {};
  }
  void transitionToState(const std::vector<uint16_t>&, std::optional<EtherCatState>, EtherCatState,
                         std::chrono::steady_clock::duration, std::chrono::steady_clock::duration,
                         std::function<void()>, std::function<bool()>) override {}
};

// Builds a device with the standard CiA402 objects enumerated, online (PRE-OP) for SDO access.
Device makeCia402Device(Cia402FakeDriver& driver) {
  driver.programCia402Objects();
  Device device(1, driver);
  // Enumerating the programmed objects is a precondition for every test built on this helper;
  // assert it so a broken enumeration fails here with its message rather than downstream.
  auto initialized = device.initializeParameters();
  EXPECT_TRUE(initialized.has_value()) << initialized.error();
  return device;
}

TEST(CreateCia402Drive, RejectsDeviceWithoutControlStatusObjects) {
  Cia402FakeDriver driver;
  Device device(1, driver);  // no parameters enumerated
  auto drive = createCia402Drive(device);
  EXPECT_FALSE(drive.has_value());
}

TEST(DeviceIsCia402, TrueOnlyWithControlAndStatusObjects) {
  Cia402FakeDriver driver;
  Device bare(1, driver);  // no parameters enumerated
  EXPECT_FALSE(bare.isCia402());

  Device drive = makeCia402Device(driver);
  EXPECT_TRUE(drive.isCia402());
}

TEST(CreateCia402Drive, AcceptsDriveAndReadsState) {
  Cia402FakeDriver driver;
  Device device = makeCia402Device(driver);

  auto drive = createCia402Drive(device);
  ASSERT_TRUE(drive.has_value());

  auto st = drive->state();
  ASSERT_TRUE(st.has_value());
  EXPECT_EQ(*st, State::kSwitchOnDisabled);
}

TEST(Cia402Drive, TransitionPreservesNonCommandBits) {
  Cia402FakeDriver driver;
  Device device = makeCia402Device(driver);
  // Seed the controlword with a mode-specific bit (4) and the halt bit (8) set.
  driver.store[Cia402FakeDriver::key(Object::kControlword, 0)] = u16le(0x0110);
  Cia402Drive drive(device);

  ASSERT_TRUE(drive.shutdown().has_value());

  ASSERT_FALSE(driver.writes.empty());
  const auto& w = driver.writes.back();
  EXPECT_EQ(w.index, static_cast<uint16_t>(Object::kControlword));
  // Command bits become Shutdown (0x06); bits 4 and 8 are preserved → 0x0116.
  EXPECT_EQ(w.data, u16le(0x0116));
}

TEST(Cia402Drive, EnableWalksToOperationEnabled) {
  Cia402FakeDriver driver;
  Device device = makeCia402Device(driver);
  Cia402Drive drive(device);

  auto result = drive.enable(std::chrono::milliseconds(500));
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(driver.machineState, State::kOperationEnabled);

  // The walk issued the three expected command words in order: Shutdown, SwitchOn, EnableOp.
  std::vector<uint16_t> commandWords;
  for (const auto& w : driver.writes) {
    if (w.index == Object::kControlword && w.data.size() >= 2) {
      commandWords.push_back(static_cast<uint16_t>(w.data[0] | (w.data[1] << 8)) &
                             mm::node::cia402::kCommandMask);
    }
  }
  ASSERT_GE(commandWords.size(), 3u);
  EXPECT_EQ(commandWords[0], 0x0006);
  EXPECT_EQ(commandWords[1], 0x0007);
  EXPECT_EQ(commandWords[2], 0x000F);
}

TEST(Cia402Drive, FaultResetClearsFaultThenEnables) {
  Cia402FakeDriver driver;
  driver.machineState = State::kFault;
  Device device = makeCia402Device(driver);
  driver.store[Cia402FakeDriver::key(Object::kStatusword, 0)] = u16le(statuswordFor(State::kFault));
  Cia402Drive drive(device);

  auto result = drive.enable(std::chrono::milliseconds(500));
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(driver.machineState, State::kOperationEnabled);
}

TEST(Cia402Drive, FaultResetAssertsRisingEdgeWithoutClearing) {
  Cia402FakeDriver driver;
  driver.machineState = State::kFault;
  Device device = makeCia402Device(driver);
  driver.store[Cia402FakeDriver::key(Object::kStatusword, 0)] = u16le(statuswordFor(State::kFault));
  Cia402Drive drive(device);

  ASSERT_TRUE(drive.faultReset().has_value());

  // faultReset() issues exactly one controlword write, setting bit 7 (the rising edge that latches
  // the reset). It must NOT clear bit 7 in the same call: a set+clear pair collapses on the PDO
  // staging path (both writes land in one output slot and the RT loop sends only the last), so the
  // drive would never see the edge. Bit 7 is re-cleared by the next state-machine command instead.
  std::vector<uint16_t> controlwords;
  for (const auto& w : driver.writes) {
    if (w.index == Object::kControlword && w.data.size() >= 2) {
      controlwords.push_back(static_cast<uint16_t>(w.data[0] | (w.data[1] << 8)));
    }
  }
  ASSERT_EQ(controlwords.size(), 1u);
  EXPECT_TRUE(controlwords.back() & 0x0080);
}

TEST(Cia402Drive, EnableTimesOutWhenStuck) {
  Cia402FakeDriver driver;
  driver.machineState = State::kQuickStopActive;  // enable() deliberately will not override this
  Device device = makeCia402Device(driver);
  driver.store[Cia402FakeDriver::key(Object::kStatusword, 0)] =
      u16le(statuswordFor(State::kQuickStopActive));
  Cia402Drive drive(device);

  auto result = drive.enable(std::chrono::milliseconds(20));
  EXPECT_FALSE(result.has_value());
}

TEST(Cia402Drive, SetOperationModeAndSetpoints) {
  Cia402FakeDriver driver;
  Device device = makeCia402Device(driver);
  Cia402Drive drive(device);

  ASSERT_TRUE(drive.setOperationMode(OperationMode::kCyclicSyncPosition).has_value());
  ASSERT_TRUE(drive.setTargetPosition(0x00405060).has_value());

  // Mode written to 0x6060 as INTEGER8 = 8; target position to 0x607A as INTEGER32.
  bool sawMode = false;
  bool sawTarget = false;
  for (const auto& w : driver.writes) {
    if (w.index == Object::kModeOfOperation) {
      EXPECT_EQ(w.data, std::vector<uint8_t>{8});
      sawMode = true;
    }
    if (w.index == Object::kTargetPosition) {
      EXPECT_EQ(w.data, i32le(0x00405060));
      sawTarget = true;
    }
  }
  EXPECT_TRUE(sawMode);
  EXPECT_TRUE(sawTarget);
}

TEST(Cia402Drive, SetTargetTorqueAcceptsNegative) {
  Cia402FakeDriver driver;
  Device device = makeCia402Device(driver);
  Cia402Drive drive(device);

  // Target torque (0x6071) is a signed INTEGER16 — regenerative / reverse torque is negative.
  ASSERT_TRUE(drive.setTargetTorque(-1000).has_value());
  ASSERT_FALSE(driver.writes.empty());
  const auto& w = driver.writes.back();
  EXPECT_EQ(w.index, static_cast<uint16_t>(Object::kTargetTorque));
  EXPECT_EQ(w.data, u16le(static_cast<uint16_t>(static_cast<int16_t>(-1000))));
}

TEST(Cia402Drive, ReadStatusReportsAllFields) {
  Cia402FakeDriver driver;
  Device device = makeCia402Device(driver);
  driver.store[Cia402FakeDriver::key(Object::kStatusword, 0)] =
      u16le(statuswordFor(State::kOperationEnabled));
  driver.store[Cia402FakeDriver::key(Object::kControlword, 0)] = u16le(0x000F);
  driver.store[Cia402FakeDriver::key(Object::kModeOfOperationDisplay, 0)] = {9};  // CSV
  // CSV reads its setpoint from target velocity (0x60FF, INTEGER32) into status.target.
  driver.store[Cia402FakeDriver::key(Object::kTargetVelocity, 0)] = i32le(250000);
  Cia402Drive drive(device);

  auto status = drive.readStatus();
  ASSERT_TRUE(status.has_value()) << status.error();
  EXPECT_EQ(status->state, State::kOperationEnabled);
  EXPECT_EQ(status->statusword, statuswordFor(State::kOperationEnabled));
  EXPECT_EQ(status->controlword, 0x000F);
  EXPECT_EQ(status->modeOfOperationDisplay, OperationMode::kCyclicSyncVelocity);
  EXPECT_EQ(status->target, 250000);
}

TEST(Cia402Drive, ReadStatusTargetTracksActiveModeQuantity) {
  Cia402FakeDriver driver;
  Device device = makeCia402Device(driver);
  driver.store[Cia402FakeDriver::key(Object::kStatusword, 0)] =
      u16le(statuswordFor(State::kOperationEnabled));
  driver.store[Cia402FakeDriver::key(Object::kControlword, 0)] = u16le(0x000F);
  // Setpoints for all three quantities are present; readStatus must pick the one the active mode
  // acts on (profile modes read their setpoint too — only NoMode/Homing report 0).
  driver.store[Cia402FakeDriver::key(Object::kTargetPosition, 0)] = i32le(111);
  driver.store[Cia402FakeDriver::key(Object::kTargetVelocity, 0)] = i32le(222);
  driver.store[Cia402FakeDriver::key(Object::kTargetTorque, 0)] = u16le(static_cast<uint16_t>(-33));
  Cia402Drive drive(device);

  auto targetInMode = [&](uint8_t mode) {
    driver.store[Cia402FakeDriver::key(Object::kModeOfOperationDisplay, 0)] = {mode};
    auto s = drive.readStatus();
    return s.has_value() ? s->target : 0;
  };
  EXPECT_EQ(targetInMode(1), 111);   // Profile Position → target position
  EXPECT_EQ(targetInMode(8), 111);   // CSP → target position
  EXPECT_EQ(targetInMode(3), 222);   // Profile Velocity → target velocity
  EXPECT_EQ(targetInMode(9), 222);   // CSV → target velocity
  EXPECT_EQ(targetInMode(4), -33);   // Profile Torque → target torque (INT16, sign-extended)
  EXPECT_EQ(targetInMode(10), -33);  // CST → target torque
  EXPECT_EQ(targetInMode(0), 0);     // NoMode → no linear setpoint
  EXPECT_EQ(targetInMode(6), 0);     // Homing → no linear setpoint
}

TEST(Cia402Drive, ProfileObjectRoundTrip) {
  // Representative rw profile objects of each width: the setter writes the exact wire bytes and
  // the (live) getter reads them back.
  Cia402FakeDriver driver;
  driver.programObject(Object::kMaxTorque, 0, ObjectDataType::UNSIGNED16, u16le(0));
  driver.programObject(Object::kProfileVelocity, 0, ObjectDataType::UNSIGNED32, u32le(0));
  driver.programObject(Object::kHomingMethod, 0, ObjectDataType::INTEGER8, {0});
  Device device = makeCia402Device(driver);
  Cia402Drive drive(device);

  ASSERT_TRUE(drive.setMaxTorque(500).has_value());
  ASSERT_TRUE(drive.setProfileVelocity(200000).has_value());
  ASSERT_TRUE(drive.setHomingMethod(-3).has_value());  // INTEGER8 is signed — e.g. method -3

  EXPECT_EQ(drive.maxTorque().value_or(0), 500);
  EXPECT_EQ(drive.profileVelocity().value_or(0), 200000u);
  EXPECT_EQ(drive.homingMethod().value_or(0), -3);
}

TEST(Cia402Drive, VolatileGetterRereadsTheDevice) {
  // Error code (0x603F) is read-only but volatile: every call must reach the device, never serve
  // a cached first reading.
  Cia402FakeDriver driver;
  driver.programObject(Object::kErrorCode, 0, ObjectDataType::UNSIGNED16, u16le(0));
  Device device = makeCia402Device(driver);
  Cia402Drive drive(device);

  EXPECT_EQ(drive.errorCode().value_or(1), 0);
  driver.store[Cia402FakeDriver::key(Object::kErrorCode, 0)] = u16le(0x7500);  // fault appears
  EXPECT_EQ(drive.errorCode().value_or(0), 0x7500);
}

TEST(Cia402Drive, SupportedDriveModesIsCached) {
  // Supported drive modes (0x6502) is a constant capability field: the first read is served from
  // the device, subsequent reads from the cache — a store change must not show through.
  Cia402FakeDriver driver;
  driver.programObject(Object::kSupportedDriveModes, 0, ObjectDataType::UNSIGNED32, u32le(0x03ED));
  Device device = makeCia402Device(driver);
  Cia402Drive drive(device);

  EXPECT_EQ(drive.supportedDriveModes().value_or(0), 0x03EDu);
  driver.store[Cia402FakeDriver::key(Object::kSupportedDriveModes, 0)] = u32le(0);
  EXPECT_EQ(drive.supportedDriveModes().value_or(0), 0x03EDu);
}

TEST(Cia402Drive, StructGetterReadsBothSubEntries) {
  Cia402FakeDriver driver;
  driver.programObject(Object::kSoftwarePositionLimit, 1, ObjectDataType::INTEGER32, i32le(-1000));
  driver.programObject(Object::kSoftwarePositionLimit, 2, ObjectDataType::INTEGER32, i32le(5000));
  Device device = makeCia402Device(driver);
  Cia402Drive drive(device);

  auto limit = drive.softwarePositionLimit();
  ASSERT_TRUE(limit.has_value()) << limit.error();
  EXPECT_EQ(limit->min, -1000);
  EXPECT_EQ(limit->max, 5000);
}

TEST(Cia402Drive, StructSetterWritesSubOneThenSubTwo) {
  Cia402FakeDriver driver;
  driver.programObject(Object::kGearRatio, 1, ObjectDataType::UNSIGNED32, u32le(1));
  driver.programObject(Object::kGearRatio, 2, ObjectDataType::UNSIGNED32, u32le(1));
  Device device = makeCia402Device(driver);
  Cia402Drive drive(device);

  ASSERT_TRUE(drive.setGearRatio({30, 1}).has_value());

  ASSERT_EQ(driver.writes.size(), 2u);
  EXPECT_EQ(driver.writes[0].index, static_cast<uint16_t>(Object::kGearRatio));
  EXPECT_EQ(driver.writes[0].subindex, 1);
  EXPECT_EQ(driver.writes[0].data, u32le(30));
  EXPECT_EQ(driver.writes[1].subindex, 2);
  EXPECT_EQ(driver.writes[1].data, u32le(1));
}

TEST(Cia402Drive, SetDigitalOutputsWritesLevelsBeforeMask) {
  Cia402FakeDriver driver;
  driver.programObject(Object::kDigitalOutputs, 1, ObjectDataType::UNSIGNED32, u32le(0));
  driver.programObject(Object::kDigitalOutputs, 2, ObjectDataType::UNSIGNED32, u32le(0));
  Device device = makeCia402Device(driver);
  Cia402Drive drive(device);

  ASSERT_TRUE(drive.setDigitalOutputs({0x5, 0x7}).has_value());

  // Levels (sub 1) must land before the enable mask (sub 2): while masked off they are inert, so
  // a newly enabled output comes up with its commanded level instead of briefly driving a stale
  // one. Mask-first would glitch the output.
  ASSERT_EQ(driver.writes.size(), 2u);
  EXPECT_EQ(driver.writes[0].index, static_cast<uint16_t>(Object::kDigitalOutputs));
  EXPECT_EQ(driver.writes[0].subindex, 1);
  EXPECT_EQ(driver.writes[0].data, u32le(0x5));
  EXPECT_EQ(driver.writes[1].subindex, 2);
  EXPECT_EQ(driver.writes[1].data, u32le(0x7));
}

TEST(Cia402Drive, StructSetterAbortsOnFirstFailure) {
  // Only sub 2 of the position range limit exists — the sub-1 write fails (unknown parameter)
  // and the setter must not go on to write sub 2.
  Cia402FakeDriver driver;
  driver.programObject(Object::kPositionRangeLimit, 2, ObjectDataType::INTEGER32, i32le(0));
  Device device = makeCia402Device(driver);
  Cia402Drive drive(device);

  EXPECT_FALSE(drive.setPositionRangeLimit({-100, 100}).has_value());
  EXPECT_TRUE(driver.writes.empty());
}

TEST(Cia402Status, JsonEmitsNamesAndNumbers) {
  const mm::node::Cia402Status s{State::kOperationEnabled, 0x1237, 0x000F,
                                 OperationMode::kCyclicSyncVelocity, 100000};
  const nlohmann::json j = s;
  EXPECT_EQ(j.at("state"), "OperationEnabled");
  EXPECT_EQ(j.at("statusword"), 0x1237);
  EXPECT_EQ(j.at("controlword"), 0x000F);
  EXPECT_EQ(j.at("modeOfOperation"), 9);
  EXPECT_EQ(j.at("modeName"), "CyclicSyncVelocity");
  EXPECT_EQ(j.at("target"), 100000);
}

TEST(Cia402ParseCommand, KnownAndUnknownTokens) {
  using mm::node::Cia402Command;
  using mm::node::parseCia402Command;
  EXPECT_EQ(parseCia402Command("enable"), Cia402Command::kEnable);
  EXPECT_EQ(parseCia402Command("disable"), Cia402Command::kDisable);
  EXPECT_EQ(parseCia402Command("quickStop"), Cia402Command::kQuickStop);
  EXPECT_EQ(parseCia402Command("faultReset"), Cia402Command::kFaultReset);
  EXPECT_FALSE(parseCia402Command("halt").has_value());
  EXPECT_FALSE(parseCia402Command("").has_value());
}

TEST(Cia402ParseTargetKind, KnownAndUnknownTokens) {
  using mm::node::Cia402TargetKind;
  using mm::node::parseCia402TargetKind;
  EXPECT_EQ(parseCia402TargetKind("position"), Cia402TargetKind::kPosition);
  EXPECT_EQ(parseCia402TargetKind("velocity"), Cia402TargetKind::kVelocity);
  EXPECT_EQ(parseCia402TargetKind("torque"), Cia402TargetKind::kTorque);
  EXPECT_FALSE(parseCia402TargetKind("accel").has_value());
}

}  // namespace
