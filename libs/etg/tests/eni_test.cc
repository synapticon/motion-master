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

#include "etg/tests/eni_fixtures.h"

namespace mm::etg {
namespace {

using mm::etg::testing::referenceNetwork;

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

TEST(EniWriterTest, WritesPreviousPortWithItsSelectedAttribute) {
  const auto eni = writeEni(referenceNetwork());
  ASSERT_TRUE(eni.has_value()) << eni.error();
  // The only attribute this writer emits. Without it a master cannot tell the port the device is
  // plugged into from a port it could be moved to.
  EXPECT_NE(eni->find(R"(<PreviousPort Selected="1">)"), std::string::npos);
  EXPECT_NE(eni->find("<Port>B</Port>"), std::string::npos);
}

TEST(EniWriterTest, RefusesPreviousPortAWhichTheSchemaDoesNotEnumerate) {
  EniNetwork network = referenceNetwork();
  network.slaves[1].previousPorts[0].port = EniPort::A;
  const auto eni = writeEni(network);
  ASSERT_FALSE(eni.has_value());
  EXPECT_NE(eni.error().find("previous port A"), std::string::npos) << eni.error();
}

TEST(EniWriterTest, WritesTheDcElementInSchemaOrder) {
  const auto eni = writeEni(referenceNetwork());
  ASSERT_TRUE(eni.has_value()) << eni.error();
  const std::array<std::string_view, 5> order = {"<PotentialReferenceClock>", "<ReferenceClock>",
                                                 "<CycleTime0>", "<CycleTime1>", "<ShiftTime>"};
  std::size_t previous = eni->find("<DC>");
  ASSERT_NE(previous, std::string::npos);
  for (const std::string_view element : order) {
    const std::size_t at = eni->find(element, previous);
    ASSERT_NE(at, std::string::npos) << element << " is missing";
    EXPECT_GT(at, previous) << element << " is out of order";
    previous = at;
  }
}

TEST(EniWriterTest, WritesDcTimesAsTheSignedNanosecondsTheyAre) {
  EniNetwork network = referenceNetwork();
  // ETG.2100 Table 32 makes CycleTime1 a derived figure, `SYNC1 cycle - SYNC0 cycle + SYNC0 shift`,
  // so it can legitimately be negative — which an unsigned write would turn into nonsense.
  network.slaves[1].dc->cycleTime1Ns = -250000;
  const auto eni = writeEni(network);
  ASSERT_TRUE(eni.has_value()) << eni.error();
  EXPECT_NE(eni->find("<CycleTime1>-250000</CycleTime1>"), std::string::npos);
}

TEST(EniWriterTest, WritesNoPreviousPortOrDcForADeviceThatHasNeither) {
  EniNetwork network = referenceNetwork();
  network.slaves.resize(1);  // The first device on a bus has no previous device.
  const auto eni = writeEni(network);
  ASSERT_TRUE(eni.has_value()) << eni.error();
  EXPECT_EQ(eni->find("<PreviousPort"), std::string::npos);
  EXPECT_EQ(eni->find("<DC>"), std::string::npos);
}

TEST(EniWriterTest, WritesAPdoWithItsSyncManagerAndEntries) {
  const auto eni = writeEni(referenceNetwork());
  ASSERT_TRUE(eni.has_value()) << eni.error();
  // The Sm attribute is what tells a master this PDO belongs in the process image.
  EXPECT_NE(eni->find(R"(<RxPdo Sm="2">)"), std::string::npos);
  EXPECT_NE(eni->find(R"(<TxPdo Sm="3">)"), std::string::npos);
  // An object index is a HexDecValue, so it is written the way every ETG document writes one.
  EXPECT_NE(eni->find("<Index>#x1600</Index>"), std::string::npos);
  EXPECT_NE(eni->find("<Index>#x6040</Index>"), std::string::npos);
}

TEST(EniWriterTest, WritesAPaddingEntryWithNeitherNameNorType) {
  const auto eni = writeEni(referenceNetwork());
  ASSERT_TRUE(eni.has_value()) << eni.error();
  // Padding occupies bits and addresses nothing, so ETG.2100's "mandatory if Index != 0" does not
  // reach it. Inventing a name for one would be inventing an object.
  const std::size_t padding = eni->find("<Index>#x0000</Index>\n            <SubIndex>");
  ASSERT_NE(padding, std::string::npos);
  const std::size_t entryEnd = eni->find("</Entry>", padding);
  ASSERT_NE(entryEnd, std::string::npos);
  const std::string entry = eni->substr(padding, entryEnd - padding);
  EXPECT_EQ(entry.find("<Name>"), std::string::npos) << entry;
  EXPECT_EQ(entry.find("<DataType>"), std::string::npos) << entry;
}

TEST(EniWriterTest, RefusesAPdoWithNoName) {
  EniNetwork network = referenceNetwork();
  network.slaves[0].processData->rxPdos[0].name.clear();
  const auto eni = writeEni(network);
  ASSERT_FALSE(eni.has_value());
  EXPECT_NE(eni.error().find("has no name"), std::string::npos) << eni.error();
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
