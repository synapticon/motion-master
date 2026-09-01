#include "etg/eni.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mm::etg {
namespace {

// The reference network below mirrors one real bus: a single SOMANET Circulo in position 1, read
// off the bench with `GET /api/bus-config` and `GET /api/devices/1/sii`. Measured values rather
// than invented ones mean the document this test writes is also what the schema-validation test
// checks, and a reader can hold it against hardware.
constexpr std::uint32_t kSynapticonVendorId = 8914;
constexpr std::uint32_t kCirculoProductCode = 769;
constexpr std::uint32_t kCirculoRevision = 285212674;
constexpr std::uint16_t kFirstStationAddress = 0x1001;

/// ESC register offsets the init commands address (ETG.1000.4, register description).
constexpr std::uint16_t kRegAlControl = 0x0120;
constexpr std::uint16_t kRegAlStatus = 0x0130;
constexpr std::uint16_t kRegSyncManager0 = 0x0800;
constexpr std::uint16_t kRegFmmu0 = 0x0600;

/// AL states as the AL Control register carries them.
constexpr std::uint8_t kAlStatePreOp = 0x02;
constexpr std::uint8_t kAlStateSafeOp = 0x04;
constexpr std::uint8_t kAlStateOp = 0x08;

EniEcatCmd writeRegister(EniTransition transition, std::uint16_t station, std::uint16_t reg,
                         std::vector<std::uint8_t> data, std::string comment) {
  EniEcatCmd command;
  command.transitions = {transition};
  command.comment = std::move(comment);
  command.requirement = EniRequires::Cycle;
  command.cmd = EniCmd::Fpwr;
  command.adp = station;
  command.ado = reg;
  command.data = std::move(data);
  command.cnt = 1;
  command.retries = 3;
  return command;
}

EniEcatCmd checkState(EniTransition transition, std::uint16_t station, std::uint8_t state) {
  EniEcatCmd command;
  command.transitions = {transition};
  command.comment = "check the device reached the state";
  command.requirement = EniRequires::Cycle;
  command.cmd = EniCmd::Fprd;
  command.adp = station;
  command.ado = kRegAlStatus;
  command.dataLength = 2;
  command.cnt = 1;
  command.retries = 3;
  command.validate =
      EniValidate{.data = {state, 0x00}, .dataMask = {0x0F, 0x00}, .timeoutMs = 10000};
  return command;
}

EniSyncManager syncManager(std::uint8_t index, EniSyncManagerType type, std::uint16_t startAddress,
                           std::uint8_t controlByte,
                           std::optional<std::uint32_t> defaultSize = std::nullopt) {
  EniSyncManager manager;
  manager.index = index;
  manager.type = type;
  manager.startAddress = startAddress;
  manager.controlByte = controlByte;
  manager.enable = true;
  manager.defaultSize = defaultSize;
  return manager;
}

EniMailboxWindow mailboxWindow(std::uint16_t start, std::uint16_t length) {
  EniMailboxWindow window;
  window.start = start;
  window.length = length;
  return window;
}

EniCoeCmd coeDownload(std::uint16_t index, std::uint8_t subindex, std::vector<std::uint8_t> data,
                      std::string comment) {
  EniCoeCmd command;
  command.transitions = {EniTransition::PS};
  command.comment = std::move(comment);
  command.timeoutMs = 1000;
  command.ccs = EniCoeCommandSpecifier::Download;
  command.index = index;
  command.subindex = subindex;
  command.data = std::move(data);
  return command;
}

EniVariable variable(std::string name, std::string dataType, std::uint32_t bitSize,
                     std::uint32_t bitOffs) {
  EniVariable entry;
  entry.name = std::move(name);
  entry.dataType = std::move(dataType);
  entry.bitSize = bitSize;
  entry.bitOffs = bitOffs;
  return entry;
}

EniNetwork referenceNetwork() {
  EniNetwork network;
  network.master.name = "Motion Master";
  network.master.destination = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  network.master.source = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};

  EniSlave slave;
  slave.info.name = "SOMANET Circulo CiA402 Drive";
  slave.info.physAddr = kFirstStationAddress;
  slave.info.autoIncAddr = 0;
  slave.info.physics = eniPhysics(0x0011);
  slave.info.vendorId = kSynapticonVendorId;
  slave.info.productCode = kCirculoProductCode;
  slave.info.revisionNo = kCirculoRevision;
  slave.info.serialNo = 0;

  EniProcessData processData;
  processData.send = EniProcessDataWindow{.bitStart = 0, .bitLength = 280};
  processData.recv = EniProcessDataWindow{.bitStart = 0, .bitLength = 376};
  processData.syncManagers = {
      syncManager(0, EniSyncManagerType::MailboxOut, 0x1000, 0x26),
      syncManager(1, EniSyncManagerType::MailboxIn, 0x1400, 0x22),
      syncManager(2, EniSyncManagerType::Outputs, 0x1800, 0x64, 35),
      syncManager(3, EniSyncManagerType::Inputs, 0x1C00, 0x20, 47),
  };
  slave.processData = processData;

  EniMailbox mailbox;
  mailbox.send = mailboxWindow(0x1000, 1024);
  mailbox.recv = mailboxWindow(0x1400, 1024);
  mailbox.bootstrapSend = mailboxWindow(0x1000, 1024);
  mailbox.bootstrapRecv = mailboxWindow(0x1400, 1024);
  mailbox.protocols = {EniMailboxProtocol::Coe, EniMailboxProtocol::Foe};
  mailbox.coeInitCmds = {
      coeDownload(0x1C12, 0, {0x00}, "clear the RxPDO assignment of sync manager 2"),
      coeDownload(0x1C12, 1, {0x00, 0x16}, "assign 0x1600 to sync manager 2"),
  };
  slave.mailbox = mailbox;

  slave.initCmds = {
      writeRegister(EniTransition::IP, kFirstStationAddress, kRegAlControl, {kAlStatePreOp, 0x00},
                    "set the device to PRE-OP"),
      checkState(EniTransition::IP, kFirstStationAddress, kAlStatePreOp),
      writeRegister(EniTransition::PS, kFirstStationAddress, kRegSyncManager0 + 0x10,
                    {0x00, 0x18, 0x23, 0x00, 0x64, 0x00, 0x01, 0x00}, "set sync manager 2"),
      writeRegister(EniTransition::PS, kFirstStationAddress, kRegFmmu0,
                    {0x00, 0x00, 0x00, 0x00, 0x23, 0x00, 0x00, 0x07, 0x00, 0x18, 0x00, 0x00, 0x02,
                     0x00, 0x00, 0x00},
                    "set FMMU 0 for the outputs"),
      writeRegister(EniTransition::PS, kFirstStationAddress, kRegAlControl, {kAlStateSafeOp, 0x00},
                    "set the device to SAFE-OP"),
      checkState(EniTransition::PS, kFirstStationAddress, kAlStateSafeOp),
      writeRegister(EniTransition::SO, kFirstStationAddress, kRegAlControl, {kAlStateOp, 0x00},
                    "set the device to OP"),
      checkState(EniTransition::SO, kFirstStationAddress, kAlStateOp),
  };
  network.slaves.push_back(slave);

  EniCyclicCmd exchange;
  exchange.states = {EniState::SafeOp, EniState::Op};
  exchange.comment = "exchange the whole process image";
  exchange.cmd = EniCmd::Lrw;
  exchange.addr = 0;
  exchange.dataLength = 82;
  exchange.cnt = 3;
  exchange.inputOffs = 0;
  exchange.outputOffs = 0;

  EniCyclic cyclic;
  cyclic.comment = "process data";
  cyclic.cycleTimeUs = 1000;
  cyclic.taskId = "process-data";
  cyclic.frames = {EniFrame{.comment = "one LRW for the whole bus", .cmds = {exchange}}};
  network.cyclic = cyclic;

  EniProcessImage image;
  image.inputs =
      EniProcessImageArea{.byteSize = 35,
                          .variables = {variable("Drive 1.Statusword", "UINT", 16, 0),
                                        variable("Drive 1.Position actual value", "DINT", 32, 16)}};
  image.outputs =
      EniProcessImageArea{.byteSize = 47,
                          .variables = {variable("Drive 1.Controlword", "UINT", 16, 0),
                                        variable("Drive 1.Target position", "DINT", 32, 16)}};
  network.processImage = image;

  return network;
}

TEST(EniPhysicsTest, MapsSiiPortCodesToSchemaCharacters) {
  EXPECT_EQ(eniPhysics(0x0011), "YY");    // Two MII ports, as a Circulo reports.
  EXPECT_EQ(eniPhysics(0x1111), "YYYY");  // Four MII ports.
  EXPECT_EQ(eniPhysics(0x0033), "KK");    // Two EBUS ports.
  EXPECT_EQ(eniPhysics(0x0301), "Y K");   // Port 0 MII, port 1 unused, port 2 EBUS.
  EXPECT_EQ(eniPhysics(0x0004), "B");     // One Fast Hot Connect port.
  EXPECT_EQ(eniPhysics(0x0000), "");      // No port in use.
}

TEST(EniWriterTest, RejectsACommandThatFillsBothHalvesOfTheAddressChoice) {
  EniNetwork network = referenceNetwork();
  network.slaves[0].initCmds[0].addr = 0;
  const auto eni = writeEni(network);
  ASSERT_FALSE(eni.has_value());
  EXPECT_NE(eni.error().find("addr excludes adp and ado"), std::string::npos) << eni.error();
}

TEST(EniWriterTest, RejectsACommandThatFillsBothHalvesOfThePayloadChoice) {
  EniNetwork network = referenceNetwork();
  network.slaves[0].initCmds[0].dataLength = 2;
  const auto eni = writeEni(network);
  ASSERT_FALSE(eni.has_value());
  EXPECT_NE(eni.error().find("data excludes dataLength"), std::string::npos) << eni.error();
}

TEST(EniWriterTest, RejectsAMacThatIsNotSixBytes) {
  EniNetwork network = referenceNetwork();
  network.master.source = {0x02, 0x00};
  const auto eni = writeEni(network);
  ASSERT_FALSE(eni.has_value());
  EXPECT_NE(eni.error().find("source MAC is 2 bytes"), std::string::npos) << eni.error();
}

TEST(EniWriterTest, RejectsARepeatedSyncManagerIndex) {
  EniNetwork network = referenceNetwork();
  network.slaves[0].processData->syncManagers[1].index = 0;
  const auto eni = writeEni(network);
  ASSERT_FALSE(eni.has_value());
  EXPECT_NE(eni.error().find("sync manager index 0 is used twice"), std::string::npos)
      << eni.error();
}

TEST(EniWriterTest, RejectsACyclicCommandThatNamesNoState) {
  EniNetwork network = referenceNetwork();
  network.cyclic->frames[0].cmds[0].states.clear();
  const auto eni = writeEni(network);
  ASSERT_FALSE(eni.has_value());
  EXPECT_NE(eni.error().find("names 0 states"), std::string::npos) << eni.error();
}

TEST(EniWriterTest, RejectsAHalfBootstrapMailbox) {
  EniNetwork network = referenceNetwork();
  network.slaves[0].mailbox->bootstrapRecv.reset();
  const auto eni = writeEni(network);
  ASSERT_FALSE(eni.has_value());
  EXPECT_NE(eni.error().find("bootstrap mailbox needs both"), std::string::npos) << eni.error();
}

TEST(EniWriterTest, WritesSyncManagersInIndexOrderWhateverOrderTheCallerSupplied) {
  EniNetwork network = referenceNetwork();
  std::ranges::reverse(network.slaves[0].processData->syncManagers);
  const auto eni = writeEni(network);
  ASSERT_TRUE(eni.has_value()) << eni.error();
  const std::size_t sm0 = eni->find("<Sm0>");
  const std::size_t sm3 = eni->find("<Sm3>");
  ASSERT_NE(sm0, std::string::npos);
  ASSERT_NE(sm3, std::string::npos);
  EXPECT_LT(sm0, sm3);
}

TEST(EniWriterTest, WritesPayloadBytesAsHexBinaryInWireOrder) {
  const auto eni = writeEni(referenceNetwork());
  ASSERT_TRUE(eni.has_value()) << eni.error();
  // The AL Control write of PRE-OP: the register is little-endian, so 0x0002 is "0200".
  EXPECT_NE(eni->find("<Data>0200</Data>"), std::string::npos);
  // The PDO assignment writes 0x1600 to 0x1C12:01, little-endian again.
  EXPECT_NE(eni->find("<Data>0016</Data>"), std::string::npos);
}

TEST(EniWriterTest, OmitsAnOptionalElementRatherThanWritingZero) {
  EniNetwork network = referenceNetwork();
  network.slaves[0].initCmds[0].cnt.reset();
  network.slaves[0].initCmds[0].retries.reset();
  network.cyclic->frames[0].cmds[0].cnt.reset();
  const auto eni = writeEni(network);
  ASSERT_TRUE(eni.has_value()) << eni.error();
  EXPECT_EQ(eni->find("<Cnt>0</Cnt>"), std::string::npos);
  EXPECT_EQ(eni->find("<Retries>0</Retries>"), std::string::npos);
}

TEST(EniWriterTest, FollowsEtg1000Part6ForTheCoeCommandSpecifier) {
  const auto eni = writeEni(referenceNetwork());
  ASSERT_TRUE(eni.has_value()) << eni.error();
  // A download is 1. ETG.2100 Table 20 says 2; EniCoeCommandSpecifier records why it is not
  // followed here.
  EXPECT_NE(eni->find("<Ccs>1</Ccs>"), std::string::npos);
  EXPECT_EQ(eni->find("<Ccs>2</Ccs>"), std::string::npos);
}

TEST(EniWriterTest, WritesTheElementsOfSlaveInfoInSchemaOrder) {
  const auto eni = writeEni(referenceNetwork());
  ASSERT_TRUE(eni.has_value()) << eni.error();
  const std::array<std::string_view, 8> order = {"<Name>",       "<PhysAddr>", "<AutoIncAddr>",
                                                 "<Physics>",    "<VendorId>", "<ProductCode>",
                                                 "<RevisionNo>", "<SerialNo>"};
  std::size_t previous = 0;
  for (const std::string_view element : order) {
    const std::size_t at = eni->find(element, previous);
    ASSERT_NE(at, std::string::npos) << element << " is missing";
    EXPECT_GT(at, previous) << element << " is out of order";
    previous = at;
  }
}

// Writes the reference document where the schema-validation test finds it. That test runs xmllint
// against the ENI XML Schema and is the only machine check that this writer produces a conformant
// document. The assertions above only cover the parts a reader would check by hand.
TEST(EniWriterTest, WritesTheReferenceDocumentForSchemaValidation) {
  const auto eni = writeEni(referenceNetwork());
  ASSERT_TRUE(eni.has_value()) << eni.error();
  const std::filesystem::path path =
      std::filesystem::path(MM_ETG_TEST_OUTPUT_DIR) / "eni_reference.xml";
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(out.is_open()) << "cannot write " << path.string();
  out << *eni;
  out.close();
  EXPECT_TRUE(std::filesystem::exists(path));
}

}  // namespace
}  // namespace mm::etg
