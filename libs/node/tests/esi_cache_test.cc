#include "node/esi_cache.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "etg/esi_entry.h"

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

}  // namespace
}  // namespace mm::node
