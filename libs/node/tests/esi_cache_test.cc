#include "node/esi_cache.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "etg/esi_entry.h"
#include "node/device_parameter.h"

namespace mm::node {
namespace {

namespace fs = std::filesystem;

constexpr uint32_t kSynapticon = 0x000022D2;
constexpr uint32_t kCirculo = 0x00000301;

// The pinned SOMANET ESI of libs/etg. It describes four products, all at revision 0x11000003.
fs::path fixturePath() { return fs::path(MM_ETG_TEST_DATA_DIR) / "somanet-v5.6.6.xml"; }

// A fresh directory unique to one test, removed first so a previous run never leaks in.
fs::path makeTempDir(const std::string& tag) {
  const fs::path dir = fs::temp_directory_path() / ("mm-esi-cache-test-" + tag);
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  return dir;
}

void addFixture(const fs::path& dir, std::string_view version) {
  fs::copy_file(fixturePath(), dir / EsiCache::fileName(kSynapticon, version),
                fs::copy_options::overwrite_existing);
}

const mm::etg::EsiEntry* findEntry(const mm::etg::EsiEntryTable& table, uint16_t index,
                                   uint8_t subindex) {
  const auto it = std::find_if(table.entries.begin(), table.entries.end(),
                               [index, subindex](const mm::etg::EsiEntry& e) {
                                 return e.index == index && e.subindex == subindex;
                               });
  return it != table.entries.end() ? &*it : nullptr;
}

TEST(EsiCacheTest, NamesAFileAfterTheVendorAndTheWholeVersion) {
  EXPECT_EQ(EsiCache::fileName(kSynapticon, "v5.6.11"), "esi-000022d2-v5.6.11.xml");
  // Build metadata stays in the name; '+' is not a file-name character, so it becomes '_'.
  EXPECT_EQ(EsiCache::fileName(kSynapticon, "v5.6.10+1.g676379dc4"),
            "esi-000022d2-v5.6.10_1.g676379dc4.xml");
}

TEST(EsiCacheTest, FindsTheUnitOfAParameterByProductCode) {
  const fs::path dir = makeTempDir("unit");
  addFixture(dir, "v5.6.6");
  const EsiCache cache({.directory = dir.string()});

  const auto table = cache.find(kSynapticon, kCirculo, "v5.6.6");
  ASSERT_NE(table, nullptr);
  const auto* maxMotorSpeed = findEntry(*table, 0x6080, 0);
  ASSERT_NE(maxMotorSpeed, nullptr);
  EXPECT_EQ(maxMotorSpeed->unit, 0x00B44700u);
  EXPECT_FALSE(maxMotorSpeed->unitSymbol.empty());
}

TEST(EsiCacheTest, MatchesTheVersionExactlyIncludingBuildMetadata) {
  const fs::path dir = makeTempDir("exact");
  addFixture(dir, "v5.6.10");
  const EsiCache cache({.directory = dir.string()});

  EXPECT_NE(cache.find(kSynapticon, kCirculo, "v5.6.10"), nullptr);
  EXPECT_EQ(cache.find(kSynapticon, kCirculo, "v5.6.10+1.g676379dc4"), nullptr);
  EXPECT_EQ(cache.find(kSynapticon, kCirculo, "v5.6.1"), nullptr);
  EXPECT_EQ(cache.find(0x00000539, kCirculo, "v5.6.10"), nullptr);
}

TEST(EsiCacheTest, MissesWithoutAFirmwareVersion) {
  const fs::path dir = makeTempDir("empty-version");
  addFixture(dir, "");
  const EsiCache cache({.directory = dir.string()});

  EXPECT_EQ(cache.find(kSynapticon, kCirculo, ""), nullptr);
}

TEST(EsiCacheTest, MissesAProductTheFileDoesNotDescribe) {
  const fs::path dir = makeTempDir("product");
  addFixture(dir, "v5.6.6");
  const EsiCache cache({.directory = dir.string()});

  EXPECT_EQ(cache.find(kSynapticon, 0x00000999, "v5.6.6"), nullptr);
}

TEST(EsiCacheTest, MissesAFileThatDoesNotParse) {
  const fs::path dir = makeTempDir("broken");
  std::ofstream(dir / EsiCache::fileName(kSynapticon, "v5.6.6")) << "<EtherCATInfo>";
  const EsiCache cache({.directory = dir.string()});

  EXPECT_EQ(cache.find(kSynapticon, kCirculo, "v5.6.6"), nullptr);
}

TEST(EsiCacheTest, ReadsAReplacedFileAgain) {
  const fs::path dir = makeTempDir("replaced");
  std::ofstream(dir / EsiCache::fileName(kSynapticon, "v5.6.6")) << "<EtherCATInfo>";
  const EsiCache cache({.directory = dir.string()});
  ASSERT_EQ(cache.find(kSynapticon, kCirculo, "v5.6.6"), nullptr);

  addFixture(dir, "v5.6.6");
  EXPECT_NE(cache.find(kSynapticon, kCirculo, "v5.6.6"), nullptr);

  fs::remove(dir / EsiCache::fileName(kSynapticon, "v5.6.6"));
  EXPECT_EQ(cache.find(kSynapticon, kCirculo, "v5.6.6"), nullptr);
}

TEST(EsiCacheTest, SharesOneTableBetweenDevicesOfOneProductAndFirmware) {
  const fs::path dir = makeTempDir("shared");
  addFixture(dir, "v5.6.6");
  const EsiCache cache({.directory = dir.string()});

  const auto first = cache.find(kSynapticon, kCirculo, "v5.6.6");
  ASSERT_NE(first, nullptr);
  for (int device = 1; device < 30; ++device) {
    EXPECT_EQ(cache.find(kSynapticon, kCirculo, "v5.6.6"), first);
  }
  // The same file serves another product with a table of its own.
  const auto integro = cache.find(kSynapticon, 0x00000401, "v5.6.6");
  ASSERT_NE(integro, nullptr);
  EXPECT_NE(integro, first);
}

TEST(EsiCacheTest, MissesEveryDeviceWhileDisabled) {
  const fs::path dir = makeTempDir("disabled");
  addFixture(dir, "v5.6.6");
  const EsiCache cache({.enabled = false, .directory = dir.string()});

  EXPECT_FALSE(cache.enabled());
  EXPECT_EQ(cache.find(kSynapticon, kCirculo, "v5.6.6"), nullptr);
}

DeviceParameter makeParameter(uint16_t index, uint8_t subindex) {
  DeviceParameter p;
  p.index = index;
  p.subindex = subindex;
  return p;
}

mm::etg::EsiEntryTable tableWithUnit(uint16_t index, uint8_t subindex, uint32_t unit,
                                     std::string symbol) {
  mm::etg::EsiEntry entry;
  entry.index = index;
  entry.subindex = subindex;
  entry.unit = unit;
  entry.unitSymbol = std::move(symbol);
  return mm::etg::EsiEntryTable{.entries = {entry}, .warnings = {}};
}

TEST(JoinEsiTest, TakesTheUnitOfTheEsiWhereCoeReportsNone) {
  const auto esi = tableWithUnit(0x6080, 0, 0x00B44700, "rpm");

  const auto joined =
      joinEsi({makeParameter(0x6080, 0), makeParameter(0x6081, 0)}, &esi, "esi.xml");

  EXPECT_EQ(joined[0].unit, 0x00B44700u);
  EXPECT_EQ(joined[0].unitSymbol, "rpm");
  // An object the ESI does not describe keeps what CoE gave it.
  EXPECT_FALSE(joined[1].unit.has_value());
  EXPECT_TRUE(joined[1].unitSymbol.empty());
}

TEST(JoinEsiTest, KeepsTheUnitCoeReports) {
  const auto esi = tableWithUnit(0x6080, 0, 0x00B44700, "rpm");
  DeviceParameter volts = makeParameter(0x6080, 0);
  volts.unit = 0x00260000;

  const auto joined = joinEsi({volts}, &esi, "esi.xml");

  EXPECT_EQ(joined[0].unit, 0x00260000u);
  EXPECT_EQ(joined[0].unitSymbol, "V");
}

TEST(JoinEsiTest, RendersTheSymbolOfACoeUnitWithoutAnEsi) {
  DeviceParameter volts = makeParameter(0x6079, 0);
  volts.unit = 0x00260000;

  const auto joined = joinEsi({volts, makeParameter(0x6080, 0)}, nullptr, "");

  EXPECT_EQ(joined[0].unitSymbol, "V");
  EXPECT_TRUE(joined[1].unitSymbol.empty());
}

// The join against the real table of a Circulo, so the ranges, descriptions and options are the
// ones a drive gets.
class JoinSomanetEsiTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const fs::path dir = makeTempDir("join");
    addFixture(dir, "v5.6.6");
    cache_ = std::make_unique<EsiCache>(EsiCacheConfig{.directory = dir.string()});
    table_ = cache_->find(kSynapticon, kCirculo, "v5.6.6");
    ASSERT_NE(table_, nullptr);
  }

  // A parameter as CoE would describe the ESI entry: same address and type, nothing else.
  DeviceParameter coeParameter(const mm::etg::EsiEntry& entry) const {
    DeviceParameter p = makeParameter(entry.index, entry.subindex);
    p.dataType = entry.dataType;
    return p;
  }

  const mm::etg::EsiEntry* findNamed(uint16_t index, std::string_view name) const {
    const auto it = std::find_if(table_->entries.begin(), table_->entries.end(),
                                 [index, name](const mm::etg::EsiEntry& e) {
                                   return e.index == index && e.entryName == name;
                                 });
    return it != table_->entries.end() ? &*it : nullptr;
  }

  std::unique_ptr<EsiCache> cache_;
  std::shared_ptr<const mm::etg::EsiEntryTable> table_;
};

TEST_F(JoinSomanetEsiTest, DecodesASignedRangeThroughTheParameterType) {
  // A SINT that runs -1..+1, written MinData=FF MaxData=01.
  const auto* direction = findNamed(0x2004, "Initial movement direction (pin brake)");
  ASSERT_NE(direction, nullptr);

  const auto joined = joinEsi({coeParameter(*direction)}, table_.get(), "esi.xml");

  ASSERT_TRUE(joined[0].minValue.has_value());
  ASSERT_TRUE(joined[0].maxValue.has_value());
  EXPECT_EQ(*joined[0].minValue, DeviceParameterValue{int8_t{-1}});
  EXPECT_EQ(*joined[0].maxValue, DeviceParameterValue{int8_t{1}});
  EXPECT_EQ(joined[0].esiFile, "esi.xml");
}

TEST_F(JoinSomanetEsiTest, TakesTheOptionsAndTheDescriptionOfAnEntry) {
  const auto withOptions =
      std::find_if(table_->entries.begin(), table_->entries.end(),
                   [](const mm::etg::EsiEntry& e) { return !e.options.empty(); });
  ASSERT_NE(withOptions, table_->entries.end());
  const auto* maxMotorSpeed = findEntry(*table_, 0x6080, 0);
  ASSERT_NE(maxMotorSpeed, nullptr);

  const auto joined =
      joinEsi({coeParameter(*withOptions), coeParameter(*maxMotorSpeed)}, table_.get(), "esi.xml");

  ASSERT_EQ(joined[0].options.size(), withOptions->options.size());
  EXPECT_EQ(joined[0].options[0].label, withOptions->options[0].label);
  EXPECT_EQ(joined[0].options[0].value, withOptions->options[0].value);
  EXPECT_NE(joined[1].description.find("max allowed speed"), std::string::npos);
}

TEST_F(JoinSomanetEsiTest, KeepsTheBoundsCoeReports) {
  const auto* maxMotorSpeed = findEntry(*table_, 0x6080, 0);
  ASSERT_NE(maxMotorSpeed, nullptr);
  DeviceParameter p = coeParameter(*maxMotorSpeed);
  p.maxValue = DeviceParameterValue{uint32_t{1234}};

  const auto joined = joinEsi({p}, table_.get(), "esi.xml");

  EXPECT_EQ(*joined[0].maxValue, DeviceParameterValue{uint32_t{1234}});
}

TEST_F(JoinSomanetEsiTest, LeavesAnEntryTheEsiDoesNotDescribeAsCoeGaveIt) {
  DeviceParameter unknown = makeParameter(0x5FFF, 0);
  unknown.dataType = 0x0007;

  const auto joined = joinEsi({unknown}, table_.get(), "esi.xml");

  EXPECT_TRUE(joined[0].esiFile.empty());
  EXPECT_TRUE(joined[0].description.empty());
  EXPECT_FALSE(joined[0].maxValue.has_value());
}

}  // namespace
}  // namespace mm::node
