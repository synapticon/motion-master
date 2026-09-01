#include "node/eni_collector.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "etg/eni.h"
#include "node/device_manager.h"
#include "node/tests/fake_bus.h"

namespace mm::node {
namespace {

using mm::comm::EtherCatState;
using mm::comm::FmmuConfig;
using mm::comm::SlaveConfig;
using mm::comm::SyncManagerConfig;
using mm::node::testing::FakeBus;
using mm::node::testing::makeCia402Bus;

// The same EEPROM image as libs/comm/tests/sii_test.cc, captured from a SOMANET Circulo: a 128-byte
// header then the STRINGS, GENERAL, FMMU and SYNC_M categories. What this test needs from it is the
// GENERAL category's physical-port word (0x0011, two MII ports) and the bootstrap mailbox windows.
constexpr std::array<std::uint8_t, 252> kCirculoSii = {
    0x8D, 0x3E, 0x02, 0x8C, 0xE8, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x58, 0x00,
    0xD2, 0x22, 0x00, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x04, 0x00, 0x14, 0x00, 0x04,
    0x00, 0x10, 0x00, 0x04, 0x00, 0x14, 0x00, 0x04, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x01, 0x00,
    0x0A, 0x00, 0x13, 0x00, 0x02, 0x07, 0x53, 0x4F, 0x4D, 0x41, 0x4E, 0x45, 0x54, 0x1C, 0x53, 0x4F,
    0x4D, 0x41, 0x4E, 0x45, 0x54, 0x20, 0x43, 0x69, 0x72, 0x63, 0x75, 0x6C, 0x6F, 0x20, 0x43, 0x69,
    0x41, 0x34, 0x30, 0x32, 0x20, 0x44, 0x72, 0x69, 0x76, 0x65, 0x1E, 0x00, 0x10, 0x00, 0x01, 0x00,
    0x00, 0x02, 0x00, 0x0F, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x11, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x28, 0x00,
    0x02, 0x00, 0x01, 0x02, 0x03, 0x00, 0x29, 0x00, 0x10, 0x00, 0x00, 0x10, 0x00, 0x04, 0x26, 0x00,
    0x01, 0x01, 0x00, 0x14, 0x00, 0x04, 0x22, 0x00, 0x01, 0x02, 0x00, 0x18, 0x23, 0x00, 0x64, 0x00,
    0x01, 0x03, 0x00, 0x1C, 0x2F, 0x00, 0x20, 0x00, 0x01, 0x04, 0xFF, 0xFF,
};

constexpr std::uint16_t kStation = 0x1001;
constexpr std::uint16_t kRegAlControl = 0x0120;
constexpr std::uint16_t kRegSyncManager0 = 0x0800;
constexpr std::uint16_t kRegFmmu0 = 0x0600;

EniCollectorOptions options() {
  EniCollectorOptions opts;
  opts.sourceMac = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
  opts.cycleTimeUs = 1000;
  return opts;
}

/// The static configuration a master programs for one Circulo, as `GET /api/bus-config` reports it
/// once the bus is mapped: four sync managers, and one FMMU per direction.
SlaveConfig circuloConfig() {
  SlaveConfig config;
  config.slavePosition = 1;
  config.configuredAddress = kStation;
  config.outputBits = 48;
  config.inputBits = 48;
  config.mailbox.writeOffset = 0x1000;
  config.mailbox.writeLength = 0x400;
  config.mailbox.readOffset = 0x1400;
  config.mailbox.readLength = 0x400;
  config.mailbox.protocols = 0x0C;  // CoE + FoE, as the Circulo advertises.
  config.syncManagers = {
      SyncManagerConfig{
          .index = 0, .physicalStart = 0x1000, .length = 0x400, .flags = 0x10026, .type = 1},
      SyncManagerConfig{
          .index = 1, .physicalStart = 0x1400, .length = 0x400, .flags = 0x10022, .type = 2},
      SyncManagerConfig{
          .index = 2, .physicalStart = 0x1800, .length = 6, .flags = 0x10064, .type = 3},
      SyncManagerConfig{
          .index = 3, .physicalStart = 0x1C00, .length = 6, .flags = 0x10020, .type = 4},
  };
  config.fmmus = {
      FmmuConfig{.index = 0,
                 .logicalStart = 0,
                 .length = 6,
                 .logicalStartBit = 0,
                 .logicalEndBit = 7,
                 .physicalStart = 0x1800,
                 .physicalStartBit = 0,
                 .type = 2,  // Write enable: the outputs.
                 .active = 1},
      FmmuConfig{.index = 1,
                 .logicalStart = 6,
                 .length = 6,
                 .logicalStartBit = 0,
                 .logicalEndBit = 7,
                 .physicalStart = 0x1C00,
                 .physicalStartBit = 0,
                 .type = 1,  // Read enable: the inputs.
                 .active = 1},
  };
  return config;
}

std::unique_ptr<FakeBus> circuloBus() {
  auto bus = makeCia402Bus();
  bus->slaveConfigs = {circuloConfig()};
  bus->siiImage.assign(kCirculoSii.begin(), kCirculoSii.end());
  bus->deviceName = "SOMANET Circulo CiA402 Drive";
  return bus;
}

/// Brings a manager to the state the collector requires: scanned, with an enumerated object
/// dictionary and a published image.
///
/// The dictionary is what gives the process image its variable names and types. Without it the
/// collector still produces a document, with the objects addressed rather than named.
void bringUp(DeviceManager& manager, std::unique_ptr<FakeBus> bus) {
  const auto slaves = static_cast<std::uint16_t>(bus->slaves);
  ASSERT_TRUE(manager.init(std::move(bus)).has_value());
  ASSERT_TRUE(manager.scan().has_value());
  for (std::uint16_t position = 1; position <= slaves; ++position) {
    ASSERT_TRUE(manager.initializeDeviceParameters(position, false).has_value());
  }
  ASSERT_TRUE(manager.configureProcessData().has_value());
  ASSERT_TRUE(manager.processImageInfo().configured);
}

const mm::etg::EniEcatCmd* findWrite(const mm::etg::EniSlave& slave, mm::etg::EniTransition at,
                                     std::uint16_t ado) {
  const auto match = [&](const mm::etg::EniEcatCmd& cmd) {
    return cmd.ado == ado && !cmd.transitions.empty() && cmd.transitions.front() == at &&
           !cmd.data.empty();
  };
  const auto it = std::ranges::find_if(slave.initCmds, match);
  return it == slave.initCmds.end() ? nullptr : &*it;
}

TEST(EniCollectorTest, RefusesABusWithNoPublishedImage) {
  DeviceManager manager;
  auto bus = circuloBus();
  ASSERT_TRUE(manager.init(std::move(bus)).has_value());
  ASSERT_TRUE(manager.scan().has_value());

  const auto collected = collectEni(manager, options());
  ASSERT_FALSE(collected.has_value());
  EXPECT_NE(collected.error().find("no published process image"), std::string::npos)
      << collected.error();
}

TEST(EniCollectorTest, RefusesASourceMacThatIsNotSixBytes) {
  DeviceManager manager;
  bringUp(manager, circuloBus());

  EniCollectorOptions opts = options();
  opts.sourceMac = {0x02, 0x00};
  const auto collected = collectEni(manager, opts);
  ASSERT_FALSE(collected.has_value());
  EXPECT_NE(collected.error().find("source MAC is 2 bytes"), std::string::npos)
      << collected.error();
}

TEST(EniCollectorTest, TakesIdentityAndAddressesFromTheBus) {
  DeviceManager manager;
  bringUp(manager, circuloBus());

  const auto collected = collectEni(manager, options());
  ASSERT_TRUE(collected.has_value()) << collected.error();
  ASSERT_EQ(collected->network.slaves.size(), 1u);
  const mm::etg::EniSlaveInfo& info = collected->network.slaves[0].info;
  EXPECT_EQ(info.physAddr, kStation);
  EXPECT_EQ(info.autoIncAddr, 0);  // First device on the ring.
  EXPECT_EQ(info.physics, "YY");   // Two MII ports, from the SII physical-port word.
  EXPECT_EQ(collected->network.master.destination,
            std::vector<std::uint8_t>(6, 0xFF));  // Broadcast.
}

TEST(EniCollectorTest, CountsTheAutoIncrementAddressDownAlongTheRing) {
  auto bus = circuloBus();
  bus->slaves = 2;
  SlaveConfig second = circuloConfig();
  second.slavePosition = 2;
  second.configuredAddress = 0x1002;
  bus->slaveConfigs.push_back(second);
  bus->layout.slaves.push_back(mm::comm::SlaveIo{
      .slavePosition = 2, .outputOffset = 6, .outputBytes = 6, .inputOffset = 6, .inputBytes = 6});
  bus->layout.outputBytes = 12;
  bus->layout.inputBytes = 12;

  DeviceManager manager;
  bringUp(manager, std::move(bus));

  const auto collected = collectEni(manager, options());
  ASSERT_TRUE(collected.has_value()) << collected.error();
  ASSERT_EQ(collected->network.slaves.size(), 2u);
  EXPECT_EQ(collected->network.slaves[0].info.autoIncAddr, 0x0000);
  EXPECT_EQ(collected->network.slaves[1].info.autoIncAddr, 0xFFFF);
}

TEST(EniCollectorTest, ProgramsMailboxSyncManagersBeforePreOpAndProcessDataOnesAtSafeOp) {
  DeviceManager manager;
  bringUp(manager, circuloBus());

  const auto collected = collectEni(manager, options());
  ASSERT_TRUE(collected.has_value()) << collected.error();
  const mm::etg::EniSlave& slave = collected->network.slaves[0];

  // The mailbox has to work in PRE-OP for the CoE downloads to reach the device, so SM0 and SM1 go
  // in at the INIT-to-PRE-OP transition and SM2 and SM3 wait for PRE-OP to SAFE-OP.
  EXPECT_NE(findWrite(slave, mm::etg::EniTransition::IP, kRegSyncManager0), nullptr);
  EXPECT_NE(findWrite(slave, mm::etg::EniTransition::IP, kRegSyncManager0 + 8), nullptr);
  EXPECT_NE(findWrite(slave, mm::etg::EniTransition::PS, kRegSyncManager0 + 16), nullptr);
  EXPECT_NE(findWrite(slave, mm::etg::EniTransition::PS, kRegSyncManager0 + 24), nullptr);
  EXPECT_EQ(findWrite(slave, mm::etg::EniTransition::IP, kRegSyncManager0 + 16), nullptr);
}

TEST(EniCollectorTest, EncodesASyncManagerAsTheEightBytesItsRegisterTakes) {
  DeviceManager manager;
  bringUp(manager, circuloBus());

  const auto collected = collectEni(manager, options());
  ASSERT_TRUE(collected.has_value()) << collected.error();
  const mm::etg::EniEcatCmd* sm2 =
      findWrite(collected->network.slaves[0], mm::etg::EniTransition::PS, kRegSyncManager0 + 16);
  ASSERT_NE(sm2, nullptr);
  // Physical start 0x1800, length 6, control 0x64 from flags, status 0, activate 1, PDI 0.
  const std::vector<std::uint8_t> expected = {0x00, 0x18, 0x06, 0x00, 0x64, 0x00, 0x01, 0x00};
  EXPECT_EQ(sm2->data, expected);
  EXPECT_EQ(sm2->adp, kStation);
  EXPECT_EQ(sm2->cmd, mm::etg::EniCmd::Fpwr);
}

TEST(EniCollectorTest, EncodesAnFmmuAsTheSixteenBytesItsRegisterTakes) {
  DeviceManager manager;
  bringUp(manager, circuloBus());

  const auto collected = collectEni(manager, options());
  ASSERT_TRUE(collected.has_value()) << collected.error();
  const mm::etg::EniEcatCmd* fmmu1 =
      findWrite(collected->network.slaves[0], mm::etg::EniTransition::PS, kRegFmmu0 + 16);
  ASSERT_NE(fmmu1, nullptr);
  // Logical start 6, length 6, bits 0..7, physical start 0x1C00, bit 0, read enable, active.
  const std::vector<std::uint8_t> expected = {0x06, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x07,
                                              0x00, 0x1C, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00};
  EXPECT_EQ(fmmu1->data, expected);
}

TEST(EniCollectorTest, ClearsTheRegisterBlocksOnceForTheWholeBus) {
  DeviceManager manager;
  bringUp(manager, circuloBus());

  const auto collected = collectEni(manager, options());
  ASSERT_TRUE(collected.has_value()) << collected.error();
  // A device's own commands assume an empty block, so the clears are the master's and run first.
  ASSERT_FALSE(collected->network.master.initCmds.empty());
  for (const mm::etg::EniEcatCmd& cmd : collected->network.master.initCmds) {
    EXPECT_EQ(cmd.cmd, mm::etg::EniCmd::Bwr);
    EXPECT_TRUE(cmd.beforeSlave);
    EXPECT_TRUE(cmd.dataLength.has_value());
  }
}

TEST(EniCollectorTest, WalksTheDeviceUpOneTransitionAtATime) {
  DeviceManager manager;
  bringUp(manager, circuloBus());

  const auto collected = collectEni(manager, options());
  ASSERT_TRUE(collected.has_value()) << collected.error();
  const mm::etg::EniSlave& slave = collected->network.slaves[0];
  EXPECT_NE(findWrite(slave, mm::etg::EniTransition::IP, kRegAlControl), nullptr);
  EXPECT_NE(findWrite(slave, mm::etg::EniTransition::PS, kRegAlControl), nullptr);
  EXPECT_NE(findWrite(slave, mm::etg::EniTransition::SO, kRegAlControl), nullptr);

  // Each request is followed by a read that the master repeats until the device agrees. The mask
  // covers the state nibble only, so the error indicator cannot make a reached state read as
  // unreached.
  const auto checks = std::ranges::count_if(
      slave.initCmds, [](const mm::etg::EniEcatCmd& cmd) { return cmd.validate.has_value(); });
  EXPECT_GE(checks, 4);
  for (const mm::etg::EniEcatCmd& cmd : slave.initCmds) {
    if (cmd.validate.has_value()) {
      EXPECT_EQ(cmd.validate->dataMask, std::vector<std::uint8_t>({0x0F, 0x00}));
    }
  }
}

TEST(EniCollectorTest, SplitsTheCyclicFrameIntoALogicalWriteAndALogicalRead) {
  DeviceManager manager;
  bringUp(manager, circuloBus());

  const auto collected = collectEni(manager, options());
  ASSERT_TRUE(collected.has_value()) << collected.error();
  ASSERT_TRUE(collected->network.cyclic.has_value());
  ASSERT_EQ(collected->network.cyclic->frames.size(), 1u);
  const auto& cmds = collected->network.cyclic->frames[0].cmds;
  ASSERT_EQ(cmds.size(), 2u);

  // A read-write datagram would only be right where a device's two FMMUs share a logical address.
  EXPECT_EQ(cmds[0].cmd, mm::etg::EniCmd::Lwr);
  EXPECT_EQ(cmds[0].addr, 0u);
  EXPECT_EQ(cmds[0].dataLength, 6u);
  EXPECT_EQ(cmds[1].cmd, mm::etg::EniCmd::Lrd);
  EXPECT_EQ(cmds[1].addr, 6u);
  EXPECT_EQ(cmds[1].dataLength, 6u);
  EXPECT_EQ(collected->network.cyclic->cycleTimeUs, 1000u);
}

TEST(EniCollectorTest, NamesEveryMappedObjectInTheProcessImageWithItsIecType) {
  DeviceManager manager;
  bringUp(manager, circuloBus());

  const auto collected = collectEni(manager, options());
  ASSERT_TRUE(collected.has_value()) << collected.error();
  ASSERT_TRUE(collected->network.processImage.has_value());
  ASSERT_TRUE(collected->network.processImage->outputs.has_value());
  const auto& outputs = *collected->network.processImage->outputs;
  EXPECT_EQ(outputs.byteSize, 6u);
  ASSERT_EQ(outputs.variables.size(), 2u);
  // 0x6040 is a UINT16 and 0x607A an INT32, so the ESI spellings are UINT and DINT.
  EXPECT_EQ(outputs.variables[0].dataType, "UINT");
  EXPECT_EQ(outputs.variables[0].bitSize, 16u);
  EXPECT_EQ(outputs.variables[0].bitOffs, 0u);
  EXPECT_EQ(outputs.variables[1].dataType, "DINT");
  EXPECT_EQ(outputs.variables[1].bitOffs, 16u);
}

TEST(EniCollectorTest, ReproducesThePdoAssignmentAsCoeDownloads) {
  DeviceManager manager;
  bringUp(manager, circuloBus());

  const auto collected = collectEni(manager, options());
  ASSERT_TRUE(collected.has_value()) << collected.error();
  ASSERT_TRUE(collected->network.slaves[0].mailbox.has_value());
  const auto& coe = collected->network.slaves[0].mailbox->coeInitCmds;
  ASSERT_FALSE(coe.empty());

  // The count is cleared, the entries are written, then the count is written back — the order the
  // standard requires and a device enforces.
  const auto first1C12 =
      std::ranges::find_if(coe, [](const mm::etg::EniCoeCmd& cmd) { return cmd.index == 0x1C12; });
  ASSERT_NE(first1C12, coe.end());
  EXPECT_EQ(first1C12->subindex, 0);
  EXPECT_EQ(first1C12->data, std::vector<std::uint8_t>({0x00}));
  EXPECT_EQ(first1C12->ccs, mm::etg::EniCoeCommandSpecifier::Download);

  const bool assigns1600 = std::ranges::any_of(coe, [](const mm::etg::EniCoeCmd& cmd) {
    return cmd.index == 0x1C12 && cmd.subindex == 1 &&
           cmd.data == std::vector<std::uint8_t>({0x00, 0x16});
  });
  EXPECT_TRUE(assigns1600);
}

TEST(EniCollectorTest, TakesTheBootstrapMailboxFromTheSii) {
  DeviceManager manager;
  bringUp(manager, circuloBus());

  const auto collected = collectEni(manager, options());
  ASSERT_TRUE(collected.has_value()) << collected.error();
  const auto& mailbox = *collected->network.slaves[0].mailbox;
  ASSERT_TRUE(mailbox.bootstrapSend.has_value());
  EXPECT_EQ(mailbox.bootstrapSend->start, 0x1000);
  EXPECT_EQ(mailbox.bootstrapSend->length, 0x400);
  EXPECT_EQ(mailbox.protocols,
            std::vector<mm::etg::EniMailboxProtocol>(
                {mm::etg::EniMailboxProtocol::Coe, mm::etg::EniMailboxProtocol::Foe}));
}

TEST(EniCollectorTest, WarnsAndLeavesOutWhatTheSiiWouldHaveAnswered) {
  auto bus = circuloBus();
  bus->siiError = "EEPROM read failed";

  DeviceManager manager;
  bringUp(manager, std::move(bus));

  const auto collected = collectEni(manager, options());
  ASSERT_TRUE(collected.has_value()) << collected.error();
  // A missing SII costs the port layout and the bootstrap mailbox. Everything else, including every
  // init command, is intact, so the document still brings the bus up.
  EXPECT_TRUE(collected->network.slaves[0].info.physics.empty());
  EXPECT_FALSE(collected->network.slaves[0].mailbox->bootstrapSend.has_value());
  EXPECT_FALSE(collected->network.slaves[0].initCmds.empty());
  ASSERT_EQ(collected->warnings.size(), 1u);
  EXPECT_NE(collected->warnings[0].find("SII will not read"), std::string::npos)
      << collected->warnings[0];
}

// Writes the collected document where the schema-validation test finds it. Passing the writer's own
// checks is not the same as conforming, so xmllint against the ENI XML Schema is what proves a
// collected bus produces a document a third-party master will parse.
TEST(EniCollectorTest, WritesTheCollectedDocumentForSchemaValidation) {
  DeviceManager manager;
  bringUp(manager, circuloBus());

  const auto collected = collectEni(manager, options());
  ASSERT_TRUE(collected.has_value()) << collected.error();
  const auto eni = mm::etg::writeEni(collected->network);
  ASSERT_TRUE(eni.has_value()) << eni.error();

  const std::filesystem::path path =
      std::filesystem::path(MM_NODE_TEST_OUTPUT_DIR) / "eni_collected.xml";
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(out.is_open()) << "cannot write " << path.string();
  out << *eni;
  out.close();
  EXPECT_TRUE(std::filesystem::exists(path));
}

}  // namespace
}  // namespace mm::node
