#include "etg/eni_reader.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "etg/eni.h"
#include "etg/tests/eni_fixtures.h"

namespace mm::etg {
namespace {

using mm::etg::testing::referenceNetwork;

// The four documents ETG ships as ENI samples. They are the only files here nobody on this project
// wrote, which makes them the only real test of a reader: a round trip proves the reader inverts
// our writer and nothing more. ETG's download terms forbid redistributing them, so they are not in
// the repository — drop them in the gitignored directory below and these tests turn themselves on.
constexpr std::array<std::string_view, 4> kSamples = {"complex.xml", "eval32input.xml",
                                                      "eval32output.xml", "mailboxDevice.xml"};

std::filesystem::path samplePath(std::string_view name) {
  return std::filesystem::path(MM_ETG_TEST_DATA_DIR) / "eni" / "samples" / name;
}

/// Reads a sample, or skips the test naming the path it wanted.
std::string loadSample(std::string_view name) {
  const std::filesystem::path path = samplePath(name);
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

bool samplesPresent() {
  return std::ranges::all_of(kSamples,
                             [](std::string_view name) { return exists(samplePath(name)); });
}

#define SKIP_WITHOUT_SAMPLES()                                                                \
  do {                                                                                        \
    if (!samplesPresent()) {                                                                  \
      GTEST_SKIP() << "the ETG ENI samples are not present under " << samplePath("").string() \
                   << " — see cmake/schema_validation.cmake for why they are not committed";  \
    }                                                                                         \
  } while (false)

TEST(EniReaderTest, RejectsWhatIsNotAnEniDocument) {
  EXPECT_FALSE(readEni("not xml at all <<<").has_value());
  const auto wrongRoot = readEni(R"(<?xml version="1.0"?><EtherCATInfo/>)");
  ASSERT_FALSE(wrongRoot.has_value());
  EXPECT_NE(wrongRoot.error().find("root element"), std::string::npos) << wrongRoot.error();
  const auto noConfig = readEni(R"(<?xml version="1.0"?><EtherCATConfig/>)");
  ASSERT_FALSE(noConfig.has_value());
  EXPECT_NE(noConfig.error().find("no <Config>"), std::string::npos) << noConfig.error();
}

TEST(EniReaderTest, ReadsBackEverythingTheWriterWrote) {
  const auto written = writeEni(referenceNetwork());
  ASSERT_TRUE(written.has_value()) << written.error();
  const auto read = readEni(*written);
  ASSERT_TRUE(read.has_value()) << read.error();
  EXPECT_TRUE(read->warnings.empty()) << read->warnings.front();

  // The strongest statement available: write, read, write again, and the two documents match. It
  // covers every field the model holds without asserting them one by one.
  const auto rewritten = writeEni(read->network);
  ASSERT_TRUE(rewritten.has_value()) << rewritten.error();
  EXPECT_EQ(*rewritten, *written);
}

TEST(EniReaderTest, KeepsTheValuesAReaderWouldBeAskedAbout) {
  const auto written = writeEni(referenceNetwork());
  ASSERT_TRUE(written.has_value()) << written.error();
  const auto read = readEni(*written);
  ASSERT_TRUE(read.has_value()) << read.error();
  ASSERT_EQ(read->network.slaves.size(), 2u);

  const EniSlave& first = read->network.slaves[0];
  EXPECT_EQ(first.info.physAddr, 0x1001);
  EXPECT_EQ(first.info.physics, "YY");
  ASSERT_TRUE(first.mailbox.has_value());
  EXPECT_EQ(first.mailbox->send.start, 0x1000);
  EXPECT_FALSE(first.initCmds.empty());
  EXPECT_TRUE(first.previousPorts.empty());  // The first device has no previous device.

  const EniSlave& second = read->network.slaves[1];
  ASSERT_EQ(second.previousPorts.size(), 1u);
  EXPECT_EQ(second.previousPorts[0].port, EniPort::B);
  EXPECT_TRUE(second.previousPorts[0].selected);
  ASSERT_TRUE(second.dc.has_value());
  EXPECT_EQ(second.dc->cycleTime0Ns, 1000000);
}

TEST(EniReaderTest, AcceptsPreviousPortAWhichTheWriterRefuses) {
  // ETG.2100 Table 29 allows A to D; ENI Schema 1.7 enumerates B, C and D. The reader takes what
  // the file says, which is the whole point of reading a file somebody else wrote.
  const auto read = readEni(R"(<?xml version="1.0"?><EtherCATConfig><Config><Master><Info>)"
                            R"(<Name>M</Name><Destination>FFFFFFFFFFFF</Destination>)"
                            R"(<Source>020000000001</Source></Info></Master><Slave><Info>)"
                            R"(<Name>D</Name><PhysAddr>4098</PhysAddr><AutoIncAddr>65535)"
                            R"(</AutoIncAddr><Physics>YY</Physics><VendorId>1</VendorId>)"
                            R"(<ProductCode>1</ProductCode><RevisionNo>1</RevisionNo>)"
                            R"(<SerialNo>0</SerialNo></Info><PreviousPort Selected="1">)"
                            R"(<Port>A</Port></PreviousPort></Slave></Config></EtherCATConfig>)");
  ASSERT_TRUE(read.has_value()) << read.error();
  ASSERT_EQ(read->network.slaves.size(), 1u);
  ASSERT_EQ(read->network.slaves[0].previousPorts.size(), 1u);
  EXPECT_EQ(read->network.slaves[0].previousPorts[0].port, EniPort::A);

  // And the writer still refuses to emit it, so the asymmetry is the design rather than a gap.
  EXPECT_FALSE(writeEni(read->network).has_value());
}

TEST(EniReaderTest, NamesAnElementItDoesNotModelRatherThanDroppingItSilently) {
  const auto read = readEni(R"(<?xml version="1.0"?><EtherCATConfig><Config><Master><Info>)"
                            R"(<Name>M</Name><Destination>FFFFFFFFFFFF</Destination>)"
                            R"(<Source>020000000001</Source></Info><MailboxStates>)"
                            R"(<StartAddr>262144</StartAddr><Count>4</Count></MailboxStates>)"
                            R"(</Master></Config></EtherCATConfig>)");
  ASSERT_TRUE(read.has_value()) << read.error();
  ASSERT_EQ(read->warnings.size(), 1u);
  EXPECT_NE(read->warnings[0].find("MailboxStates"), std::string::npos) << read->warnings[0];
  EXPECT_NE(read->warnings[0].find("not modelled"), std::string::npos) << read->warnings[0];
}

TEST(EniReaderTest, ReadsEverySampleEtgShips) {
  SKIP_WITHOUT_SAMPLES();
  for (const std::string_view name : kSamples) {
    const auto read = readEni(loadSample(name));
    ASSERT_TRUE(read.has_value()) << name << ": " << read.error();
    EXPECT_FALSE(read->network.slaves.empty()) << name << " yielded no devices";
    EXPECT_EQ(read->network.master.destination.size(), 6u) << name;
    EXPECT_EQ(read->network.master.source.size(), 6u) << name;
  }
}

TEST(EniReaderTest, ReadsTheNineDeviceSampleWholeWithItsInitCommands) {
  SKIP_WITHOUT_SAMPLES();
  const auto read = readEni(loadSample("complex.xml"));
  ASSERT_TRUE(read.has_value()) << read.error();
  EXPECT_EQ(read->network.slaves.size(), 9u);

  // Every device is addressed, and the ring counts down from the first.
  EXPECT_EQ(read->network.slaves[0].info.physAddr, 1001);
  EXPECT_EQ(read->network.slaves[0].info.autoIncAddr, 0);
  EXPECT_EQ(read->network.slaves[8].info.physAddr, 1009);
  EXPECT_EQ(read->network.slaves[8].info.autoIncAddr, 65528);

  // 161 init commands in the file, and the split is the point: 155 are EtherCAT datagrams and 6 are
  // CoE downloads, which live under a device's mailbox rather than beside its datagrams.
  std::size_t datagrams = read->network.master.initCmds.size();
  std::size_t coeTransfers = 0;
  for (const EniSlave& slave : read->network.slaves) {
    datagrams += slave.initCmds.size();
    if (slave.mailbox.has_value()) {
      coeTransfers += slave.mailbox->coeInitCmds.size();
    }
  }
  EXPECT_EQ(datagrams, 155u);
  EXPECT_EQ(coeTransfers, 6u);

  // The cyclic frame is a logical read and a logical write, not one read-write.
  ASSERT_TRUE(read->network.cyclic.has_value());
  ASSERT_EQ(read->network.cyclic->frames.size(), 1u);
  EXPECT_EQ(read->network.cyclic->frames[0].cmds.size(), 3u);
  EXPECT_EQ(read->network.cyclic->frames[0].cmds[0].cmd, EniCmd::Lrd);
  EXPECT_EQ(read->network.cyclic->frames[0].cmds[1].cmd, EniCmd::Lwr);
}

TEST(EniReaderTest, ReadsTheCoeInitCommandsOfTheSample) {
  SKIP_WITHOUT_SAMPLES();
  const auto read = readEni(loadSample("complex.xml"));
  ASSERT_TRUE(read.has_value()) << read.error();
  const auto withCoe = std::ranges::find_if(read->network.slaves, [](const EniSlave& slave) {
    return slave.mailbox.has_value() && !slave.mailbox->coeInitCmds.empty();
  });
  ASSERT_NE(withCoe, read->network.slaves.end());
  const EniCoeCmd& first = withCoe->mailbox->coeInitCmds.front();
  // Every one of them carries a payload, which is what settles the Ccs question: an upload has no
  // payload, so a Ccs of 1 in these files is a download, as ETG.1000.6 says and ETG.2100 does not.
  EXPECT_EQ(first.ccs, EniCoeCommandSpecifier::Download);
  EXPECT_FALSE(first.data.empty());
}

TEST(EniReaderTest, ReadsAMailboxDeviceWithItsProtocolsAndWindows) {
  SKIP_WITHOUT_SAMPLES();
  const auto read = readEni(loadSample("mailboxDevice.xml"));
  ASSERT_TRUE(read.has_value()) << read.error();
  ASSERT_FALSE(read->network.slaves.empty());
  const auto withMailbox = std::ranges::find_if(
      read->network.slaves, [](const EniSlave& slave) { return slave.mailbox.has_value(); });
  ASSERT_NE(withMailbox, read->network.slaves.end());
  EXPECT_GT(withMailbox->mailbox->send.length, 0);
  EXPECT_GT(withMailbox->mailbox->recv.length, 0);
}

TEST(EniReaderTest, ReadsASampleThatOmitsElementsItsOwnSchemaRequires) {
  SKIP_WITHOUT_SAMPLES();
  // eval32output.xml has no <AutoIncAddr> and no <Physics>, both of which ENI Schema 1.7 marks
  // mandatory, so the file does not validate against it. A reader must not care.
  const auto read = readEni(loadSample("eval32output.xml"));
  ASSERT_TRUE(read.has_value()) << read.error();
  ASSERT_EQ(read->network.slaves.size(), 1u);
  EXPECT_TRUE(read->network.slaves[0].info.physics.empty());
  EXPECT_FALSE(read->network.slaves[0].initCmds.empty());
}

}  // namespace
}  // namespace mm::etg
