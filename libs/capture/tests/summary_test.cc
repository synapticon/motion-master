#include "capture/summary.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <nlohmann/json.hpp>
#include <utility>
#include <vector>

#include "test_frames.h"

namespace mm::capture {
namespace {

constexpr int64_t kMs = 1'000'000;

Record at(int64_t ns, std::vector<uint8_t> bytes) { return Record{ns, std::move(bytes)}; }

TEST(SummaryTest, TimesEachReturnFromItsSendAndSortsIntoCumulativeBands) {
  const test::DatagramSpec sdo{4, 3, 0x1001, 0x1000, 8};
  std::vector<Record> records;
  // 0.3 ms: in no band.
  records.push_back(at(0, test::frame({sdo}, true)));
  records.push_back(at(300'000, test::frame({sdo}, false)));
  // 25 ms: over 2 and over 20.
  records.push_back(at(10 * kMs, test::frame({sdo}, true)));
  records.push_back(at(35 * kMs, test::frame({sdo}, false)));
  // 150 ms: in all three, and the latest.
  records.push_back(at(100 * kMs, test::frame({sdo}, true)));
  records.push_back(at(250 * kMs, test::frame({sdo}, false)));

  const Summary s = summarise(records);
  EXPECT_EQ(s.frames, 6u);
  EXPECT_EQ(s.control.sent, 3u);
  EXPECT_EQ(s.control.returned, 3u);
  EXPECT_EQ(s.control.over2Ms, 2u);
  EXPECT_EQ(s.control.over20Ms, 2u);
  EXPECT_EQ(s.control.over100Ms, 1u);
  EXPECT_EQ(s.control.latestUs, 150'000);
  EXPECT_EQ(s.control.lost, 0u);
  EXPECT_EQ(s.cyclic.sent, 0u);
}

TEST(SummaryTest, ASendWithNoReturnIsLost) {
  const test::DatagramSpec sdo{4, 3, 0x1001, 0x1000, 8};
  std::vector<Record> records;
  records.push_back(at(0, test::frame({sdo}, true)));
  const Summary s = summarise(records);
  EXPECT_EQ(s.control.sent, 1u);
  EXPECT_EQ(s.control.lost, 1u);
  EXPECT_EQ(s.control.returned, 0u);
}

TEST(SummaryTest, AFrameSentAgainCountsOnceAndIsTimedFromItsFirstCopy) {
  const test::DatagramSpec reg{4, 9, 0x1001, 0x0130, 2};
  std::vector<Record> records;
  records.push_back(at(0, test::frame({reg}, true)));
  records.push_back(at(2 * kMs, test::frame({reg}, true)));   // sent again after 2 ms
  records.push_back(at(4 * kMs, test::frame({reg}, true)));   // and again
  records.push_back(at(5 * kMs, test::frame({reg}, false)));  // one answer
  records.push_back(at(6 * kMs, test::frame({reg}, false)));  // and a second, to the second copy
  const Summary s = summarise(records);
  EXPECT_EQ(s.control.sent, 1u);
  EXPECT_EQ(s.control.resent, 2u);
  EXPECT_EQ(s.control.returned, 1u);
  EXPECT_EQ(s.control.latestUs, 5'000);
  EXPECT_EQ(s.control.over2Ms, 1u);
  EXPECT_EQ(s.unmatchedReturns, 1u);
}

TEST(SummaryTest, TheCyclicFrameIsItsOwnKindAndAPositionCountedUpStillMatches) {
  std::vector<Record> records;
  // A cyclic frame: LRD first. Its index differs every cycle; one here.
  records.push_back(at(0, test::frame({{10, 1, 0, 0, 47}, {11, 1, 0, 0x40, 35}}, true)));
  records.push_back(
      at(250'000, test::frame({{10, 1, 0, 0, 47, 3}, {11, 1, 0, 0x40, 35, 2}}, false)));
  // A broadcast read whose position comes back counted up by the one slave.
  records.push_back(at(1 * kMs, test::frame({{7, 2, 0, 0x0130, 2}}, true)));
  records.push_back(at(1 * kMs + 200'000, test::frame({{7, 2, 1, 0x0130, 2, 1}}, false)));
  const Summary s = summarise(records);
  EXPECT_EQ(s.cyclic.sent, 1u);
  EXPECT_EQ(s.cyclic.returned, 1u);
  EXPECT_EQ(s.cyclic.latestUs, 250);
  EXPECT_EQ(s.control.sent, 1u);
  EXPECT_EQ(s.control.returned, 1u);
  EXPECT_EQ(s.control.lost, 0u);
}

TEST(SummaryTest, SerialisesWithCamelCaseKeys) {
  const test::DatagramSpec sdo{4, 3, 0x1001, 0x1000, 8};
  std::vector<Record> records;
  records.push_back(at(0, test::frame({sdo}, true)));
  records.push_back(at(300'000, test::frame({sdo}, false)));
  const nlohmann::json j = summarise(records);
  EXPECT_EQ(j["frames"], 2);
  EXPECT_EQ(j["control"]["sent"], 1);
  EXPECT_EQ(j["control"]["returned"], 1);
  EXPECT_EQ(j["control"]["latestUs"], 300);
  EXPECT_EQ(j["cyclic"]["sent"], 0);
  EXPECT_EQ(j["lastUs"], 300);
}

// Two seconds recorded on the bench Integro while the AL walk ran: the control plane of a
// PRE-OP, the map of a SAFE-OP and the cyclic frames of an OP. The run had `netem` delay 150 ms
// on 2 % of the outgoing frames, and the capture shows none of it: a packet tap sits at the driver
// boundary, and netem is a queue between that boundary and SOEM. So the file pins the parser and
// the matcher against real frames, and the summary of a clean wire, which is what a capture on a
// host whose delay is its own shows.
TEST(SummaryTest, ReadsTheBenchCapture) {
  const std::filesystem::path path =
      std::filesystem::path(MM_CAPTURE_TEST_DATA_DIR) / "integro-bench.pcap";
  const auto records = readPcap(path);
  ASSERT_TRUE(records.has_value());
  const Summary s = summarise(*records);
  EXPECT_EQ(s.notEtherCat, 0u);
  EXPECT_GT(s.control.sent, 1000u);
  EXPECT_GT(s.cyclic.sent, 100u);
  // Every send has its return, apart from what was in flight when the two seconds ended.
  EXPECT_LE(s.control.sent - s.control.returned, 2u);
  EXPECT_LE(s.cyclic.sent - s.cyclic.returned, 2u);
  EXPECT_EQ(s.control.over2Ms, 0u);
  EXPECT_EQ(s.cyclic.over2Ms, 0u);
  EXPECT_LT(s.control.latestUs, 2'000);
  EXPECT_LT(s.cyclic.latestUs, 2'000);
  // A return with no send is a second answer to a resend, and there were a few resends.
  EXPECT_LE(s.unmatchedReturns, s.control.resent);
}

}  // namespace
}  // namespace mm::capture
