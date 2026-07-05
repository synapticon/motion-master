#include "node/parameter_cache.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "node/device_parameter.h"

namespace mm::node {
namespace {

namespace fs = std::filesystem;

constexpr uint16_t kUnsigned32 = 0x0007;
constexpr uint16_t kUnsigned64 = 0x001B;
constexpr uint32_t kOtherVendor = 0x00000539;

// A fresh, empty temp directory unique to one test, removed first so a previous run never leaks in.
fs::path makeTempDir(const std::string& tag) {
  fs::path dir = fs::temp_directory_path() / ("mm-parameter-cache-test-" + tag);
  std::error_code ec;
  fs::remove_all(dir, ec);
  return dir;
}

DeviceParameter makeParam(uint16_t index, uint8_t subindex, uint16_t dataType) {
  DeviceParameter p;
  p.index = index;
  p.subindex = subindex;
  p.name = "param";
  p.objectCode = 7;
  p.dataType = dataType;
  p.bitLength = 32;
  p.access = 0x3F;
  p.value = defaultValueForDataType(dataType);
  return p;
}

// The definitions persist; the live value / sync state do not. Round-tripping a Synapticon device
// (cached by default) must return the schema and bounds verbatim, but with the value reset.
TEST(ParameterCacheTest, RoundTripsDefinitionsButNotLiveValues) {
  ParameterCache cache(
      {.cacheAllVendors = false, .directory = makeTempDir("roundtrip").string(), .enabled = true});

  DeviceParameter withBounds = makeParam(0x6040, 0, kUnsigned32);
  withBounds.unit = 0x00010000u;
  withBounds.defaultValue = DeviceParameterValue{uint32_t{5}};
  withBounds.minValue = DeviceParameterValue{uint32_t{0}};
  withBounds.maxValue = DeviceParameterValue{uint32_t{100}};
  // These two are volatile and must be dropped by the cache.
  withBounds.value = DeviceParameterValue{uint32_t{42}};
  withBounds.syncState = SyncState::Synced;

  DeviceParameter plain = makeParam(0x6041, 0, kUnsigned32);

  cache.store(kSynapticonVendorId, 0x0201, 0x0A, {withBounds, plain});

  auto loaded = cache.load(kSynapticonVendorId, 0x0201, 0x0A);
  ASSERT_TRUE(loaded.has_value());
  ASSERT_EQ(loaded->size(), 2u);

  const DeviceParameter& a = (*loaded)[0];
  EXPECT_EQ(a.index, 0x6040);
  EXPECT_EQ(a.subindex, 0);
  EXPECT_EQ(a.name, "param");
  EXPECT_EQ(a.objectCode, 7);
  EXPECT_EQ(a.dataType, kUnsigned32);
  EXPECT_EQ(a.bitLength, 32);
  EXPECT_EQ(a.access, 0x3F);
  ASSERT_TRUE(a.unit.has_value());
  EXPECT_EQ(*a.unit, 0x00010000u);
  ASSERT_TRUE(a.defaultValue.has_value());
  EXPECT_EQ(std::get<uint32_t>(*a.defaultValue), 5u);
  EXPECT_EQ(std::get<uint32_t>(*a.minValue), 0u);
  EXPECT_EQ(std::get<uint32_t>(*a.maxValue), 100u);
  // The live value is reset to the type default and the sync state to Unknown — never cached.
  EXPECT_EQ(std::get<uint32_t>(a.value), 0u);
  EXPECT_EQ(a.syncState, SyncState::Unknown);

  const DeviceParameter& b = (*loaded)[1];
  EXPECT_EQ(b.index, 0x6041);
  EXPECT_FALSE(b.unit.has_value());
  EXPECT_FALSE(b.defaultValue.has_value());
}

// 64-bit bounds must survive exactly (a double round-trip would lose the low bits).
TEST(ParameterCacheTest, Preserves64BitBoundsExactly) {
  ParameterCache cache(
      {.cacheAllVendors = false, .directory = makeTempDir("u64").string(), .enabled = true});

  DeviceParameter p = makeParam(0x2000, 0, kUnsigned64);
  const uint64_t big = 0xFEDCBA9876543210ull;
  p.maxValue = DeviceParameterValue{big};
  cache.store(kSynapticonVendorId, 1, 1, {p});

  auto loaded = cache.load(kSynapticonVendorId, 1, 1);
  ASSERT_TRUE(loaded.has_value());
  ASSERT_EQ(loaded->size(), 1u);
  ASSERT_TRUE((*loaded)[0].maxValue.has_value());
  EXPECT_EQ(std::get<uint64_t>(*(*loaded)[0].maxValue), big);
}

// The key is (vendor, product, revision): a different revision is a clean miss, not a wrong hit.
TEST(ParameterCacheTest, MissesOnDifferentIdentity) {
  ParameterCache cache(
      {.cacheAllVendors = false, .directory = makeTempDir("identity").string(), .enabled = true});
  cache.store(kSynapticonVendorId, 0x0201, 0x0A, {makeParam(0x6040, 0, kUnsigned32)});

  EXPECT_FALSE(cache.load(kSynapticonVendorId, 0x0201, 0x0B).has_value());  // other revision
  EXPECT_FALSE(cache.load(kSynapticonVendorId, 0x0202, 0x0A).has_value());  // other product
  EXPECT_TRUE(cache.load(kSynapticonVendorId, 0x0201, 0x0A).has_value());   // exact match
}

// Default policy: Synapticon is cached, other vendors are not — store is a no-op and load misses.
TEST(ParameterCacheTest, OtherVendorsDisabledByDefault) {
  ParameterCache cache({.cacheAllVendors = false,
                        .directory = makeTempDir("vendor-default").string(),
                        .enabled = true});

  EXPECT_TRUE(cache.enabledForVendor(kSynapticonVendorId));
  EXPECT_FALSE(cache.enabledForVendor(kOtherVendor));

  cache.store(kOtherVendor, 1, 1, {makeParam(0x6040, 0, kUnsigned32)});
  EXPECT_FALSE(cache.load(kOtherVendor, 1, 1).has_value());
}

// cacheAllVendors flips the policy on for every vendor.
TEST(ParameterCacheTest, CacheAllVendorsEnablesOtherVendors) {
  ParameterCache cache(
      {.cacheAllVendors = true, .directory = makeTempDir("vendor-all").string(), .enabled = true});

  EXPECT_TRUE(cache.enabledForVendor(kOtherVendor));
  cache.store(kOtherVendor, 1, 1, {makeParam(0x6040, 0, kUnsigned32)});
  EXPECT_TRUE(cache.load(kOtherVendor, 1, 1).has_value());
}

// The master switch disables the cache for everyone, Synapticon included.
TEST(ParameterCacheTest, DisabledMasterSwitchCachesNothing) {
  ParameterCache cache(
      {.cacheAllVendors = true, .directory = makeTempDir("disabled").string(), .enabled = false});

  EXPECT_FALSE(cache.enabledForVendor(kSynapticonVendorId));
  cache.store(kSynapticonVendorId, 1, 1, {makeParam(0x6040, 0, kUnsigned32)});
  EXPECT_FALSE(cache.load(kSynapticonVendorId, 1, 1).has_value());
}

// A corrupt file is treated as a miss (the caller re-enumerates), never a throw.
TEST(ParameterCacheTest, CorruptFileIsAMiss) {
  const fs::path dir = makeTempDir("corrupt");
  ParameterCache cache({.cacheAllVendors = false, .directory = dir.string(), .enabled = true});

  fs::create_directories(dir);
  std::ofstream(dir / "parameters-000022d2-00000001-00000001.json") << "{ not valid json ]";

  EXPECT_FALSE(cache.load(kSynapticonVendorId, 1, 1).has_value());
}

// list() reports each stored file's identity and a parameter count, for the management UI.
TEST(ParameterCacheTest, ListReportsStoredCaches) {
  ParameterCache cache(
      {.cacheAllVendors = false, .directory = makeTempDir("list").string(), .enabled = true});
  EXPECT_TRUE(cache.list().empty());

  cache.store(kSynapticonVendorId, 0x0201, 0x0A,
              {makeParam(0x6040, 0, kUnsigned32), makeParam(0x6041, 0, kUnsigned32)});
  cache.store(kSynapticonVendorId, 0x0301, 0x0C, {makeParam(0x6060, 0, kUnsigned32)});

  auto entries = cache.list();
  ASSERT_EQ(entries.size(), 2u);
  const auto byKey = [&](uint32_t product) {
    return std::find_if(entries.begin(), entries.end(),
                        [&](const auto& e) { return e.productCode == product; });
  };
  auto a = byKey(0x0201);
  ASSERT_NE(a, entries.end());
  EXPECT_EQ(a->vendorId, kSynapticonVendorId);
  EXPECT_EQ(a->revisionNumber, 0x0Au);
  EXPECT_EQ(a->parameterCount, 2u);
  EXPECT_GT(a->sizeBytes, 0u);
  EXPECT_EQ(byKey(0x0301)->parameterCount, 1u);
}

// Listing is policy-independent: a disabled cache still reports files already on disk so they can
// be cleaned up.
TEST(ParameterCacheTest, ListIgnoresPolicy) {
  const fs::path dir = makeTempDir("list-policy");
  ParameterCache writer({.cacheAllVendors = false, .directory = dir.string(), .enabled = true});
  writer.store(kSynapticonVendorId, 1, 1, {makeParam(0x6040, 0, kUnsigned32)});

  ParameterCache disabled({.cacheAllVendors = false, .directory = dir.string(), .enabled = false});
  EXPECT_EQ(disabled.list().size(), 1u);
}

// readRaw returns the JSON file verbatim; remove deletes it. Both are addressed by the opaque id
// (the same one list() reports), and reject a malformed id.
TEST(ParameterCacheTest, ReadRawAndRemove) {
  ParameterCache cache(
      {.cacheAllVendors = false, .directory = makeTempDir("rawremove").string(), .enabled = true});
  cache.store(kSynapticonVendorId, 0x0201, 0x0A, {makeParam(0x6040, 0, kUnsigned32)});
  const std::string id = ParameterCache::makeId(kSynapticonVendorId, 0x0201, 0x0A);

  auto raw = cache.readRaw(id);
  ASSERT_TRUE(raw.has_value());
  const auto doc = nlohmann::json::parse(*raw);
  EXPECT_EQ(doc.at("vendorId").get<uint32_t>(), kSynapticonVendorId);
  EXPECT_EQ(doc.at("parameters").size(), 1u);

  EXPECT_FALSE(cache.readRaw("not-a-valid-id").has_value());  // malformed → error
  EXPECT_FALSE(
      cache.readRaw(ParameterCache::makeId(kSynapticonVendorId, 0x0201, 0x0B)).has_value());

  EXPECT_TRUE(cache.remove(id).has_value());
  EXPECT_TRUE(cache.list().empty());
  EXPECT_FALSE(cache.remove(id).has_value());  // already gone → error
}

}  // namespace
}  // namespace mm::node
