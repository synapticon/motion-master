#include "node/hardware_description.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace mm::node {
namespace {

// The specification's own example (§3.2): an ACTILINK, so a device inside an assembly, with every
// field populated. Pinning the parser against the document's example rather than a file shaped by
// what the parser expects.
constexpr std::string_view kSpecificationExample = R"json({
  "fileVersion" : "1.2.0",
  "assembly" : {
    "name" : "ACTILINK Motion Node 60/220 EtherCAT",
    "imageId" : "6000-03-image",
    "id" : "8432",
    "version" : "03",
    "serialNumber" : "8432-03-940592-1025",
    "components" : [
      {"name" : "Motor 220", "version" : "A.2", "serialNumber" : "1234"},
      {"name" : "BiSS 19-bit", "version" : "B.4", "serialNumber" : "1234"},
      {"name" : "Plastic housing", "version" : "C.4", "serialNumber" : "1234"}
    ]
  },
  "device" : {
    "name" : "SOMANET Servo Node 400 EtherCAT",
    "imageId" : "9501-xx",
    "id" : "9501",
    "version" : "01",
    "keyId" : "4622",
    "serialNumber" : "9501-01-040595-4023",
    "macAddress": "01-23-45-67-89-AB",
    "components" : [
      {"name" : "Com EtherCAT", "version" : "A.2", "serialNumber" : "1234"},
      {"name" : "Core C2X", "version" : "B.4", "serialNumber" : "1234"},
      {"name" : "Drive 1000", "version" : "C.4", "serialNumber" : "1234"}
    ]
  }
})json";

// A real Integro read off a drive: an assembly (ACTILINK 6000-01) around a 9010-02 device. Trimmed
// to the fields that matter here.
constexpr std::string_view kIntegroInAssembly = R"json({
  "fileVersion": "1.1.0",
  "device": {
    "macAddress": "40:49:8A:01:21:47",
    "keyId": "2423",
    "serialNumber": "9004-02-0000331-2434",
    "name": "SOMANET Integro Motor mountable 60mm generic",
    "id": "9010",
    "version": "02",
    "components": [
      {"name": "SOMANET Drive Module", "serialNumber": "0007-03-0001086-2433", "version": "B.1"}
    ]
  },
  "assembly": {
    "serialNumber": "6000-01-0000518-2435",
    "name": "SOMANET Actilink S C Line G1, 60mm",
    "id": "6000",
    "version": "01",
    "components": []
  }
})json";

// A legacy SOMANET Node: no assembly, and an empty key id — firmware for it predates encryption.
constexpr std::string_view kNodeWithoutKey = R"json({
  "fileVersion": "1.1.0",
  "device": {
    "macAddress": "40:49:8A:01:01:03",
    "keyId": "",
    "serialNumber": "9500-01-0007927-1836",
    "name": "SOMANET Node (STN-48-33-ECF0)",
    "id": "9500",
    "version": "01",
    "components": []
  }
})json";

TEST(HardwareDescriptionTest, ParsesTheSpecificationExample) {
  auto description = parseHardwareDescription(kSpecificationExample);
  ASSERT_TRUE(description) << description.error();
  EXPECT_EQ(description->fileVersion, "1.2.0");

  EXPECT_EQ(description->device.name, "SOMANET Servo Node 400 EtherCAT");
  EXPECT_EQ(description->device.imageId, "9501-xx");
  EXPECT_EQ(description->device.id, "9501");
  EXPECT_EQ(description->device.version, "01");
  EXPECT_EQ(description->device.keyId, "4622");
  EXPECT_EQ(description->device.serialNumber, "9501-01-040595-4023");
  EXPECT_EQ(description->device.macAddress, "01-23-45-67-89-AB");
  EXPECT_EQ(description->device.buildDescriptor(), "9501-01");
  ASSERT_EQ(description->device.components.size(), 3u);
  EXPECT_EQ(description->device.components[1].name, "Core C2X");
  EXPECT_EQ(description->device.components[1].version, "B.4");
  EXPECT_EQ(description->device.components[1].serialNumber, "1234");

  ASSERT_TRUE(description->assembly.has_value());
  EXPECT_EQ(description->assembly->id, "8432");
  EXPECT_EQ(description->assembly->version, "03");
  EXPECT_EQ(description->assembly->buildDescriptor(), "8432-03");
  // §3.2.2: an assembly carries no key of its own.
  EXPECT_TRUE(description->assembly->keyId.empty());
  EXPECT_EQ(description->assembly->components.size(), 3u);
}

TEST(HardwareDescriptionTest, KeepsALeadingZeroInAVersion) {
  auto description = parseHardwareDescription(kIntegroInAssembly);
  ASSERT_TRUE(description) << description.error();
  // The whole point of holding these as text: "02" read as a number and printed back would be "2",
  // and "9010-2" matches no package ever built.
  EXPECT_EQ(description->device.version, "02");
  EXPECT_EQ(description->device.buildDescriptor(), "9010-02");
  ASSERT_TRUE(description->assembly.has_value());
  EXPECT_EQ(description->assembly->buildDescriptor(), "6000-01");
}

TEST(HardwareDescriptionTest, AcceptsADeviceWithNoAssemblyAndNoKey) {
  auto description = parseHardwareDescription(kNodeWithoutKey);
  ASSERT_TRUE(description) << description.error();
  EXPECT_FALSE(description->assembly.has_value());
  EXPECT_TRUE(description->device.keyId.empty());
  EXPECT_EQ(description->device.buildDescriptor(), "9500-01");
}

TEST(HardwareDescriptionTest, TreatsANumericIdAsItsText) {
  // Every field is specified as a string, but id and version look like numbers and a producer that
  // writes them unquoted should not make the file unreadable.
  auto description = parseHardwareDescription(R"json({"device": {"id": 9501, "version": 1}})json");
  ASSERT_TRUE(description) << description.error();
  EXPECT_EQ(description->device.id, "9501");
  // Nothing can recover the leading zero a number never carried — the file is at fault, and this
  // records that the loss is visible rather than papered over.
  EXPECT_EQ(description->device.version, "1");
}

TEST(HardwareDescriptionTest, DropsAnAssemblyThatCannotContributeADescriptor) {
  constexpr std::string_view content = R"json({
    "device": {"id": "9010", "version": "02"},
    "assembly": {"name": "Something", "components": []}
  })json";
  auto description = parseHardwareDescription(content);
  ASSERT_TRUE(description) << description.error();
  // An assembly with no id or version would yield the descriptor "-", which matches nothing. §4.1
  // says the device descriptor is compatible anyway, so dropping it leaves a usable answer.
  EXPECT_FALSE(description->assembly.has_value());
}

TEST(HardwareDescriptionTest, RejectsWhatIsNotAHardwareDescription) {
  EXPECT_FALSE(parseHardwareDescription("").has_value());
  EXPECT_FALSE(parseHardwareDescription("not json at all").has_value());
  EXPECT_FALSE(parseHardwareDescription("[1, 2, 3]").has_value());
  EXPECT_FALSE(parseHardwareDescription(R"json({"fileVersion": "1.2.0"})json").has_value());
  // The two fields a descriptor is built from are what make this a type check: without them the
  // file is some other JSON that happens to have a "device" key.
  EXPECT_FALSE(parseHardwareDescription(R"json({"device": {"id": "9501"}})json").has_value());
  EXPECT_FALSE(parseHardwareDescription(R"json({"device": {"version": "01"}})json").has_value());
  // A stack_info.json, the other file a drive carries — rejected for the same reason.
  EXPECT_FALSE(parseHardwareDescription(
                   R"json({"mac_address": 1, "stack_serial_number": "x", "boards": []})json")
                   .has_value());
}

TEST(HardwareDescriptionTest, DoesNotEnforceTheFileVersionMajor) {
  // A major bump means "a new parser is needed" (§3.2.1), but refusing one would make a drive
  // flashed with a future file unreadable today. The version is reported; the decision is the
  // caller's.
  auto description = parseHardwareDescription(
      R"json({"fileVersion": "9.0.0", "device": {"id": "1", "version": "2"}})json");
  ASSERT_TRUE(description) << description.error();
  EXPECT_EQ(description->fileVersion, "9.0.0");
}

}  // namespace
}  // namespace mm::node
