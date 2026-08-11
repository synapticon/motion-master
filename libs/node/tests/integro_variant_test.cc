#include "node/integro_variant.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace mm::node {
namespace {

// Two real files read off Integro drives. The second one matters more than it looks: it selects
// three fieldbus protocols at once, which is the case the descriptor's tie-break exists for.
constexpr std::string_view kIntegroVariant = "integro.variant";
constexpr std::string_view kMultipleFieldbusVariant = "integro-multiple-fieldbus.variant";

std::vector<uint8_t> readVariant(std::string_view filename) {
  const std::filesystem::path path = std::filesystem::path(MM_NODE_TEST_DATA_DIR) / filename;
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in) << "missing test fixture: " << path;
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

TEST(IntegroVariantTest, DecodesARealFile) {
  auto variant = parseIntegroVariant(readVariant(kIntegroVariant));
  ASSERT_TRUE(variant) << variant.error();
  EXPECT_EQ(variant->fileVersion, 1u);
  EXPECT_EQ(variant->serialNumber, "9004-02-0000424-2445");
  // Pins the offset the layout is easiest to get wrong at: the MAC field is eight bytes and the
  // address is the last six, so reading it from the start of the field yields "00:00:40:49:8A:01".
  EXPECT_EQ(variant->macAddress, "40:49:8A:01:21:D6");
  EXPECT_EQ(variant->customerId, 1u);
  EXPECT_EQ(variant->operationMode, static_cast<uint16_t>(VariantOperationMode::kLive));
  EXPECT_EQ(variant->chipId[0], 0x4Eu);
  // File order, not sorted: this is how the file was written and the only record of it.
  EXPECT_EQ(variant->optionIds, (std::vector<uint16_t>{1, 17, 34, 4, 7, 10, 14}));
}

TEST(IntegroVariantTest, ReadsTheSerialNumberUpToItsPadding) {
  auto variant = parseIntegroVariant(readVariant(kIntegroVariant));
  ASSERT_TRUE(variant) << variant.error();
  // The field is 24 bytes and the serial number is 20, NUL-padded. Taking the field whole would
  // append four NULs to every serial number this decodes.
  EXPECT_EQ(variant->serialNumber.size(), 20u);
}

TEST(IntegroVariantTest, PrefersEtherCatOverEveryOtherFieldbus) {
  auto variant = parseIntegroVariant(readVariant(kMultipleFieldbusVariant));
  ASSERT_TRUE(variant) << variant.error();
  // EtherNet/IP (0), EtherCAT (1) and PROFINET (2) are all selected, and 0 comes first in the file.
  EXPECT_EQ(variant->optionIds, (std::vector<uint16_t>{0, 1, 17, 2, 4, 7, 10, 14}));
  EXPECT_EQ(variantFieldbusProtocol(*variant), 1u);
}

TEST(IntegroVariantTest, ReportsTheOnlyFieldbusWhenItIsNotEtherCat) {
  std::vector<uint8_t> content = readVariant(kIntegroVariant);
  content[132] = 3;  // The first option was EtherCAT (1); make it CAN.
  auto variant = parseIntegroVariant(content);
  ASSERT_TRUE(variant) << variant.error();
  EXPECT_EQ(variantFieldbusProtocol(*variant), 3u);
}

TEST(IntegroVariantTest, ReportsNoFieldbusWhenNoneIsSelected) {
  std::vector<uint8_t> content = readVariant(kIntegroVariant);
  content[132] = 17;  // Cyclic mode active, which is in no fieldbus category.
  auto variant = parseIntegroVariant(content);
  ASSERT_TRUE(variant) << variant.error();
  EXPECT_FALSE(variantFieldbusProtocol(*variant).has_value());
}

TEST(IntegroVariantTest, RejectsAFileTooShortToHoldItsHeader) {
  EXPECT_FALSE(parseIntegroVariant(std::vector<uint8_t>{}).has_value());
  EXPECT_FALSE(parseIntegroVariant(std::vector<uint8_t>(131, 0)).has_value());
}

TEST(IntegroVariantTest, RejectsMoreOptionsThanADriveAccepts) {
  std::vector<uint8_t> content(1024, 0);
  content[130] = kMaxVariantOptions + 1;
  auto variant = parseIntegroVariant(content);
  ASSERT_FALSE(variant.has_value());
  // Firmware answers OPTIONS_STATUS_TOO_MANY_OPTIONS to exactly this, so a file the drive would
  // refuse is refused here too.
  EXPECT_NE(variant.error().find("33"), std::string::npos) << variant.error();
}

TEST(IntegroVariantTest, RejectsAFileTruncatedMidOption) {
  std::vector<uint8_t> content = readVariant(kIntegroVariant);
  content.pop_back();
  EXPECT_FALSE(parseIntegroVariant(content).has_value());
}

TEST(IntegroVariantTest, AcceptsTrailingPadding) {
  // The firmware reads as many options as the count claims and ignores the rest, so a padded file
  // is one the drive is happy with.
  std::vector<uint8_t> content = readVariant(kIntegroVariant);
  content.insert(content.end(), 16, 0);
  auto variant = parseIntegroVariant(content);
  ASSERT_TRUE(variant) << variant.error();
  EXPECT_EQ(variant->optionIds.size(), 7u);
}

TEST(IntegroVariantTest, NamesEveryOperationMode) {
  EXPECT_EQ(toString(VariantOperationMode::kPassive), "passive");
  EXPECT_EQ(toString(VariantOperationMode::kTrial), "trial");
  EXPECT_EQ(toString(VariantOperationMode::kProduction), "production");
  EXPECT_EQ(toString(VariantOperationMode::kLive), "live");
  EXPECT_EQ(toString(static_cast<VariantOperationMode>(0x42)), "unknown");
}

TEST(IntegroVariantTest, CarriesTheWholeOptionCatalogue) {
  const auto options = integroVariantOptions();
  EXPECT_EQ(options.size(), 39u);
  // Ascending with real gaps: 15 was never an option code.
  for (size_t i = 1; i < options.size(); ++i) {
    EXPECT_LT(options[i - 1].id, options[i].id);
  }
  EXPECT_EQ(integroVariantOption(15), nullptr);

  const IntegroVariantOption* etherCat = integroVariantOption(1);
  ASSERT_NE(etherCat, nullptr);
  EXPECT_EQ(etherCat->category, "Fieldbus Protocol");
  EXPECT_EQ(etherCat->meaning, "EtherCAT");
  EXPECT_TRUE(etherCat->socVariables.empty());
  EXPECT_EQ(etherCat->incompatibleOptionIds.size(), 4u);
  ASSERT_EQ(etherCat->mpnSegmentCodes.size(), 1u);
  EXPECT_EQ(etherCat->mpnSegmentCodes[0], "EC");

  // A category whose options all configure the same SoC values — the shared list, referenced rather
  // than repeated, so two rows of one category cannot drift apart.
  const IntegroVariantOption* limit = integroVariantOption(7);
  ASSERT_NE(limit, nullptr);
  EXPECT_EQ(limit->category, "Current Limits");
  EXPECT_EQ(limit->socVariables.size(), 15u);
  EXPECT_EQ(limit->socVariables[0], "drive_max_continuous_current_mA");

  // The codes current firmware no longer implements are still here, because old files carry them.
  EXPECT_NE(integroVariantOption(21), nullptr);
  EXPECT_NE(integroVariantOption(39), nullptr);
  EXPECT_EQ(integroVariantOption(40), nullptr);
}

}  // namespace
}  // namespace mm::node
