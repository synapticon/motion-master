#include "node/config_file.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "comm/object_data_types.h"

namespace mm::node {
namespace {

using mm::comm::ObjectDataType;

// A config.csv saved by an Integro with firmware v5.6.11.
constexpr std::string_view kIntegroConfig = "config-integro.csv";

std::string readFixture(std::string_view filename) {
  const std::filesystem::path path = std::filesystem::path(MM_NODE_TEST_DATA_DIR) / filename;
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in) << "missing test fixture: " << path;
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

DeviceParameter object(ObjectDataType type, uint16_t access = 0x0100, uint16_t bitLength = 32) {
  DeviceParameter p;
  p.dataType = static_cast<uint16_t>(type);
  p.access = access;
  p.bitLength = bitLength;
  return p;
}

TEST(ConfigFileTest, ReadsAFileSavedByADrive) {
  const auto text = readFixture(kIntegroConfig);
  const auto file = parseConfigFile(text);
  EXPECT_EQ(file.format, 2);
  EXPECT_EQ(file.rows.size(), 243U);
  EXPECT_TRUE(file.problems.empty());
  ASSERT_EQ(file.comments.size(), 2U);
  EXPECT_EQ(file.deviceName(), "ACTILINK-S Type ASC1-061A-0");
  EXPECT_EQ(file.firmwareVersion(), "v5.6.8");
  EXPECT_EQ(file.rows.front().index, 0x2001);
  EXPECT_EQ(file.rows.front().subindex, 0);
  EXPECT_EQ(file.rows.front().trimmedValue(), "848");
  EXPECT_EQ(file.rows.front().line, 4U);
}

TEST(ConfigFileTest, WritesAnUnchangedFileByteForByte) {
  const auto text = readFixture(kIntegroConfig);
  EXPECT_EQ(writeConfigFile(parseConfigFile(text)), text);

  const std::string crlf =
      "META,FORMAT,2\r\n# v5.6.11\r\n0x2001,  0,  848\r\nno newline at the end";
  EXPECT_EQ(writeConfigFile(parseConfigFile(crlf)), crlf);
}

TEST(ConfigFileTest, FormatsARowAsTheDriveDoes) {
  EXPECT_EQ(formatConfigFileRow(0x2001, 0, "848"), "0x2001,  0,  848\n");
  EXPECT_EQ(formatConfigFileRow(0x2012, 12, "0.5"), "0x2012, 12,  0.5\n");
  EXPECT_EQ(formatConfigFileRow(0x1a00, 1, "1"), "0x1a00,  1,  1\n");
}

TEST(ConfigFileTest, ReportsEachLineTheDriveIgnores) {
  const auto file = parseConfigFile(
      "META,FORMAT,2\n"
      "META,COLOUR,red\n"
      "0x2001, 0\n"
      "0x2001, 0, 1, 2\n"
      "0x0100, 0, 1\n"
      "zz, 0, 1\n"
      "0x2001, 300, 1\n"
      "0x2002, 0, 2 # a trailing comment\n"
      "META,FORMAT,1\n");
  EXPECT_EQ(file.format, 2);
  ASSERT_EQ(file.rows.size(), 1U);
  EXPECT_EQ(file.rows[0].index, 0x2002);
  EXPECT_EQ(file.rows[0].trimmedValue(), "2");
  std::vector<std::size_t> lines;
  lines.reserve(file.problems.size());
  for (const auto& problem : file.problems) {
    lines.push_back(problem.line);
  }
  EXPECT_EQ(lines, (std::vector<std::size_t>{2, 3, 4, 5, 6, 7, 9}));
}

TEST(ConfigFileTest, ReadsAFileWithAByteOrderMark) {
  const auto file = parseConfigFile("\xEF\xBB\xBFMETA,FORMAT,2\n0x2001, 0, 1\n");
  EXPECT_EQ(file.format, 2);
  EXPECT_EQ(file.rows.size(), 1U);
  EXPECT_TRUE(file.problems.empty());
}

TEST(ConfigFileTest, SelectsRowsAndKeepsEveryOtherLine) {
  const auto file = parseConfigFile("META,FORMAT,2\n# v5.6.11\n0x2001, 0, 1\n0x2002, 0, 2\n");
  const auto selected = selectConfigFileRows(file, {{0x2002, 0}});
  EXPECT_EQ(writeConfigFile(selected), "META,FORMAT,2\n# v5.6.11\n0x2002, 0, 2\n");
}

TEST(ConfigFileTest, MergesRowsInPlaceAndAddsNewOnesAtTheEnd) {
  const auto base = parseConfigFile("META,FORMAT,2\n0x2001,  0,  1\n0x2002,  0,  2");
  const auto update = parseConfigFile("0x2002, 0, 20\n0x2003, 1, 3\n");
  const auto merged = mergeConfigFiles(base, update);
  EXPECT_EQ(writeConfigFile(merged),
            "META,FORMAT,2\n0x2001,  0,  1\n0x2002,  0,  20\n0x2003,  1,  3\n");
}

TEST(ConfigFileTest, ComparesRowsByAddressAndByNumber) {
  const auto first = parseConfigFile("0x2001, 0, 1.50\n0x2002, 0, 2\n0x2003, 0, a\n");
  const auto second = parseConfigFile("0x2001, 0, 1.5\n0x2002, 0, 3\n0x2004, 0, 4\n");
  const auto comparison = compareConfigFileRows(first.rows, second.rows);
  ASSERT_EQ(comparison.differing.size(), 1U);
  EXPECT_EQ(comparison.differing[0].index, 0x2002);
  EXPECT_EQ(comparison.differing[0].first, "2");
  EXPECT_EQ(comparison.differing[0].second, "3");
  ASSERT_EQ(comparison.onlyInFirst.size(), 1U);
  EXPECT_EQ(comparison.onlyInFirst[0].index, 0x2003);
  ASSERT_EQ(comparison.onlyInSecond.size(), 1U);
  EXPECT_EQ(comparison.onlyInSecond[0].index, 0x2004);
}

TEST(ConfigFileTest, DecidesTheOutcomeOfARowAsTheDriveDoes) {
  const auto backedUp = object(ObjectDataType::UNSIGNED32, 0x0100);
  const auto notBackedUp = object(ObjectDataType::UNSIGNED32, 0x0007);
  const auto sixtyFourBit = object(ObjectDataType::UNSIGNED64, 0x0100);

  EXPECT_EQ(configRowOutcome(0x2001, nullptr), ConfigRowOutcome::kUnknownObject);
  EXPECT_EQ(configRowOutcome(0x2001, &backedUp), ConfigRowOutcome::kLoads);
  EXPECT_EQ(configRowOutcome(0x2001, &notBackedUp), ConfigRowOutcome::kNotLoadable);
  EXPECT_EQ(configRowOutcome(0x2001, &sixtyFourBit), ConfigRowOutcome::kNotLoadable);
  // Below 0x2000 only the PDO mapping and assignment objects load, backup flag or not.
  EXPECT_EQ(configRowOutcome(0x1600, &notBackedUp), ConfigRowOutcome::kLoads);
  EXPECT_EQ(configRowOutcome(0x1C13, &notBackedUp), ConfigRowOutcome::kLoads);
  EXPECT_EQ(configRowOutcome(0x1604, &backedUp), ConfigRowOutcome::kNotLoadable);
  EXPECT_EQ(configRowOutcome(0x100A, &backedUp), ConfigRowOutcome::kNotLoadable);
  EXPECT_EQ(toString(ConfigRowOutcome::kNotLoadable), "not-loadable");
}

TEST(ConfigFileTest, ReadsIntegerValuesInTheirTypeRange) {
  const auto u16 = object(ObjectDataType::UNSIGNED16);
  EXPECT_EQ(std::get<uint16_t>(*parseConfigFileValue(u16, "  65535 ", 2)), 65535);
  EXPECT_FALSE(parseConfigFileValue(u16, "65536", 2));
  EXPECT_FALSE(parseConfigFileValue(u16, "-1", 2));
  EXPECT_FALSE(parseConfigFileValue(u16, "abc", 2));
  EXPECT_FALSE(parseConfigFileValue(u16, "", 2));

  const auto i8 = object(ObjectDataType::INTEGER8);
  EXPECT_EQ(std::get<int8_t>(*parseConfigFileValue(i8, "-128", 2)), -128);
  EXPECT_EQ(std::get<int8_t>(*parseConfigFileValue(i8, "+5", 2)), 5);
  EXPECT_FALSE(parseConfigFileValue(i8, "128", 2));

  const auto boolean = object(ObjectDataType::BOOLEAN);
  EXPECT_EQ(std::get<uint8_t>(*parseConfigFileValue(boolean, "1", 2)), 1);
  EXPECT_FALSE(parseConfigFileValue(boolean, "2", 2));
}

TEST(ConfigFileTest, ReadsRealValuesAsTheDriveDoes) {
  const auto real = object(ObjectDataType::REAL32);
  EXPECT_FLOAT_EQ(std::get<float>(*parseConfigFileValue(real, "  0.0247177552", 2)), 0.0247177552F);
  EXPECT_FLOAT_EQ(std::get<float>(*parseConfigFileValue(real, "-1.5", 2)), -1.5F);
  EXPECT_FLOAT_EQ(std::get<float>(*parseConfigFileValue(real, "3", 2)), 3.0F);
  // The drive reads 0 for a REAL32 value with any other character, a trailing space included.
  EXPECT_FALSE(parseConfigFileValue(real, "1.5 ", 2));
  EXPECT_FALSE(parseConfigFileValue(real, "1e3", 2));
  EXPECT_FALSE(parseConfigFileValue(real, "-", 2));
  // Format 0 reads the digits after the point as a count of 1e-10, whatever their number.
  EXPECT_FLOAT_EQ(std::get<float>(*parseConfigFileValue(real, "0.100000000", 0)), 0.01F);
}

TEST(ConfigFileTest, ReadsAStringThatFitsTheObject) {
  const auto text = object(ObjectDataType::VISIBLE_STRING, 0x0100, 8 * 8);
  EXPECT_EQ(std::get<std::string>(*parseConfigFileValue(text, "  axis 1", 2)), "axis 1");
  EXPECT_FALSE(parseConfigFileValue(text, "much too long", 2));
}

TEST(ConfigFileTest, KnowsTheSafetyAreas) {
  EXPECT_TRUE(isSafetyConfigIndex(0x2600));
  EXPECT_TRUE(isSafetyConfigIndex(0x67EF));
  EXPECT_FALSE(isSafetyConfigIndex(0x2700));
  EXPECT_FALSE(isSafetyConfigIndex(0x67F0));
}

}  // namespace
}  // namespace mm::node
