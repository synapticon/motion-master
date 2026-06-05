#include "node/cia402_drive.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <map>
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
std::vector<uint8_t> i32le(int32_t v) {
  const auto u = static_cast<uint32_t>(v);
  return {static_cast<uint8_t>(u), static_cast<uint8_t>(u >> 8), static_cast<uint8_t>(u >> 16),
          static_cast<uint8_t>(u >> 24)};
}

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
    programObject(Object::kPositionActualValue, 0, ObjectDataType::INTEGER32, i32le(0));
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
  (void)device.initializeParameters();
  return device;
}

TEST(CreateCia402Drive, RejectsDeviceWithoutControlStatusObjects) {
  Cia402FakeDriver driver;
  Device device(1, driver);  // no parameters enumerated
  auto drive = createCia402Drive(device);
  EXPECT_FALSE(drive.has_value());
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

}  // namespace
