#include "etg/esi.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "esi_fixtures.h"

namespace mm::etg {
namespace {

using test::kPrimitiveDataTypes;
using test::wrapDictionary;

const EsiDictionary& firstDictionary(const EsiFile& file) {
  return *file.devices.front().profiles.front().dictionary;
}

// ---------------------------------------------------------------------------------------------
// Hard failures. Only three conditions may cost the caller the whole document.
// ---------------------------------------------------------------------------------------------

TEST(EsiParseTest, RejectsMalformedXmlAndNamesTheOffset) {
  const auto result = parseEsi("<EtherCATInfo><Vendor></EtherCATInfo>");
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("offset"), std::string::npos) << result.error();
}

TEST(EsiParseTest, RejectsAnotherEtherCatSchemaAndNamesTheRootItFound) {
  // EtherCATModule and EtherCATDict are sibling schemas with their own root elements. Handing one
  // to this parser is an easy mistake, and "expected EtherCATInfo, found EtherCATModule" is the
  // difference between a two-second fix and a confusing empty result.
  const auto result =
      parseEsi(R"(<?xml version="1.0"?><EtherCATModule><Module/></EtherCATModule>)");
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("EtherCATModule"), std::string::npos) << result.error();
}

TEST(EsiParseTest, RejectsAnEtherCatInfoWithNoDevices) {
  const auto result = parseEsi(R"(<EtherCATInfo><Vendor><Id>1</Id></Vendor></EtherCATInfo>)");
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("Descriptions"), std::string::npos) << result.error();
}

// ---------------------------------------------------------------------------------------------
// Encoding conventions.
// ---------------------------------------------------------------------------------------------

TEST(EsiParseTest, SkipsAUtf8ByteOrderMark) {
  // Real vendor ESIs are written with a BOM. A byte-level reader that compared the first
  // characters against "<?xml" would reject every one of them.
  const std::string xml = "\xEF\xBB\xBF" + wrapDictionary("<DataTypes/><Objects/>");
  const auto result = parseEsi(xml);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(result->vendor.id, 0x22D2u);
}

TEST(EsiParseTest, AcceptsEveryHexDecValueSpelling) {
  // The XSD permits signed decimal or a "#x" hex prefix, and hex-digit case is inconsistent even
  // within one real file — 0x1A00 appears as both "#x1A00" and "#x1a00". Comparing the parsed
  // integers rather than the strings is what makes that a non-issue.
  const auto xml = wrapDictionary(std::format(R"(
      <DataTypes>{}</DataTypes>
      <Objects>
        <Object><Index>#x1A00</Index><Name>Upper</Name><Type>UDINT</Type><BitSize>32</BitSize></Object>
        <Object><Index>#x1a01</Index><Name>Lower</Name><Type>UDINT</Type><BitSize>32</BitSize></Object>
        <Object><Index>6658</Index><Name>Decimal</Name><Type>UDINT</Type><BitSize>32</BitSize></Object>
        <Object><Index>0x1A03</Index><Name>CStyle</Name><Type>UDINT</Type><BitSize>32</BitSize></Object>
      </Objects>)",
                                              kPrimitiveDataTypes));
  const auto result = parseEsi(xml);
  ASSERT_TRUE(result.has_value()) << result.error();

  const auto& objects = firstDictionary(*result).objects;
  ASSERT_EQ(objects.size(), 4u);
  EXPECT_EQ(objects[0].index, 0x1A00u);
  EXPECT_EQ(objects[1].index, 0x1A01u);
  EXPECT_EQ(objects[2].index, 0x1A02u);  // 6658 decimal.
  EXPECT_EQ(objects[3].index, 0x1A03u);
}

TEST(EsiParseTest, ToleratesWhitespaceAroundElementText) {
  // pugixml preserves the indentation around element text, and std::from_chars rejects a trailing
  // space rather than skipping it — so every text read has to be trimmed before parsing.
  const auto xml = wrapDictionary(std::format(R"(
      <DataTypes>{}</DataTypes>
      <Objects>
        <Object>
          <Index>
            #x1000
          </Index>
          <Name>  Padded  </Name>
          <Type>
            UDINT
          </Type>
          <BitSize>32</BitSize>
        </Object>
      </Objects>)",
                                              kPrimitiveDataTypes));
  const auto result = parseEsi(xml);
  ASSERT_TRUE(result.has_value()) << result.error();

  const auto& objects = firstDictionary(*result).objects;
  ASSERT_EQ(objects.size(), 1u);
  EXPECT_EQ(objects[0].index, 0x1000u);
  EXPECT_EQ(objects[0].type, "UDINT");
  EXPECT_EQ(esiText(objects[0].names), "Padded");
}

TEST(EsiParseTest, DecodesHexBinaryLeastSignificantByteFirst) {
  // xs:hexBinary in an ESI is little-endian: "92010200" is 0x00020192, not 0x92010200. Getting
  // this backwards would invert every default, bound and PDO mapping value in the file.
  const auto xml = wrapDictionary(std::format(R"(
      <DataTypes>{}</DataTypes>
      <Objects>
        <Object><Index>#x1000</Index><Name>Device type</Name><Type>UDINT</Type><BitSize>32</BitSize>
          <Info><DefaultData>92010200</DefaultData><MaxData>FFFFFF7F</MaxData></Info>
        </Object>
      </Objects>)",
                                              kPrimitiveDataTypes));
  const auto result = parseEsi(xml);
  ASSERT_TRUE(result.has_value()) << result.error();

  const EsiObject& object = firstDictionary(*result).objects.front();
  EXPECT_EQ(object.info.defaultData, (std::vector<uint8_t>{0x92, 0x01, 0x02, 0x00}));
  EXPECT_EQ(object.info.maxData, (std::vector<uint8_t>{0xFF, 0xFF, 0xFF, 0x7F}));  // INT32_MAX.
}

TEST(EsiParseTest, WarnsAboutMalformedHexBinaryInsteadOfGuessing) {
  const auto xml = wrapDictionary(std::format(R"(
      <DataTypes>{}</DataTypes>
      <Objects>
        <Object><Index>#x1000</Index><Name>Bad</Name><Type>UDINT</Type><BitSize>32</BitSize>
          <Info><DefaultData>NOTHEX</DefaultData></Info>
        </Object>
      </Objects>)",
                                              kPrimitiveDataTypes));
  const auto result = parseEsi(xml);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_TRUE(firstDictionary(*result).objects.front().info.defaultData.empty());
  EXPECT_FALSE(result->warnings.empty());
}

// ---------------------------------------------------------------------------------------------
// Schema shapes that are easy to model wrongly.
// ---------------------------------------------------------------------------------------------

TEST(EsiParseTest, ReadsTheSlotAttributesThatLiveOnTheIndexElement) {
  // DependOnSlot, DependOnSlotGroup and OverwrittenByModule are attributes of <Index>, not of
  // <Object>. Looking for them on the object silently loses the modular-device semantics they
  // carry — the flattener gates its slot relocation on DependOnSlot.
  const auto xml = wrapDictionary(std::format(R"(
      <DataTypes>{}</DataTypes>
      <Objects>
        <Object>
          <Index DependOnSlot="1" OverwrittenByModule="1">#x6000</Index>
          <Name>Relocatable</Name><Type>UDINT</Type><BitSize>32</BitSize>
        </Object>
      </Objects>)",
                                              kPrimitiveDataTypes));
  const auto result = parseEsi(xml);
  ASSERT_TRUE(result.has_value()) << result.error();

  const EsiObject& object = firstDictionary(*result).objects.front();
  EXPECT_TRUE(object.dependOnSlot);
  EXPECT_TRUE(object.overwrittenByModule);
  EXPECT_FALSE(object.dependOnSlotGroup);
}

TEST(EsiParseTest, ReadsBarePropertyChildrenOfADataTypeSubItem) {
  // Object and DataType wrap their annotations in <Properties>; DataType/SubItem does not. The
  // asymmetry is in the XSD, so both placements must be handled.
  const auto xml = wrapDictionary(R"(
      <DataTypes>
        <DataType><Name>USINT</Name><BitSize>8</BitSize></DataType>
        <DataType><Name>UINT</Name><BitSize>16</BitSize></DataType>
        <DataType>
          <Name>DT1C32</Name><BitSize>32</BitSize>
          <SubItem><SubIdx>0</SubIdx><Name>SubIndex 000</Name><Type>USINT</Type>
            <BitSize>8</BitSize><BitOffs>0</BitOffs></SubItem>
          <SubItem><SubIdx>1</SubIdx><Name>Sync type</Name><Type>UINT</Type>
            <BitSize>16</BitSize><BitOffs>16</BitOffs>
            <Property><Name>options</Name><Value>{"Free Run":0}</Value></Property>
          </SubItem>
          <Properties>
            <Property><Name>description</Name><Value>Sync manager parameters.</Value></Property>
          </Properties>
        </DataType>
      </DataTypes>
      <Objects/>)");
  const auto result = parseEsi(xml);
  ASSERT_TRUE(result.has_value()) << result.error();

  const auto& types = firstDictionary(*result).dataTypes;
  const EsiDataType& record = types.back();
  ASSERT_EQ(record.subItems.size(), 2u);
  ASSERT_EQ(record.subItems[1].properties.size(), 1u);
  EXPECT_EQ(record.subItems[1].properties[0].name, "options");
  ASSERT_EQ(record.properties.size(), 1u);
  EXPECT_EQ(record.properties[0].name, "description");
}

TEST(EsiParseTest, HandlesAnEmptyPropertiesElement) {
  // <Properties></Properties> occurs in real files. Dereferencing the wrapper without checking
  // would be a null access on a perfectly valid document.
  const auto xml = wrapDictionary(std::format(R"(
      <DataTypes>{}</DataTypes>
      <Objects>
        <Object><Index>#x1600</Index><Name>Mapping</Name><Type>UDINT</Type><BitSize>32</BitSize>
          <Properties></Properties>
        </Object>
      </Objects>)",
                                              kPrimitiveDataTypes));
  const auto result = parseEsi(xml);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_TRUE(firstDictionary(*result).objects.front().properties.empty());
}

TEST(EsiParseTest, ReadsAnOptionalSubIdxAndMultipleArrayInfoDimensions) {
  // SubIdx is optional (real files omit it on an array's element SubItem) and ArrayInfo is
  // maxOccurs="3", so multi-dimensional arrays are schema-legal even though one dimension is the
  // norm.
  const auto xml = wrapDictionary(R"(
      <DataTypes>
        <DataType><Name>USINT</Name><BitSize>8</BitSize></DataType>
        <DataType>
          <Name>DT1010ARR</Name><BaseType>USINT</BaseType><BitSize>32</BitSize>
          <ArrayInfo><LBound>1</LBound><Elements>4</Elements></ArrayInfo>
          <ArrayInfo><LBound>0</LBound><Elements>2</Elements></ArrayInfo>
        </DataType>
        <DataType>
          <Name>DT1010</Name><BitSize>48</BitSize>
          <SubItem><SubIdx>0</SubIdx><Name>SubIndex 000</Name><Type>USINT</Type>
            <BitSize>8</BitSize><BitOffs>0</BitOffs></SubItem>
          <SubItem><Name>Elements</Name><Type>DT1010ARR</Type>
            <BitSize>32</BitSize><BitOffs>16</BitOffs></SubItem>
        </DataType>
      </DataTypes>
      <Objects/>)");
  const auto result = parseEsi(xml);
  ASSERT_TRUE(result.has_value()) << result.error();

  const auto& types = firstDictionary(*result).dataTypes;
  const EsiDataType& arrayInfoType = types[1];
  ASSERT_EQ(arrayInfoType.arrayInfo.size(), 2u);
  EXPECT_EQ(arrayInfoType.arrayInfo[0].lBound, 1);
  EXPECT_EQ(arrayInfoType.arrayInfo[0].elements, 4);

  const EsiDataType& arrayType = types[2];
  ASSERT_EQ(arrayType.subItems.size(), 2u);
  EXPECT_EQ(arrayType.subItems[0].subIdx, 0);
  EXPECT_FALSE(arrayType.subItems[1].subIdx.has_value());
}

TEST(EsiParseTest, ReadsLocalisedNamesAndPrefersTheRequestedLocale) {
  const auto xml = wrapDictionary(std::format(R"(
      <DataTypes>{}</DataTypes>
      <Objects>
        <Object><Index>#x1000</Index>
          <Name LcId="1033">Device type</Name>
          <Name LcId="1031">Geraetetyp</Name>
          <Type>UDINT</Type><BitSize>32</BitSize>
        </Object>
      </Objects>)",
                                              kPrimitiveDataTypes));
  const auto result = parseEsi(xml);
  ASSERT_TRUE(result.has_value()) << result.error();

  const EsiObject& object = firstDictionary(*result).objects.front();
  EXPECT_EQ(esiText(object.names), "Device type");
  EXPECT_EQ(esiText(object.names, 1031), "Geraetetyp");
  EXPECT_EQ(esiText(object.names, 9999), "Device type");  // Unknown locale falls back.
}

TEST(EsiParseTest, ReadsTheObsoleteMinValueBranchAlongsideMinData) {
  // ObjectInfo models its payload as an xs:choice, and real vendor toolchains emit both branches
  // in the same file — v5.6.6.xml has exactly one MinValue among 374 MinData.
  const auto xml = wrapDictionary(std::format(R"(
      <DataTypes>{}</DataTypes>
      <Objects>
        <Object><Index>#x2050</Index><Name>Controller</Name><Type>DINT</Type><BitSize>32</BitSize>
          <Info><MinValue>-1</MinValue><MaxValue>100</MaxValue><DefaultValue>0</DefaultValue></Info>
        </Object>
      </Objects>)",
                                              kPrimitiveDataTypes));
  const auto result = parseEsi(xml);
  ASSERT_TRUE(result.has_value()) << result.error();

  const EsiObject::Info& info = firstDictionary(*result).objects.front().info;
  EXPECT_EQ(info.minValue, -1);  // The decimal branch permits a leading sign.
  EXPECT_EQ(info.maxValue, 100);
  EXPECT_EQ(info.defaultValue, 0);
  EXPECT_TRUE(info.minData.empty());
}

TEST(EsiParseTest, ReadsPdoEntriesIncludingAlignmentPadding) {
  // A padding entry is index 0 with neither SubIndex nor DataType — the schema makes both
  // optional for exactly this — so their absence must not be treated as a defect.
  const auto xml =
      wrapDictionary(std::format("<DataTypes>{}</DataTypes><Objects/>", kPrimitiveDataTypes),
                     R"(
      <RxPdo Fixed="1" Sm="2">
        <Index>#x1600</Index><Name>Outputs</Name>
        <Entry><Index>#x6040</Index><SubIndex>0</SubIndex><BitLen>16</BitLen>
          <Name>Controlword</Name><DataType>UINT</DataType></Entry>
        <Entry><Index>#x0</Index><BitLen>1</BitLen><Name></Name></Entry>
      </RxPdo>)");
  const auto result = parseEsi(xml);
  ASSERT_TRUE(result.has_value()) << result.error();

  const EsiDevice& device = result->devices.front();
  ASSERT_EQ(device.rxPdos.size(), 1u);
  const EsiPdo& pdo = device.rxPdos.front();
  EXPECT_EQ(pdo.index, 0x1600u);
  EXPECT_EQ(pdo.sm, 2);
  EXPECT_TRUE(pdo.fixed);
  ASSERT_EQ(pdo.entries.size(), 2u);

  EXPECT_FALSE(pdo.entries[0].isPadding());
  EXPECT_EQ(pdo.entries[0].index, 0x6040u);
  EXPECT_EQ(pdo.entries[0].dataType, "UINT");

  EXPECT_TRUE(pdo.entries[1].isPadding());
  EXPECT_EQ(pdo.entries[1].bitLen, 1);
  EXPECT_FALSE(pdo.entries[1].dataType.has_value());
  EXPECT_TRUE(result->warnings.empty()) << result->warnings.front();
}

TEST(EsiParseTest, ReadsSlotsWithAPerSlotIncrementOverride) {
  // The increments exist both on <Slots> and on each <Slot>; the per-slot value wins. A parser
  // that read only the outer one would relocate a module's objects by the wrong step.
  const auto xml =
      wrapDictionary(std::format("<DataTypes>{}</DataTypes><Objects/>", kPrimitiveDataTypes),
                     R"(
      <Slots SlotPdoIncrement="1" SlotIndexIncrement="16" DownloadModuleIdentList="1">
        <Slot MinInstances="1" MaxInstances="1">
          <ModuleIdent Default="1">#x04020001</ModuleIdent>
        </Slot>
        <Slot MinInstances="0" MaxInstances="1" SlotIndexIncrement="32">
          <ModuleIdent Default="1">#x22D20001</ModuleIdent>
          <ModuleIdent>#x22D20002</ModuleIdent>
        </Slot>
        <ModulePdoGroup Alignment="4" RxPdo="#x1701" TxPdo="#x1B01"/>
      </Slots>)");
  const auto result = parseEsi(xml);
  ASSERT_TRUE(result.has_value()) << result.error();

  const EsiSlots& slots = result->devices.front().slots;
  EXPECT_EQ(slots.slotIndexIncrement, 16u);
  EXPECT_EQ(slots.slotPdoIncrement, 1u);
  EXPECT_TRUE(slots.downloadModuleIdentList);
  ASSERT_EQ(slots.slots.size(), 2u);

  EXPECT_FALSE(slots.slots[0].slotIndexIncrement.has_value());
  EXPECT_EQ(slots.slots[1].slotIndexIncrement, 32u);
  EXPECT_EQ(slots.slots[1].minInstances, 0u);
  EXPECT_EQ(slots.slots[1].moduleIdents, (std::vector<uint32_t>{0x22D20001u, 0x22D20002u}));
  EXPECT_EQ(slots.slots[1].defaultModuleIdent, 0x22D20001u);

  ASSERT_EQ(slots.modulePdoGroups.size(), 1u);
  EXPECT_EQ(slots.modulePdoGroups[0].rxPdo, 0x1701u);
  EXPECT_EQ(slots.modulePdoGroups[0].txPdo, 0x1B01u);
}

TEST(EsiParseTest, ReadsDictionaryLocalUnitTypes) {
  const auto xml = wrapDictionary(R"(
      <UnitTypes>
        <UnitType><NotationIndex>#xB6</NotationIndex><Index>#x4B6</Index>
          <Name>Bits</Name><Symbol>Bit</Symbol></UnitType>
      </UnitTypes>
      <DataTypes/><Objects/>)");
  const auto result = parseEsi(xml);
  ASSERT_TRUE(result.has_value()) << result.error();

  const auto& units = firstDictionary(*result).unitTypes;
  ASSERT_EQ(units.size(), 1u);
  EXPECT_EQ(units[0].notationIndex, 0xB6);
  EXPECT_EQ(units[0].symbol, "Bit");
}

TEST(EsiParseTest, ReadsTheMailboxCoeCompleteAccessDefault) {
  // CompleteAccess is not just a capability advertisement: it sets the default SdoAccess for every
  // object whose <Flags> omits the element.
  const auto withCa =
      parseEsi(wrapDictionary("<DataTypes/><Objects/>", "", "",
                              R"(<Mailbox><CoE SdoInfo="1" CompleteAccess="1"/></Mailbox>)"));
  ASSERT_TRUE(withCa.has_value()) << withCa.error();
  ASSERT_TRUE(withCa->devices.front().coe.has_value());
  EXPECT_TRUE(withCa->devices.front().coe->completeAccess);
  EXPECT_TRUE(withCa->devices.front().coe->sdoInfo);

  const auto without = parseEsi(wrapDictionary("<DataTypes/><Objects/>"));
  ASSERT_TRUE(without.has_value()) << without.error();
  EXPECT_FALSE(without->devices.front().coe.has_value());
}

// ---------------------------------------------------------------------------------------------
// Tolerance. One bad element must not cost the rest of the document.
// ---------------------------------------------------------------------------------------------

TEST(EsiParseTest, SkipsAMalformedObjectAndKeepsItsSiblings) {
  const auto xml = wrapDictionary(std::format(R"(
      <DataTypes>{}</DataTypes>
      <Objects>
        <Object><Index>#x1000</Index><Name>Good</Name><Type>UDINT</Type><BitSize>32</BitSize></Object>
        <Object><Index>notanumber</Index><Name>BadIndex</Name><Type>UDINT</Type><BitSize>32</BitSize></Object>
        <Object><Index>#x1002</Index><Name>NoType</Name><BitSize>32</BitSize></Object>
        <Object><Index>#x1003</Index><Name>AlsoGood</Name><Type>UDINT</Type><BitSize>32</BitSize></Object>
      </Objects>)",
                                              kPrimitiveDataTypes));
  const auto result = parseEsi(xml);
  ASSERT_TRUE(result.has_value()) << result.error();

  const auto& objects = firstDictionary(*result).objects;
  ASSERT_EQ(objects.size(), 2u);
  EXPECT_EQ(objects[0].index, 0x1000u);
  EXPECT_EQ(objects[1].index, 0x1003u);
  EXPECT_EQ(result->warnings.size(), 2u);
}

TEST(EsiParseTest, RecordsAnUnfollowedDictionaryFileAsAWarning) {
  // An external <DictionaryFile> is a schema-legal alternative to an inline <Dictionary>. parseEsi
  // is a pure transform over one document and does no file I/O, so it says so rather than
  // returning a device that silently has no objects.
  const std::string xml = R"(<?xml version="1.0"?>
<EtherCATInfo>
  <Vendor><Id>#x22D2</Id></Vendor>
  <Descriptions><Devices><Device>
    <Type ProductCode="#x1" RevisionNo="#x1">Ext</Type>
    <Profile><ProfileNo>402</ProfileNo><DictionaryFile>other.xml</DictionaryFile></Profile>
  </Device></Devices></Descriptions>
</EtherCATInfo>)";
  const auto result = parseEsi(xml);
  ASSERT_TRUE(result.has_value()) << result.error();

  const EsiProfile& profile = result->devices.front().profiles.front();
  EXPECT_EQ(profile.dictionaryFile, "other.xml");
  EXPECT_FALSE(profile.dictionary.has_value());
  ASSERT_FALSE(result->warnings.empty());
  EXPECT_NE(result->warnings.front().find("other.xml"), std::string::npos);
}

TEST(EsiParseTest, AcceptsAModuleProfileWithNoProfileNo) {
  // ProfileNo is optional, and real modules omit it — requiring it would drop their dictionaries.
  const auto xml =
      wrapDictionary("<DataTypes/><Objects/>", "",
                     test::moduleWithDictionary(
                         "#x04020001", "CiA402 Dictionary",
                         std::format("<DataTypes>{}</DataTypes><Objects/>", kPrimitiveDataTypes)));
  const auto result = parseEsi(xml);
  ASSERT_TRUE(result.has_value()) << result.error();

  ASSERT_EQ(result->modules.size(), 1u);
  const EsiModule& module = result->modules.front();
  EXPECT_EQ(module.moduleIdent, 0x04020001u);
  ASSERT_TRUE(module.profile.has_value());
  EXPECT_FALSE(module.profile->profileNo.has_value());
  EXPECT_TRUE(module.profile->dictionary.has_value());
}

// ---------------------------------------------------------------------------------------------
// Lookups.
// ---------------------------------------------------------------------------------------------

TEST(EsiParseTest, LooksUpDevicesAndModules) {
  const auto xml = wrapDictionary(
      "<DataTypes/><Objects/>", "",
      test::moduleWithDictionary("#x04020001", "CiA402 Dictionary", "<DataTypes/><Objects/>"));
  const auto result = parseEsi(xml);
  ASSERT_TRUE(result.has_value()) << result.error();

  EXPECT_NE(findEsiDevice(*result, "Test Device"), nullptr);
  EXPECT_EQ(findEsiDevice(*result, "test device"), nullptr);  // Case-sensitive.
  EXPECT_NE(findEsiDeviceByProductCode(*result, 0x00000201), nullptr);
  EXPECT_EQ(findEsiDeviceByProductCode(*result, 0x00000999), nullptr);
  EXPECT_NE(findEsiDeviceByProductCode(*result, 0x00000201, 0x00010000), nullptr);
  EXPECT_EQ(findEsiDeviceByProductCode(*result, 0x00000201, 0x00020000), nullptr);
  EXPECT_NE(findEsiModule(*result, 0x04020001), nullptr);
  EXPECT_EQ(findEsiModule(*result, 0x04020002), nullptr);
}

TEST(EsiParseTest, ListsTheModuleIdentsEverySlotReferences) {
  const auto xml = wrapDictionary("<DataTypes/><Objects/>",
                                  R"(<Slots SlotIndexIncrement="16">
           <Slot MinInstances="1" MaxInstances="1"><ModuleIdent Default="1">#x0402</ModuleIdent></Slot>
           <Slot MinInstances="0" MaxInstances="1">
             <ModuleIdent Default="1">#x2201</ModuleIdent>
             <ModuleIdent>#x2202</ModuleIdent>
             <ModuleIdent>#x0402</ModuleIdent>
           </Slot>
         </Slots>)");
  const auto result = parseEsi(xml);
  ASSERT_TRUE(result.has_value()) << result.error();

  // Duplicates collapse, keeping the first occurrence, so a merge visits each dictionary once.
  EXPECT_EQ(esiSlotModuleIdents(result->devices.front()),
            (std::vector<uint32_t>{0x0402u, 0x2201u, 0x2202u}));
}

TEST(EsiParseTest, CapsInfoNestingInsteadOfOverflowingTheStack) {
  // ObjectInfoType is recursive by schema and the reader recurses with it. A real file nests two
  // deep; nothing in the XSD says it must. Because this parser reads whatever an HTTP client
  // uploads, an unbounded reader turns a crafted file into a segfault that takes the whole
  // process — RT loop included — with it. Verified: without the cap this test crashed.
  std::string xml = R"(<?xml version="1.0"?>
<EtherCATInfo><Vendor><Id>1</Id></Vendor><Descriptions><Devices><Device>
  <Type ProductCode="#x1" RevisionNo="#x1">Deep</Type>
  <Profile><Dictionary>
    <DataTypes><DataType><Name>UDINT</Name><BitSize>32</BitSize></DataType></DataTypes>
    <Objects><Object><Index>#x1000</Index><Name>Nested</Name><Type>UDINT</Type>
      <BitSize>32</BitSize><Info>)";
  constexpr int kDepth = 20000;
  for (int i = 0; i < kDepth; ++i) {
    xml += "<SubItem><Name>a</Name><Info>";
  }
  for (int i = 0; i < kDepth; ++i) {
    xml += "</Info></SubItem>";
  }
  xml += R"(</Info></Object></Objects>
  </Dictionary></Profile></Device></Devices></Descriptions></EtherCATInfo>)";

  const auto result = parseEsi(xml);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_TRUE(
      std::any_of(result->warnings.begin(), result->warnings.end(),
                  [](const std::string& w) { return w.find("nests deeper") != std::string::npos; }))
      << "the truncation must be reported, not silent";
}

}  // namespace
}  // namespace mm::etg
