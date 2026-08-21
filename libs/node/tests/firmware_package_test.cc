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
// a COM binary and no SII. A synthetic zip would take the shape this parser expects;
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
  EXPECT_EQ(name->fullFirmwareDescriptor, "8500-04-2332");
  EXPECT_EQ(name->softwareName, "motion-drive");
  EXPECT_EQ(name->softwareVersion, "v5.6.10");
  EXPECT_EQ(name->firmwareId, "8500");
  EXPECT_EQ(name->firmwareVersion, "04");
  EXPECT_EQ(name->keyId, "2332");
  EXPECT_EQ(name->buildDescriptor(), "8500-04");
  EXPECT_FALSE(name->fieldbusProtocol.has_value());  // No fieldbus character on this one.
}

TEST(FirmwarePackageNameTest, DecodesTheFieldbusCharacter) {
  auto name = parseFirmwarePackageName(kActilinkPackage);
  ASSERT_TRUE(name) << name.error();
  EXPECT_EQ(name->hardwareName, "ACTILINK");
  EXPECT_EQ(name->fullFirmwareDescriptor, "6000-01-2332-1");
  EXPECT_EQ(name->firmwareId, "6000");
  EXPECT_EQ(name->firmwareVersion, "01");
  EXPECT_EQ(name->keyId, "2332");
  EXPECT_EQ(name->fieldbusProtocol, "1");  // EtherCAT.
}

// The specification's own third example. Its descriptor is not numeric at all, which must parse as
// a valid *name* while leaving every decoded field empty — the descriptor is an opaque string by
// design, so guessing numbers out of it would be worse than reporting none.
TEST(FirmwarePackageNameTest, AcceptsANonNumericDescriptor) {
  auto name = parseFirmwarePackageName(
      "package_Branded-Drive-Elite_MyProduct-v25-key3-ecat_motion-drive_v5.3.0-rc.3.zip");
  ASSERT_TRUE(name) << name.error();
  EXPECT_EQ(name->fullFirmwareDescriptor, "MyProduct-v25-key3-ecat");
  EXPECT_EQ(name->softwareVersion, "v5.3.0-rc.3");
  EXPECT_FALSE(name->firmwareId.has_value());
  EXPECT_FALSE(name->firmwareVersion.has_value());
  EXPECT_FALSE(name->keyId.has_value());
  EXPECT_FALSE(name->buildDescriptor().has_value());
}

TEST(FirmwarePackageNameTest, IgnoresAWindowsDuplicateSuffix) {
  auto name = parseFirmwarePackageName(
      "package_SOMANET-Integro-60_9002-02_motion-drive_v5.4.0-rc.3 (1).zip");
  ASSERT_TRUE(name) << name.error();
  EXPECT_EQ(name->softwareVersion, "v5.4.0-rc.3");
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

// ── Compatibility ──────────────────────────────────────────────────────────────────────────────

// An Integro (device 9010-02, key 2423) inside an ACTILINK assembly (6000-01) — the case that
// exercises every part of descriptor assembly at once: a head that comes from the assembly, a key
// that comes from the device regardless, and a fieldbus character that comes from neither.
constexpr std::string_view kIntegroInAssembly = R"json({
  "fileVersion": "1.1.0",
  "device": {"id": "9010", "version": "02", "keyId": "2423",
             "serialNumber": "9004-02-0000331-2434", "name": "SOMANET Integro 60"},
  "assembly": {"id": "6000", "version": "01",
               "serialNumber": "6000-01-0000518-2435", "name": "SOMANET Actilink S"}
})json";

// A Circulo: no assembly, so its only descriptor is its own.
constexpr std::string_view kCirculo = R"json({
  "fileVersion": "1.1.0",
  "device": {"id": "8500", "version": "04", "keyId": "2332", "name": "SOMANET Circulo 7"}
})json";

// A legacy Node: no assembly and no key, so a descriptor of just two fields.
constexpr std::string_view kNodeWithoutKey = R"json({
  "fileVersion": "1.1.0",
  "device": {"id": "9500", "version": "01", "keyId": "", "name": "SOMANET Node"}
})json";

// An Integro variant selecting EtherCAT, built rather than read from a file so the descriptor tests
// state their own input. The file layout itself is pinned by integro_variant_test.cc.
IntegroVariant etherCatVariant() {
  IntegroVariant variant;
  variant.optionIds = {1, 17, 4};
  return variant;
}

TEST(FirmwareCompatibilityTest, AssemblyDescriptorTakesTheDevicesKey) {
  auto description = parseHardwareDescription(kIntegroInAssembly);
  ASSERT_TRUE(description) << description.error();
  const IntegroVariant variant = etherCatVariant();
  const FullFirmwareDescriptors descriptors = fullFirmwareDescriptors(*description, &variant);
  // §3.4.1: the id and version come from the assembly, the key id from the device — assemblies have
  // no key of their own.
  EXPECT_EQ(descriptors.assembly, "6000-01-2423-1");
  EXPECT_EQ(descriptors.device, "9010-02-2423-1");
}

TEST(FirmwareCompatibilityTest, NoVariantMeansNoFieldbusCharacter) {
  auto description = parseHardwareDescription(kCirculo);
  ASSERT_TRUE(description) << description.error();
  // A Circulo carries no .variant, and its packages are named without a fieldbus character. This is
  // the normal case, not a degraded one.
  const FullFirmwareDescriptors descriptors = fullFirmwareDescriptors(*description, nullptr);
  EXPECT_EQ(descriptors.device, "8500-04-2332");
  EXPECT_FALSE(descriptors.assembly.has_value());
}

TEST(FirmwareCompatibilityTest, AnEmptyKeyIsOmittedRatherThanLeftAsADash) {
  auto description = parseHardwareDescription(kNodeWithoutKey);
  ASSERT_TRUE(description) << description.error();
  const FullFirmwareDescriptors descriptors = fullFirmwareDescriptors(*description, nullptr);
  EXPECT_EQ(descriptors.device, "9500-01");
}

TEST(FirmwareCompatibilityTest, MatchesTheAssemblyPackage) {
  const IntegroVariant variant = etherCatVariant();
  auto verdict = checkFirmwareCompatibility(
      kIntegroInAssembly, "package_ACTILINK_6000-01-2423-1_motion-drive_v5.6.10.zip", &variant);
  ASSERT_TRUE(verdict) << verdict.error();
  EXPECT_TRUE(verdict->compatible());
  EXPECT_EQ(verdict->match, FirmwareMatch::kAssembly);
  EXPECT_EQ(verdict->packageName.softwareVersion, "v5.6.10");
}

TEST(FirmwareCompatibilityTest, MatchesTheGenericDevicePackage) {
  const IntegroVariant variant = etherCatVariant();
  auto verdict = checkFirmwareCompatibility(
      kIntegroInAssembly, "package_SOMANET-Integro_9010-02-2423-1_motion-drive_v5.6.10.zip",
      &variant);
  ASSERT_TRUE(verdict) << verdict.error();
  // §4.1 step 5: both are compatible with the device, and the assembly one is the one to prefer —
  // so this is a match that still has something to say.
  EXPECT_TRUE(verdict->compatible());
  EXPECT_EQ(verdict->match, FirmwareMatch::kDevice);
  EXPECT_NE(verdict->explanation.find("6000-01-2423-1"), std::string::npos) << verdict->explanation;
}

TEST(FirmwareCompatibilityTest, RejectsAPackageForOtherHardware) {
  const IntegroVariant variant = etherCatVariant();
  auto verdict = checkFirmwareCompatibility(
      kIntegroInAssembly, "package_SOMANET-Circulo-7_8500-04-2332_motion-drive_v5.6.10.zip",
      &variant);
  ASSERT_TRUE(verdict) << verdict.error();
  EXPECT_FALSE(verdict->compatible());
  EXPECT_EQ(verdict->match, FirmwareMatch::kNone);
  // Both descriptors are named, because "not this one" is only useful alongside "this one".
  EXPECT_NE(verdict->explanation.find("8500-04-2332"), std::string::npos) << verdict->explanation;
  EXPECT_NE(verdict->explanation.find("9010-02-2423-1"), std::string::npos) << verdict->explanation;
}

TEST(FirmwareCompatibilityTest, TheFieldbusCharacterDecidesTheVerdict) {
  // The specification's reason for the character existing: the same hardware with two fieldbus
  // firmwares takes two different packages, and only the .variant says which.
  const IntegroVariant variant = etherCatVariant();
  auto etherCat = checkFirmwareCompatibility(
      kIntegroInAssembly, "package_ACTILINK_6000-01-2423-1_motion-drive_v5.6.10.zip", &variant);
  ASSERT_TRUE(etherCat) << etherCat.error();
  EXPECT_TRUE(etherCat->compatible());

  auto ethernetIp = checkFirmwareCompatibility(
      kIntegroInAssembly, "package_ACTILINK_6000-01-2423-0_motion-drive_v5.6.10.zip", &variant);
  ASSERT_TRUE(ethernetIp) << ethernetIp.error();
  EXPECT_FALSE(ethernetIp->compatible());

  // And without the variant there is no character to compare, so the four-field package misses.
  auto noVariant = checkFirmwareCompatibility(
      kIntegroInAssembly, "package_ACTILINK_6000-01-2423-1_motion-drive_v5.6.10.zip", nullptr);
  ASSERT_TRUE(noVariant) << noVariant.error();
  EXPECT_FALSE(noVariant->compatible());
  EXPECT_EQ(noVariant->deviceDescriptors.assembly, "6000-01-2423");
}

TEST(FirmwareCompatibilityTest, ComparesTheWholeDescriptorNotItsParts) {
  // Right hardware, wrong encryption key: the build descriptors agree and the packages do not. A
  // check on the decoded product id would accept this.
  auto verdict = checkFirmwareCompatibility(
      kCirculo, "package_SOMANET-Circulo-7_8500-04-9999_motion-drive_v5.6.10.zip");
  ASSERT_TRUE(verdict) << verdict.error();
  EXPECT_FALSE(verdict->compatible());
  EXPECT_EQ(verdict->packageName.buildDescriptor(), "8500-04");
}

TEST(FirmwareCompatibilityTest, ADescriptorOutsideTheNumericConventionStillCompares) {
  // The whole-string comparison is what makes this work: nothing about "MyProduct-v25-key3-ecat"
  // decodes, and it still matches the device that declares exactly that.
  constexpr std::string_view branded = R"json({
    "fileVersion": "1.2.0",
    "device": {"id": "MyProduct", "version": "v25", "keyId": "key3-ecat", "name": "Branded Drive"}
  })json";
  auto verdict = checkFirmwareCompatibility(
      branded, "package_Branded-Drive-Elite_MyProduct-v25-key3-ecat_motion-drive_v5.3.0-rc.3.zip");
  ASSERT_TRUE(verdict) << verdict.error();
  EXPECT_TRUE(verdict->compatible());
  EXPECT_FALSE(verdict->packageName.firmwareId.has_value());
}

TEST(FirmwareCompatibilityTest, UnreadableInputsAreAnErrorRatherThanAVerdict) {
  // A renamed package: there is no verdict to give, and the reason is what a caller shows instead.
  EXPECT_FALSE(checkFirmwareCompatibility(kCirculo, "my-firmware.zip").has_value());
  EXPECT_FALSE(
      checkFirmwareCompatibility("not a hardware description",
                                 "package_SOMANET-Circulo-7_8500-04-2332_motion-drive_v5.6.10.zip")
          .has_value());
}

// The two committed packages against the hardware they were built for, so the descriptors this
// compares are real strings from real releases rather than ones written for the test.
TEST(FirmwareCompatibilityTest, MatchesTheCommittedPackages) {
  auto circulo = checkFirmwareCompatibility(kCirculo, kCirculoPackage);
  ASSERT_TRUE(circulo) << circulo.error();
  EXPECT_TRUE(circulo->compatible());
  EXPECT_EQ(circulo->match, FirmwareMatch::kDevice);

  // The ACTILINK package is named 6000-01-2332-1, so it needs the assembly's key to be 2332.
  constexpr std::string_view actilinkHardware = R"json({
    "fileVersion": "1.1.0",
    "device": {"id": "9010", "version": "02", "keyId": "2332", "name": "SOMANET Integro 60"},
    "assembly": {"id": "6000", "version": "01", "name": "SOMANET Actilink S"}
  })json";
  const IntegroVariant variant = etherCatVariant();
  auto actilink = checkFirmwareCompatibility(actilinkHardware, kActilinkPackage, &variant);
  ASSERT_TRUE(actilink) << actilink.error();
  EXPECT_TRUE(actilink->compatible());
  EXPECT_EQ(actilink->match, FirmwareMatch::kAssembly);
}

TEST(FirmwareCompatibilityTest, NamesEveryMatchKind) {
  EXPECT_EQ(toString(FirmwareMatch::kNone), "none");
  EXPECT_EQ(toString(FirmwareMatch::kAssembly), "assembly");
  EXPECT_EQ(toString(FirmwareMatch::kDevice), "device");
}

}  // namespace
}  // namespace mm::node
