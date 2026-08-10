#include "node/firmware_package.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace mm::node {
namespace {

// The two packages committed under tests/data are real Synapticon releases, and between them they
// cover the shape difference that matters: the Circulo ships an SII and no COM binary, the ACTILINK
// a COM binary and no SII. A synthetic zip would have been shaped by what this parser expects;
// these were shaped by the build that produces them.
constexpr std::string_view kCirculoPackage =
    "package_SOMANET-Circulo-7_8500-04-2332_motion-drive_v5.6.10.zip";
constexpr std::string_view kActilinkPackage =
    "package_ACTILINK_6000-01-2332-1_motion-drive_v5.6.10.zip";

std::vector<uint8_t> readPackage(std::string_view filename) {
  const std::filesystem::path path = std::filesystem::path(MM_NODE_TEST_DATA_DIR) / filename;
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in) << "missing test fixture: " << path;
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<std::string> defaultSkipFiles() {
  std::vector<std::string> files;
  files.reserve(std::size(kDefaultSkippedFirmwareFiles));
  for (std::string_view name : kDefaultSkippedFirmwareFiles) {
    files.emplace_back(name);
  }
  return files;
}

// ── Filename grammar ───────────────────────────────────────────────────────────────────────────

TEST(FirmwarePackageNameTest, DecodesACirculoPackage) {
  auto name = parseFirmwarePackageName(kCirculoPackage);
  ASSERT_TRUE(name) << name.error();
  EXPECT_EQ(name->description, "package");
  EXPECT_EQ(name->hardwareName, "SOMANET-Circulo-7");
  EXPECT_EQ(name->firmwareId, "8500-04-2332");
  EXPECT_EQ(name->firmwareName, "motion-drive");
  EXPECT_EQ(name->firmwareVersion, "v5.6.10");
  EXPECT_EQ(name->productId, 8500u);
  EXPECT_EQ(name->productVersion, 4u);
  EXPECT_EQ(name->keyId, 2332u);
  EXPECT_FALSE(name->fieldbusProtocol.has_value());  // No fieldbus character on this one.
}

TEST(FirmwarePackageNameTest, DecodesTheFieldbusCharacter) {
  auto name = parseFirmwarePackageName(kActilinkPackage);
  ASSERT_TRUE(name) << name.error();
  EXPECT_EQ(name->hardwareName, "ACTILINK");
  EXPECT_EQ(name->firmwareId, "6000-01-2332-1");
  EXPECT_EQ(name->productId, 6000u);
  EXPECT_EQ(name->productVersion, 1u);
  EXPECT_EQ(name->keyId, 2332u);
  EXPECT_EQ(name->fieldbusProtocol, 1u);  // EtherCAT.
}

// The specification's own third example. Its descriptor is not numeric at all, which must parse as
// a valid *name* while leaving every decoded field empty — the descriptor is an opaque string by
// design, so guessing numbers out of it would be worse than reporting none.
TEST(FirmwarePackageNameTest, AcceptsANonNumericDescriptor) {
  auto name = parseFirmwarePackageName(
      "package_Branded-Drive-Elite_MyProduct-v25-key3-ecat_motion-drive_v5.3.0-rc.3.zip");
  ASSERT_TRUE(name) << name.error();
  EXPECT_EQ(name->firmwareId, "MyProduct-v25-key3-ecat");
  EXPECT_EQ(name->firmwareVersion, "v5.3.0-rc.3");
  EXPECT_FALSE(name->productId.has_value());
  EXPECT_FALSE(name->productVersion.has_value());
  EXPECT_FALSE(name->keyId.has_value());
}

TEST(FirmwarePackageNameTest, IgnoresAWindowsDuplicateSuffix) {
  auto name = parseFirmwarePackageName(
      "package_SOMANET-Integro-60_9002-02_motion-drive_v5.4.0-rc.3 (1).zip");
  ASSERT_TRUE(name) << name.error();
  EXPECT_EQ(name->firmwareVersion, "v5.4.0-rc.3");
}

TEST(FirmwarePackageNameTest, RejectsNamesOutsideTheGrammar) {
  EXPECT_FALSE(parseFirmwarePackageName("firmware.bin"));                       // Not a zip.
  EXPECT_FALSE(parseFirmwarePackageName("package_Circulo_8500-04.zip"));        // Too few fields.
  EXPECT_FALSE(parseFirmwarePackageName("bundle_C7_8500-04_motion_v1.0.zip"));  // Not "package".
  EXPECT_FALSE(parseFirmwarePackageName("package_C7_8500-04_motion_1.0.zip"));  // No leading 'v'.
  EXPECT_FALSE(parseFirmwarePackageName("package__8500-04_motion_v1.0.zip"));   // Empty field.
  // The obsolete pre-specification naming; unsupported on purpose rather than by omission.
  EXPECT_FALSE(parseFirmwarePackageName(
      "package-motion-drive_ComEtherCAT-b_CoreC2X-a_Drive400-e_v4.2.1.zip"));
}

// ── Package contents ───────────────────────────────────────────────────────────────────────────

TEST(FirmwarePackageTest, ClassifiesACirculoPackage) {
  const std::vector<uint8_t> zip = readPackage(kCirculoPackage);
  auto package = openFirmwarePackage(zip, defaultSkipFiles());
  ASSERT_TRUE(package) << package.error();

  ASSERT_TRUE(package->appBinary.has_value());
  EXPECT_EQ(package->appBinary->name, "app_8500-04_motion-drive_v5.6.10_2332.bin");
  EXPECT_EQ(package->appBinary->content.size(), 397312u);

  EXPECT_FALSE(package->comBinary.has_value());  // A Circulo has no separate COM processor.

  ASSERT_TRUE(package->sii.has_value());
  EXPECT_EQ(package->sii->name, "somanet_cia402.sii");
  EXPECT_EQ(package->sii->content.size(), 252u);

  EXPECT_TRUE(package->extras.empty());
  EXPECT_EQ(package->skipped,
            (std::vector<std::string>{"SOMANET_CiA_402.xml.zip", "stack_image.svg.zip"}));
}

TEST(FirmwarePackageTest, ClassifiesAnActilinkPackage) {
  const std::vector<uint8_t> zip = readPackage(kActilinkPackage);
  auto package = openFirmwarePackage(zip, defaultSkipFiles());
  ASSERT_TRUE(package) << package.error();

  ASSERT_TRUE(package->appBinary.has_value());
  EXPECT_EQ(package->appBinary->name, "app_9010-02-1_motion-drive_v5.6.10_2332.bin");
  ASSERT_TRUE(package->comBinary.has_value());
  EXPECT_EQ(package->comBinary->name, "com_motion-drive-v5.6.10-9010-02-1_2423-netx.bin");
  EXPECT_FALSE(package->sii.has_value());  // This one ships no EEPROM image.
  EXPECT_TRUE(package->extras.empty());
}

// The extracted bytes must be the real decompressed content, not the stored size or a truncation —
// the app binary is deflated in both fixtures, so this is a genuine round trip through zlib.
TEST(FirmwarePackageTest, ExtractsDecompressedContent) {
  const std::vector<uint8_t> zip = readPackage(kCirculoPackage);
  auto package = openFirmwarePackage(zip, defaultSkipFiles());
  ASSERT_TRUE(package) << package.error();
  // The SII in the package is byte-identical to the one the SII parser's own fixture pins, so its
  // first bytes are a known-good anchor rather than a self-fulfilling expectation.
  const std::vector<uint8_t>& sii = package->sii->content;
  ASSERT_GE(sii.size(), 16u);
  EXPECT_EQ(sii[0], 0x8Du);
  EXPECT_EQ(sii[1], 0x3Eu);
  EXPECT_EQ(sii[14], 0x58u);  // The SII checksum byte.
  EXPECT_EQ(sii[16], 0xD2u);  // Vendor id 0x22D2, little-endian.
  EXPECT_EQ(sii[17], 0x22u);
}

TEST(FirmwarePackageTest, AnEmptySkipListKeepsEverything) {
  const std::vector<uint8_t> zip = readPackage(kCirculoPackage);
  auto package = openFirmwarePackage(zip, {});
  ASSERT_TRUE(package) << package.error();
  EXPECT_TRUE(package->skipped.empty());
  ASSERT_EQ(package->extras.size(), 2u);
  EXPECT_EQ(package->extras[0].name, "SOMANET_CiA_402.xml.zip");
  EXPECT_EQ(package->extras[1].name, "stack_image.svg.zip");
  EXPECT_FALSE(package->extras[0].content.empty());
}

// One mechanism for skipping, so naming the SII or a binary in the list works exactly as naming an
// extra does. Skipping the application firmware is a deliberate "install only the rest".
TEST(FirmwarePackageTest, TheSkipListReachesEveryKindOfEntry) {
  const std::vector<uint8_t> zip = readPackage(kCirculoPackage);
  const std::vector<std::string> skip = {"somanet_cia402.sii",
                                         "app_8500-04_motion-drive_v5.6.10_2332.bin"};
  auto package = openFirmwarePackage(zip, skip);
  ASSERT_TRUE(package) << package.error();
  EXPECT_FALSE(package->sii.has_value());
  EXPECT_FALSE(package->appBinary.has_value());
  EXPECT_EQ(package->skipped.size(), 2u);
}

TEST(FirmwarePackageTest, RejectsBytesThatAreNotAZip) {
  const std::vector<uint8_t> notAZip = {'n', 'o', 't', ' ', 'a', ' ', 'z', 'i', 'p'};
  EXPECT_FALSE(openFirmwarePackage(notAZip, {}));
}

TEST(FirmwarePackageTest, RejectsAZipWithNoApplicationFirmware) {
  // The ESI is a zip in its own right, and one that holds no app_*.bin — a convenient stand-in for
  // "a valid archive that is not a firmware package", taken from the package itself.
  const std::vector<uint8_t> zip = readPackage(kCirculoPackage);
  auto outer = openFirmwarePackage(zip, {});
  ASSERT_TRUE(outer) << outer.error();
  auto inner = openFirmwarePackage(outer->extras[0].content, {});
  ASSERT_FALSE(inner);
  EXPECT_NE(inner.error().find("app_*.bin"), std::string::npos) << inner.error();
}

}  // namespace
}  // namespace mm::node
