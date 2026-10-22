#include "capture/pcap.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include "test_frames.h"

namespace mm::capture {
namespace {

TEST(PcapTest, WritesAndReadsBackEveryRecord) {
  const std::filesystem::path path =
      std::filesystem::path(MM_CAPTURE_TEST_OUTPUT_DIR) / "round_trip.pcap";
  const auto a = test::frame({{4, 1, 0x1001, 0x0130, 2}}, true);
  const auto b = test::frame({{4, 1, 0x1001, 0x0130, 2, 1}}, false);
  {
    PcapWriter writer;
    ASSERT_TRUE(writer.open(path).has_value());
    ASSERT_TRUE(writer.write(1'700'000'000'123'456'789, a).has_value());
    ASSERT_TRUE(writer.write(1'700'000'000'123'756'789, b).has_value());
  }
  const auto records = readPcap(path);
  ASSERT_TRUE(records.has_value());
  ASSERT_EQ(records->size(), 2u);
  EXPECT_EQ((*records)[0].timestampNs, 1'700'000'000'123'456'789);
  EXPECT_EQ((*records)[0].bytes, a);
  EXPECT_EQ((*records)[1].timestampNs, 1'700'000'000'123'756'789);
  EXPECT_EQ((*records)[1].bytes, b);
}

TEST(PcapTest, RefusesAFileThatIsNotPcap) {
  const std::filesystem::path path =
      std::filesystem::path(MM_CAPTURE_TEST_OUTPUT_DIR) / "not_pcap.bin";
  {
    std::FILE* f = std::fopen(path.string().c_str(), "wb");
    ASSERT_NE(f, nullptr);
    std::fputs("this is not a capture, not even a short one", f);
    std::fclose(f);
  }
  const auto records = readPcap(path);
  EXPECT_FALSE(records.has_value());
}

}  // namespace
}  // namespace mm::capture
