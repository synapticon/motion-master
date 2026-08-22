#include "node/fsoe_manager.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "etg/tests/fake_fsoe_slave.h"
#include "node/device_manager.h"
#include "node/tests/fake_bus.h"

namespace mm::node {
namespace {

using mm::comm::EtherCatState;
using mm::comm::SlaveIo;
using mm::node::testing::FakeBus;
using mm::node::testing::kI32;
using mm::node::testing::kU16;
using mm::node::testing::kU8;
using mm::node::testing::pdoEntry;
using mm::node::testing::u16le;
using mm::node::testing::u8le;

// The bus geometry of a Synapticon safe drive: a CiA-402 PDO pair, then the FSoE frame pair. The
// frame mapping below is the one the shipping ESI declares, entry for entry — including the two
// alignment gaps where a 32-bit safe value is split by the CRCs between its halves.
constexpr uint16_t kCia402OutputBytes = 6;
constexpr uint16_t kCia402InputBytes = 6;
constexpr uint16_t kMasterFrameBytes = 19;  // 8 SafeData octets
constexpr uint16_t kSlaveFrameBytes = 27;   // 12 SafeData octets
constexpr uint16_t kSlaveAddress = 3;
constexpr uint16_t kConnectionId = 7;

std::unique_ptr<FakeBus> makeSafeDriveBus() {
  auto bus = std::make_unique<FakeBus>();

  // Outputs: CiA-402, then the Safety Master Frame.
  bus->program(0x1C12, 0x00, u8le(2));
  bus->program(0x1C12, 0x01, u16le(0x1600));
  bus->program(0x1C12, 0x02, u16le(0x1604));
  bus->program(0x1600, 0x00, u8le(2));
  bus->program(0x1600, 0x01, pdoEntry(0x6040, 0x00, 16));
  bus->program(0x1600, 0x02, pdoEntry(0x607A, 0x00, 32));
  bus->program(0x1604, 0x00, u8le(14));
  bus->program(0x1604, 0x01, pdoEntry(0x2621, 0x01, 8));   // FSoE command
  bus->program(0x1604, 0x02, pdoEntry(0x6620, 0x01, 8));   // SafeData 1
  bus->program(0x1604, 0x03, pdoEntry(0x6620, 0x02, 8));   // SafeData 2
  bus->program(0x1604, 0x04, pdoEntry(0x2621, 0x04, 16));  // CRC_0
  bus->program(0x1604, 0x05, pdoEntry(0x6620, 0x03, 8));
  bus->program(0x1604, 0x06, pdoEntry(0x6620, 0x04, 8));
  bus->program(0x1604, 0x07, pdoEntry(0x2621, 0x05, 16));  // CRC_1
  bus->program(0x1604, 0x08, pdoEntry(0x6620, 0x05, 8));
  bus->program(0x1604, 0x09, pdoEntry(0x6620, 0x06, 8));
  bus->program(0x1604, 0x0A, pdoEntry(0x2621, 0x06, 16));  // CRC_2
  bus->program(0x1604, 0x0B, pdoEntry(0x6620, 0x07, 8));
  bus->program(0x1604, 0x0C, pdoEntry(0x6620, 0x08, 8));
  bus->program(0x1604, 0x0D, pdoEntry(0x2621, 0x07, 16));  // CRC_3
  bus->program(0x1604, 0x0E, pdoEntry(0x2621, 0x03, 16));  // connection ID

  // Inputs: CiA-402, then the Safety Slave Frame.
  bus->program(0x1C13, 0x00, u8le(2));
  bus->program(0x1C13, 0x01, u16le(0x1A00));
  bus->program(0x1C13, 0x02, u16le(0x1A04));
  bus->program(0x1A00, 0x00, u8le(2));
  bus->program(0x1A00, 0x01, pdoEntry(0x6041, 0x00, 16));
  bus->program(0x1A00, 0x02, pdoEntry(0x6064, 0x00, 32));
  bus->program(0x1A04, 0x00, u8le(15));
  bus->program(0x1A04, 0x01, pdoEntry(0x2622, 0x01, 8));   // FSoE command
  bus->program(0x1A04, 0x02, pdoEntry(0x6621, 0x01, 8));   // safety statusword
  bus->program(0x1A04, 0x03, pdoEntry(0x6621, 0x02, 8));   // validity bits
  bus->program(0x1A04, 0x04, pdoEntry(0x2622, 0x04, 16));  // CRC_0
  bus->program(0x1A04, 0x05, pdoEntry(0x6611, 0x00, 16));  // safe position, low half
  bus->program(0x1A04, 0x06, pdoEntry(0x2622, 0x05, 16));  // CRC_1
  bus->program(0x1A04, 0x07, pdoEntry(0x0000, 0x00, 16));  // safe position, high half
  bus->program(0x1A04, 0x08, pdoEntry(0x2622, 0x06, 16));  // CRC_2
  bus->program(0x1A04, 0x09, pdoEntry(0x6613, 0x00, 16));  // safe velocity, low half
  bus->program(0x1A04, 0x0A, pdoEntry(0x2622, 0x07, 16));  // CRC_3
  bus->program(0x1A04, 0x0B, pdoEntry(0x0000, 0x00, 16));  // safe velocity, high half
  bus->program(0x1A04, 0x0C, pdoEntry(0x2622, 0x08, 16));  // CRC_4
  bus->program(0x1A04, 0x0D, pdoEntry(0x6616, 0x00, 16));  // safe torque
  bus->program(0x1A04, 0x0E, pdoEntry(0x2622, 0x09, 16));  // CRC_5
  bus->program(0x1A04, 0x0F, pdoEntry(0x2622, 0x03, 16));  // connection ID

  bus->programOd(0x6040, 0x00, kU16);
  bus->programOd(0x607A, 0x00, kI32);
  bus->programOd(0x6041, 0x00, kU16);
  bus->programOd(0x6064, 0x00, kI32);
  bus->programOd(0x2621, 0x01, kU8);
  bus->programOd(0x2621, 0x03, kU16);
  for (uint8_t sub = 0x04; sub <= 0x07; ++sub) {
    bus->programOd(0x2621, sub, kU16);
  }
  for (uint8_t sub = 0x01; sub <= 0x08; ++sub) {
    bus->programOd(0x6620, sub, kU8);
  }
  bus->programOd(0x2622, 0x01, kU8);
  bus->programOd(0x6621, 0x01, kU8);
  bus->programOd(0x6621, 0x02, kU8);

  bus->slaves = 1;
  bus->layout.outputBytes = kCia402OutputBytes + kMasterFrameBytes;
  bus->layout.inputBytes = kCia402InputBytes + kSlaveFrameBytes;
  bus->layout.expectedWkc = 3;
  bus->layout.slaves = {SlaveIo{.slavePosition = 1,
                                .outputOffset = 0,
                                .outputBytes = bus->layout.outputBytes,
                                .inputOffset = 0,
                                .inputBytes = bus->layout.inputBytes}};
  bus->wkc = 3;
  bus->state = static_cast<uint16_t>(EtherCatState::Op);
  bus->cannedInputs.assign(bus->layout.inputBytes, 0);
  return bus;
}

/// A drive on a fake bus, with a real FSoE slave state machine answering the frames.
///
/// The peer is @c mm::etg::FakeFsoeSlave, the same double the master's own tests use — a port of
/// the ETG.5100 slave tables that authenticates every frame. So this exercises the whole path:
/// the frame is composed into the output image from parameter cells, read back out of the wire
/// buffer, answered, and decoded from the input image.
class SafeDrive {
 public:
  SafeDrive() : slave_(12, 8, kSlaveAddress, 6) {
    auto bus = makeSafeDriveBus();
    bus_ = bus.get();
    EXPECT_TRUE(dm_.init(std::move(bus)).has_value());
    EXPECT_TRUE(dm_.scan().has_value());
    EXPECT_TRUE(dm_.initializeDeviceParameters(1, false).has_value());
    EXPECT_TRUE(dm_.configureProcessData().has_value());
    slave_.setDataCommand(mm::etg::FsoeCommand::ProcessData);
    setSafeInputs();
  }

  /// One bus cycle, in the order a real one happens.
  ///
  /// The drive answers what the **previous** exchange delivered, because that is what a slave
  /// does: its TxPDO is prepared from the outputs it received in the cycle before. So the master
  /// always reads the answer to the frame it sent one exchange ago — the single cycle of lag an
  /// FSoE master sees on any fieldbus, and the reason the state machine tolerates it.
  void cycle() {
    const std::span<const uint8_t> previous(bus_->lastOutputs);
    if (previous.size() >= static_cast<size_t>(kCia402OutputBytes) + kMasterFrameBytes) {
      const auto frame = previous.subspan(kCia402OutputBytes, kMasterFrameBytes);
      // Only a frame in which a bit changed is an event, and the transport is what enforces that.
      // A slave handed the same frame twice answers twice, and the second answer breaks the CRC
      // chain — so the drive's own glue does exactly this check before it calls the FSoE core.
      const bool changed = !std::ranges::equal(frame, lastFedToSlave_);
      lastFedToSlave_.assign(frame.begin(), frame.end());
      if (!changed) {
        dm_.exchangeProcessData();
        manager_.execute();
        return;
      }
      const auto answer = slave_.process(frame);
      std::ranges::copy(answer, bus_->cannedInputs.begin() + kCia402InputBytes);
      if (getenv("FSOE_TRACE") != nullptr) {
        std::printf("M->S");
        for (uint8_t b : frame) {
          std::printf(" %02X", b);
        }
        std::printf("   S->M");
        for (uint8_t b : answer) {
          std::printf(" %02X", b);
        }
        std::printf("   slave=%d\n", static_cast<int>(slave_.state()));
      }
    }
    dm_.exchangeProcessData();
    manager_.execute();
  }

  void cycles(int n) {
    for (int i = 0; i < n; ++i) {
      cycle();
    }
  }

  /// The safe process values the drive reports: 1.5 revolutions, 600 mrpm, 250 mNm, all valid.
  void setSafeInputs(bool valid = true) {
    std::array<uint8_t, 12> inputs{};
    inputs[0] = 0x01;  // safety statusword: STO active
    inputs[1] = valid ? 0b0001'1111 : 0;
    const int32_t position = 3 << 23;  // 1.5 revolutions in 8.24 fixed point
    const int32_t velocity = 600;
    const int16_t torque = 250;
    std::memcpy(&inputs[2], &position, 4);
    std::memcpy(&inputs[6], &velocity, 4);
    std::memcpy(&inputs[10], &torque, 2);
    slave_.setSafeInputs(inputs);
  }

  DeviceManager& deviceManager() { return dm_; }
  FsoeManager& manager() { return manager_; }
  mm::etg::FakeFsoeSlave& slave() { return slave_; }
  FakeBus& bus() { return *bus_; }

  FsoeConnectionConfig config() const {
    return FsoeConnectionConfig{.slavePosition = 1,
                                .slaveAddress = kSlaveAddress,
                                .connectionId = kConnectionId,
                                .watchdogMs = 100};
  }

 private:
  DeviceManager dm_;
  FsoeManager manager_{dm_};
  std::vector<uint8_t> lastFedToSlave_;
  mm::etg::FakeFsoeSlave slave_;
  FakeBus* bus_ = nullptr;
};

TEST(FsoeManagerTest, OpensAConnectionAndReachesTheDataState) {
  SafeDrive drive;
  const auto opened = drive.manager().open(drive.config());
  ASSERT_TRUE(opened.has_value()) << (opened ? "" : opened.error());

  FsoeConnection* connection = drive.manager().find(1);
  ASSERT_NE(connection, nullptr);
  ASSERT_TRUE(connection->setDataCommand(mm::etg::FsoeCommand::ProcessData));

  drive.cycles(24);

  const FsoeConnectionState state = connection->state();
  EXPECT_TRUE(state.bound);
  EXPECT_EQ(state.state, mm::etg::FsoeState::Data);
  EXPECT_EQ(drive.slave().state(), mm::etg::FsoeState::Data);
  EXPECT_EQ(state.fault, mm::etg::FsoeError::None);
  EXPECT_TRUE(state.inputsValid);
  EXPECT_GT(state.framesAccepted, 0u);
}

TEST(FsoeManagerTest, TheFrameTravelsInTheProcessImageAfterTheCia402Data) {
  // The FSoE PDU is ordinary process data. This pins where it lands, because an offset error is
  // the failure that looks like a drive that never answers.
  SafeDrive drive;
  ASSERT_TRUE(drive.manager().open(drive.config()).has_value());
  drive.cycles(2);

  const std::vector<uint8_t>& outputs = drive.bus().lastOutputs;
  ASSERT_EQ(outputs.size(), kCia402OutputBytes + kMasterFrameBytes);
  const uint8_t command = outputs[kCia402OutputBytes];
  EXPECT_TRUE(mm::etg::fsoeIsKnownCommand(static_cast<mm::etg::FsoeCommand>(command)))
      << "octet " << kCia402OutputBytes << " is " << static_cast<int>(command);
}

TEST(FsoeManagerTest, CarriesTheSafeProcessValuesBackToTheCaller) {
  SafeDrive drive;
  ASSERT_TRUE(drive.manager().open(drive.config()).has_value());
  drive.manager().find(1)->setDataCommand(mm::etg::FsoeCommand::ProcessData);
  drive.cycles(24);

  const FsoeConnectionState state = drive.manager().find(1)->state();
  ASSERT_TRUE(state.inputsValid);

  const mm::etg::SdpProcessValues values = state.processValues();
  EXPECT_TRUE(values.positionValid);
  EXPECT_TRUE(values.velocityValid);
  EXPECT_TRUE(values.torqueValid);
  EXPECT_TRUE(values.crossCheckOk);
  EXPECT_TRUE(values.positionReferenced);
  EXPECT_DOUBLE_EQ(mm::etg::sdpPositionRevolutions(values.positionFixedPoint), 1.5);
  EXPECT_EQ(values.velocityMilliRpm, 600);
  EXPECT_EQ(values.torqueMillinewtonMetres, 250);
  EXPECT_TRUE(state.safetyStatus().stoActive);
}

TEST(FsoeManagerTest, ReleasingStoReachesTheDrive) {
  // The point of the whole layer: a caller sets one bit and the drive sees it, CRC-protected.
  SafeDrive drive;
  ASSERT_TRUE(drive.manager().open(drive.config()).has_value());
  FsoeConnection* connection = drive.manager().find(1);
  connection->setDataCommand(mm::etg::FsoeCommand::ProcessData);
  drive.cycles(24);
  ASSERT_EQ(connection->state().state, mm::etg::FsoeState::Data);

  // Still safe: nobody has asked for torque.
  EXPECT_TRUE(mm::etg::sdpDecodeControl(drive.slave().safeOutputs()).stoRequested);

  ASSERT_TRUE(connection->setControl(mm::etg::SdpControl{.stoRequested = false}));
  drive.cycles(3);

  EXPECT_TRUE(drive.slave().outputsValid());
  EXPECT_FALSE(mm::etg::sdpDecodeControl(drive.slave().safeOutputs()).stoRequested);
  EXPECT_EQ(connection->state().safeOutputs[0], 0x01);
}

TEST(FsoeManagerTest, FailSafeDataDropsTheDriveOutputsWithoutLosingTheConnection) {
  SafeDrive drive;
  ASSERT_TRUE(drive.manager().open(drive.config()).has_value());
  FsoeConnection* connection = drive.manager().find(1);
  connection->setDataCommand(mm::etg::FsoeCommand::ProcessData);
  connection->setControl(mm::etg::SdpControl{.stoRequested = false});
  drive.cycles(24);
  ASSERT_TRUE(drive.slave().outputsValid());

  ASSERT_TRUE(connection->setDataCommand(mm::etg::FsoeCommand::FailSafeData));
  drive.cycles(3);

  EXPECT_EQ(connection->state().state, mm::etg::FsoeState::Data);
  EXPECT_FALSE(drive.slave().outputsValid());
  // The drive reads all-zero SafeOutputs, which is a request for Safe Torque Off.
  EXPECT_TRUE(mm::etg::sdpDecodeControl(drive.slave().safeOutputs()).stoRequested);
}

TEST(FsoeManagerTest, ALocalResetRestartsTheHandshake) {
  SafeDrive drive;
  ASSERT_TRUE(drive.manager().open(drive.config()).has_value());
  FsoeConnection* connection = drive.manager().find(1);
  connection->setDataCommand(mm::etg::FsoeCommand::ProcessData);
  drive.cycles(24);
  ASSERT_EQ(connection->state().state, mm::etg::FsoeState::Data);

  connection->requestReset();
  drive.cycle();
  EXPECT_EQ(connection->state().state, mm::etg::FsoeState::Reset);
  EXPECT_FALSE(connection->state().inputsValid);

  drive.cycles(24);
  EXPECT_EQ(connection->state().state, mm::etg::FsoeState::Data);
}

TEST(FsoeManagerTest, ClosingStopsDrivingTheFrame) {
  SafeDrive drive;
  ASSERT_TRUE(drive.manager().open(drive.config()).has_value());
  drive.manager().find(1)->setDataCommand(mm::etg::FsoeCommand::ProcessData);
  drive.cycles(24);
  ASSERT_EQ(drive.slave().state(), mm::etg::FsoeState::Data);

  ASSERT_TRUE(drive.manager().close(1).has_value());
  EXPECT_EQ(drive.manager().find(1), nullptr);
  EXPECT_TRUE(drive.manager().report().empty());

  // The output image now composes from cells nobody updates, so the drive stops seeing new frames.
  // On hardware its watchdog would expire; here it is enough that the master stopped writing.
  const std::vector<uint8_t> before = drive.bus().lastOutputs;
  drive.cycles(3);
  EXPECT_EQ(drive.bus().lastOutputs, before);
}

TEST(FsoeManagerTest, RefusesADeviceThatCarriesNoSafetyPdo) {
  auto bus = mm::node::testing::makeCia402Bus();
  DeviceManager dm;
  ASSERT_TRUE(dm.init(std::move(bus)).has_value());
  ASSERT_TRUE(dm.scan().has_value());
  ASSERT_TRUE(dm.initializeDeviceParameters(1, false).has_value());
  ASSERT_TRUE(dm.configureProcessData().has_value());

  FsoeManager manager(dm);
  const auto opened = manager.open(FsoeConnectionConfig{.slavePosition = 1});
  ASSERT_FALSE(opened.has_value());
  EXPECT_NE(opened.error().find("0x1604"), std::string::npos) << opened.error();
}

TEST(FsoeManagerTest, RefusesAnUnknownDevice) {
  SafeDrive drive;
  const auto opened = drive.manager().open(FsoeConnectionConfig{.slavePosition = 9});
  ASSERT_FALSE(opened.has_value());
  EXPECT_NE(opened.error().find("not found"), std::string::npos) << opened.error();
}

TEST(FsoeManagerTest, ReOpeningReplacesTheConnectionRatherThanAddingOne) {
  SafeDrive drive;
  ASSERT_TRUE(drive.manager().open(drive.config()).has_value());
  drive.cycles(8);
  FsoeConnection* first = drive.manager().find(1);

  auto config = drive.config();
  config.connectionId = 9;
  ASSERT_TRUE(drive.manager().open(config).has_value());

  FsoeConnection* second = drive.manager().find(1);
  EXPECT_NE(second, first);
  EXPECT_FALSE(first->active());
  EXPECT_EQ(drive.manager().report().size(), 1u);
  EXPECT_EQ(second->config().connectionId, 9);

  drive.cycles(24);
  EXPECT_EQ(second->state().state, mm::etg::FsoeState::Data);
}

TEST(FsoeManagerTest, AReMapUnbindsTheConnectionInsteadOfWritingSomewhereElse) {
  // Offsets belong to one process image. If the bus is re-mapped, the frame may have moved, and a
  // master that kept writing would put a Safety PDU into whatever now occupies those octets.
  SafeDrive drive;
  ASSERT_TRUE(drive.manager().open(drive.config()).has_value());
  drive.cycles(24);
  FsoeConnection* connection = drive.manager().find(1);
  ASSERT_TRUE(connection->state().bound);

  ASSERT_TRUE(drive.deviceManager().configureProcessData().has_value());
  drive.cycle();

  EXPECT_FALSE(connection->state().bound);
  EXPECT_FALSE(connection->state().inputsValid);

  // Re-opening rebinds against the new image and the connection comes back.
  ASSERT_TRUE(drive.manager().open(drive.config()).has_value());
  drive.manager().find(1)->setDataCommand(mm::etg::FsoeCommand::ProcessData);
  drive.cycles(24);
  EXPECT_EQ(drive.manager().find(1)->state().state, mm::etg::FsoeState::Data);
}

}  // namespace
}  // namespace mm::node
