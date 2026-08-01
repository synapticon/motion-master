#include "etg/esi_entry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <format>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#include "esi_fixtures.h"
#include "etg/esi.h"

namespace mm::etg {
namespace {

using test::kPrimitiveDataTypes;
using test::moduleWithDictionary;
using test::wrapDictionary;

/// Parses @p xml and flattens its single device. Fails the test rather than returning on error,
/// so a case body can use the table without unwrapping.
EsiEntryTable flatten(const std::string& xml, const EsiEntryOptions& options = {}) {
  auto file = parseEsi(xml);
  EXPECT_TRUE(file.has_value()) << (file ? std::string{} : file.error());
  if (!file || file->devices.empty()) {
    return {};
  }
  auto table = buildDeviceEntries(*file, file->devices.front(), options);
  EXPECT_TRUE(table.has_value()) << (table ? std::string{} : table.error());
  return table ? std::move(*table) : EsiEntryTable{};
}

const EsiEntry* find(const EsiEntryTable& table, uint16_t index, uint8_t subindex) {
  const auto it = std::find_if(
      table.entries.begin(), table.entries.end(),
      [index, subindex](const EsiEntry& e) { return e.index == index && e.subindex == subindex; });
  return it != table.entries.end() ? &*it : nullptr;
}

bool warnedAbout(const EsiEntryTable& table, std::string_view needle) {
  return std::any_of(table.warnings.begin(), table.warnings.end(), [needle](const std::string& w) {
    return w.find(needle) != std::string::npos;
  });
}

/// A single VAR object with the given <Flags> body.
std::string varWithFlags(std::string_view flagsXml) {
  return wrapDictionary(
      std::format(R"(
      <DataTypes>{}</DataTypes>
      <Objects>
        <Object><Index>#x1000</Index><Name>Scalar</Name><Type>UDINT</Type><BitSize>32</BitSize>
          {}
        </Object>
      </Objects>)",
                  kPrimitiveDataTypes,
                  flagsXml.empty() ? std::string{} : std::format("<Flags>{}</Flags>", flagsXml)));
}

/// A RECORD object (0x2000, two members) where the object and its second DataType SubItem each
/// carry the supplied <Flags> body — the shape every flag-inheritance case needs.
std::string recordWithFlags(std::string_view objectFlags, std::string_view subItemFlags) {
  return wrapDictionary(std::format(
      R"(
      <DataTypes>
        {}
        <DataType>
          <Name>DT2000</Name><BitSize>48</BitSize>
          <SubItem><SubIdx>0</SubIdx><Name>SubIndex 000</Name><Type>USINT</Type>
            <BitSize>8</BitSize><BitOffs>0</BitOffs></SubItem>
          <SubItem><SubIdx>1</SubIdx><Name>Member</Name><Type>UDINT</Type>
            <BitSize>32</BitSize><BitOffs>16</BitOffs>{}</SubItem>
        </DataType>
      </DataTypes>
      <Objects>
        <Object><Index>#x2000</Index><Name>Record</Name><Type>DT2000</Type><BitSize>48</BitSize>
          <Info>
            <SubItem><Name>SubIndex 000</Name><Info><DefaultData>01</DefaultData></Info></SubItem>
            <SubItem><Name>Member</Name><Info><DefaultData>00000000</DefaultData></Info></SubItem>
          </Info>
          {}
        </Object>
      </Objects>)",
      kPrimitiveDataTypes,
      subItemFlags.empty() ? std::string{} : std::format("<Flags>{}</Flags>", subItemFlags),
      objectFlags.empty() ? std::string{} : std::format("<Flags>{}</Flags>", objectFlags)));
}

// ---------------------------------------------------------------------------------------------
// Flag inheritance — ETG.2000 §Figure 37. The four cases below are the whole rule.
// ---------------------------------------------------------------------------------------------

TEST(EsiEntryTest, AVarWithNoFlagsGetsTheSpecDefaults) {
  const EsiEntryTable table = flatten(varWithFlags(""));
  const EsiEntry* entry = find(table, 0x1000, 0);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->access.mode, AccessMode::Ro);
  EXPECT_EQ(entry->category, Category::Optional);
  EXPECT_EQ(entry->pdoMapping, PdoMapping::None);
  EXPECT_FALSE(entry->backup);
}

TEST(EsiEntryTest, ASubItemWithNoFlagsElementInheritsTheObjectFlags) {
  const EsiEntryTable table =
      flatten(recordWithFlags("<Access>rw</Access><Category>m</Category>", ""));
  const EsiEntry* member = find(table, 0x2000, 1);
  ASSERT_NE(member, nullptr);
  EXPECT_EQ(member->access.mode, AccessMode::Rw);
  EXPECT_EQ(member->category, Category::Mandatory);
}

TEST(EsiEntryTest, ASubItemFlagsElementOverridesTheObjectFlags) {
  const EsiEntryTable table =
      flatten(recordWithFlags("<Access>rw</Access>", "<Access>ro</Access>"));
  const EsiEntry* member = find(table, 0x2000, 1);
  ASSERT_NE(member, nullptr);
  EXPECT_EQ(member->access.mode, AccessMode::Ro);
}

TEST(EsiEntryTest, ASubItemFlagsElementShadowsTheObjectWholesale) {
  // The crux of the rule, and the easiest thing to get wrong: once the SubItem has a <Flags>
  // element at all, a flag it does *not* carry falls back to the SPEC DEFAULT — not to the
  // object's value. Here the object says Category=m but the SubItem's Flags mentions only Access,
  // so the member is Optional. Treating the object as a fallback per-flag would report Mandatory
  // and mislead a config tool into thinking the entry must be written.
  const EsiEntryTable table =
      flatten(recordWithFlags("<Access>rw</Access><Category>m</Category>", "<Access>rw</Access>"));
  const EsiEntry* member = find(table, 0x2000, 1);
  ASSERT_NE(member, nullptr);
  EXPECT_EQ(member->access.mode, AccessMode::Rw);
  EXPECT_EQ(member->category, Category::Optional);
}

TEST(EsiEntryTest, AnEmptyAccessElementCountsAsAbsent) {
  // A vendor toolchain emits <Access/> for "no override". Reading it as a value would wrongly
  // shadow the inherited flag with the ro default.
  const EsiEntryTable table = flatten(recordWithFlags("<Access>rw</Access>", "<Access/>"));
  const EsiEntry* member = find(table, 0x2000, 1);
  ASSERT_NE(member, nullptr);
  EXPECT_EQ(member->access.mode, AccessMode::Ro);
}

TEST(EsiEntryTest, ParsesAccessRestrictionsCaseInsensitively) {
  // ETG.2000 requires a tool to accept the legacy "PreOp" spelling identically to "PreOP".
  const EsiEntryTable modern =
      flatten(varWithFlags(R"(<Access WriteRestrictions="PreOP">rw</Access>)"));
  const EsiEntryTable legacy =
      flatten(varWithFlags(R"(<Access WriteRestrictions="PreOp">rw</Access>)"));
  ASSERT_NE(find(modern, 0x1000, 0), nullptr);
  ASSERT_NE(find(legacy, 0x1000, 0), nullptr);
  EXPECT_EQ(find(modern, 0x1000, 0)->access.writeRestrictions, StateRestriction::PreOp);
  EXPECT_EQ(find(legacy, 0x1000, 0)->access.writeRestrictions, StateRestriction::PreOp);
}

TEST(EsiEntryTest, FoldsEveryPdoMappingSpelling) {
  // "T R TR RT t r tr rt" — neither case nor order is significant.
  for (const auto& [spelling, expected] :
       std::vector<std::pair<std::string, PdoMapping>>{{"t", PdoMapping::Tx},
                                                       {"T", PdoMapping::Tx},
                                                       {"r", PdoMapping::Rx},
                                                       {"tr", PdoMapping::TxRx},
                                                       {"RT", PdoMapping::TxRx}}) {
    const EsiEntryTable table =
        flatten(varWithFlags(std::format("<PdoMapping>{}</PdoMapping>", spelling)));
    const EsiEntry* entry = find(table, 0x1000, 0);
    ASSERT_NE(entry, nullptr) << spelling;
    EXPECT_EQ(entry->pdoMapping, expected) << spelling;
  }
}

TEST(EsiEntryTest, SdoAccessComesFromTheObjectAndDefaultsToTheMailboxCapability) {
  // SubItemType/Flags has no SdoAccess element, so every subindex necessarily shares the object's
  // value — a deliberate carve-out from the per-flag inheritance loop. Its default comes from
  // Mailbox/CoE/@CompleteAccess.
  const auto build = [](std::string_view mailbox, std::string_view objectFlags) {
    return wrapDictionary(std::format(R"(
        <DataTypes>
          {}
          <DataType><Name>DT2000</Name><BitSize>48</BitSize>
            <SubItem><SubIdx>0</SubIdx><Name>SubIndex 000</Name><Type>USINT</Type>
              <BitSize>8</BitSize><BitOffs>0</BitOffs></SubItem>
            <SubItem><SubIdx>1</SubIdx><Name>Member</Name><Type>UDINT</Type>
              <BitSize>32</BitSize><BitOffs>16</BitOffs>
              <Flags><Access>ro</Access></Flags></SubItem>
          </DataType>
        </DataTypes>
        <Objects>
          <Object><Index>#x2000</Index><Name>Record</Name><Type>DT2000</Type><BitSize>48</BitSize>
            <Info>
              <SubItem><Name>SubIndex 000</Name><Info><DefaultData>01</DefaultData></Info></SubItem>
              <SubItem><Name>Member</Name><Info/></SubItem>
            </Info>
            {}
          </Object>
        </Objects>)",
                                      kPrimitiveDataTypes, objectFlags),
                          "", "", mailbox);
  };

  const EsiEntryTable subIndexDefault = flatten(build("", ""));
  ASSERT_NE(find(subIndexDefault, 0x2000, 1), nullptr);
  EXPECT_EQ(find(subIndexDefault, 0x2000, 1)->sdoAccess, SdoAccess::SubIndexAccess);

  const EsiEntryTable completeDefault =
      flatten(build(R"(<Mailbox><CoE CompleteAccess="1"/></Mailbox>)", ""));
  ASSERT_NE(find(completeDefault, 0x2000, 1), nullptr);
  EXPECT_EQ(find(completeDefault, 0x2000, 1)->sdoAccess, SdoAccess::CompleteAccess);

  // An explicit value on the object still wins over the mailbox default...
  const EsiEntryTable explicitValue =
      flatten(build(R"(<Mailbox><CoE CompleteAccess="1"/></Mailbox>)",
                    "<Flags><SdoAccess>SubIndexAccess</SdoAccess></Flags>"));
  ASSERT_NE(find(explicitValue, 0x2000, 1), nullptr);
  EXPECT_EQ(find(explicitValue, 0x2000, 1)->sdoAccess, SdoAccess::SubIndexAccess);
  // ...and applies to subindex 0 as well, since it is an object-level property.
  ASSERT_NE(find(explicitValue, 0x2000, 0), nullptr);
  EXPECT_EQ(find(explicitValue, 0x2000, 0)->sdoAccess, SdoAccess::SubIndexAccess);
}

TEST(EsiEntryTest, SynthesisesTheEtg1000ObjAccessBitfield) {
  const EsiEntryTable table = flatten(varWithFlags(
      R"(<Access WriteRestrictions="PreOP">rw</Access><PdoMapping>tr</PdoMapping><Backup>1</Backup>)"));
  const EsiEntry* entry = find(table, 0x1000, 0);
  ASSERT_NE(entry, nullptr);

  // Readable in all three states (bits 0-2), writable in PreOP only (bit 3), RxPDO- and
  // TxPDO-mappable (bits 6-7), backup (bit 8).
  EXPECT_EQ(entry->objAccess & 0b111, 0b111);
  EXPECT_EQ((entry->objAccess >> 3) & 0b111, 0b001);
  EXPECT_TRUE(entry->objAccess & (1u << 6));
  EXPECT_TRUE(entry->objAccess & (1u << 7));
  EXPECT_TRUE(entry->objAccess & (1u << 8));
  EXPECT_FALSE(entry->objAccess & (1u << 9));

  // A read-only object must not advertise any write state.
  const EsiEntryTable readOnly = flatten(varWithFlags("<Access>ro</Access>"));
  ASSERT_NE(find(readOnly, 0x1000, 0), nullptr);
  EXPECT_EQ((find(readOnly, 0x1000, 0)->objAccess >> 3) & 0b111, 0u);
}

// ---------------------------------------------------------------------------------------------
// ARRAY and RECORD expansion.
// ---------------------------------------------------------------------------------------------

/// An ARRAY object: DT1010 with the conventional DT1010ARR element helper type.
std::string arrayObject(int elements) {
  std::string infoSubItems =
      R"(<SubItem><Name>SubIndex 000</Name><Info><DefaultData>03</DefaultData></Info></SubItem>)";
  for (int i = 1; i <= elements; ++i) {
    infoSubItems += std::format(
        R"(<SubItem><Name>SubIndex {:03}</Name><Info><DefaultData>0{}000000</DefaultData></Info></SubItem>)",
        i, i);
  }
  return wrapDictionary(std::format(R"(
      <DataTypes>
        {}
        <DataType><Name>DT1010ARR</Name><BaseType>UDINT</BaseType><BitSize>96</BitSize>
          <ArrayInfo><LBound>1</LBound><Elements>3</Elements></ArrayInfo></DataType>
        <DataType><Name>DT1010</Name><BitSize>112</BitSize>
          <SubItem><SubIdx>0</SubIdx><Name>SubIndex 000</Name><Type>USINT</Type>
            <BitSize>8</BitSize><BitOffs>0</BitOffs>
            <Flags><Access>ro</Access><Category>m</Category></Flags></SubItem>
          <SubItem><Name>Elements</Name><Type>DT1010ARR</Type>
            <BitSize>96</BitSize><BitOffs>16</BitOffs>
            <Flags><Access>rw</Access></Flags></SubItem>
        </DataType>
      </DataTypes>
      <Objects>
        <Object><Index>#x1010</Index><Name>Store parameters</Name><Type>DT1010</Type>
          <BitSize>112</BitSize>
          <Info>{}</Info>
          <Flags><Access>ro</Access><Category>o</Category></Flags>
        </Object>
      </Objects>)",
                                    kPrimitiveDataTypes, infoSubItems));
}

TEST(EsiEntryTest, ExpandsAnArrayWithPositionalSubindices) {
  const EsiEntryTable table = flatten(arrayObject(3));

  ASSERT_EQ(table.entries.size(), 4u);
  for (uint8_t i = 0; i <= 3; ++i) {
    EXPECT_NE(find(table, 0x1010, i), nullptr) << int{i};
  }

  // Subindex 0 of an ARRAY is always USINT, whatever the DataType says.
  const EsiEntry* count = find(table, 0x1010, 0);
  ASSERT_NE(count, nullptr);
  EXPECT_EQ(count->objectCode, ObjectCode::Array);
  EXPECT_EQ(count->dataTypeName, "USINT");
  EXPECT_EQ(count->bitSize, 8);
  EXPECT_EQ(count->numberOfEntries, 3);

  // Elements resolve one hop through the "...ARR" helper type's BaseType.
  const EsiEntry* first = find(table, 0x1010, 1);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->dataTypeName, "UDINT");
  EXPECT_EQ(first->dataType, 0x0007);
  EXPECT_EQ(first->bitSize, 32);
  EXPECT_EQ(first->entryName, "SubIndex 001");
}

TEST(EsiEntryTest, ArrayElementBitOffsetsAdvanceByTheElementWidth) {
  const EsiEntryTable table = flatten(arrayObject(3));
  EXPECT_EQ(find(table, 0x1010, 0)->bitOffset, 0);
  EXPECT_EQ(find(table, 0x1010, 1)->bitOffset, 16);  // After the count plus its alignment pad.
  EXPECT_EQ(find(table, 0x1010, 2)->bitOffset, 48);
  EXPECT_EQ(find(table, 0x1010, 3)->bitOffset, 80);
}

TEST(EsiEntryTest, EveryArrayElementSharesTheSingleElementSubItemFlags) {
  // An ARRAY DataType describes all of 1..N with one SubItem, so its flags apply to every element
  // while subindex 0 keeps its own.
  const EsiEntryTable table = flatten(arrayObject(3));
  EXPECT_EQ(find(table, 0x1010, 0)->access.mode, AccessMode::Ro);
  EXPECT_EQ(find(table, 0x1010, 0)->category, Category::Mandatory);
  for (uint8_t i = 1; i <= 3; ++i) {
    EXPECT_EQ(find(table, 0x1010, i)->access.mode, AccessMode::Rw) << int{i};
  }
}

TEST(EsiEntryTest, TakesRecordSubindicesFromSubIdxNotPosition) {
  // A RECORD may leave gaps in its subindex numbering; using the loop position instead of <SubIdx>
  // would address the wrong entries on the wire.
  const auto xml = wrapDictionary(std::format(R"(
      <DataTypes>
        {}
        <DataType><Name>DT2000</Name><BitSize>80</BitSize>
          <SubItem><SubIdx>0</SubIdx><Name>SubIndex 000</Name><Type>USINT</Type>
            <BitSize>8</BitSize><BitOffs>0</BitOffs></SubItem>
          <SubItem><SubIdx>1</SubIdx><Name>Alpha</Name><Type>UINT</Type>
            <BitSize>16</BitSize><BitOffs>16</BitOffs></SubItem>
          <SubItem><SubIdx>4</SubIdx><Name>Delta</Name><Type>UDINT</Type>
            <BitSize>32</BitSize><BitOffs>32</BitOffs></SubItem>
        </DataType>
      </DataTypes>
      <Objects>
        <Object><Index>#x2000</Index><Name>Sparse</Name><Type>DT2000</Type><BitSize>80</BitSize>
          <Info>
            <SubItem><Name>SubIndex 000</Name><Info><DefaultData>04</DefaultData></Info></SubItem>
            <SubItem><Name>Alpha</Name><Info/></SubItem>
            <SubItem><Name>Delta</Name><Info/></SubItem>
          </Info>
        </Object>
      </Objects>)",
                                              kPrimitiveDataTypes));
  const EsiEntryTable table = flatten(xml);

  EXPECT_EQ(table.entries.size(), 3u);
  EXPECT_NE(find(table, 0x2000, 0), nullptr);
  EXPECT_NE(find(table, 0x2000, 1), nullptr);
  EXPECT_NE(find(table, 0x2000, 4), nullptr);
  EXPECT_EQ(find(table, 0x2000, 2), nullptr);  // No entry invented for the gap.
  EXPECT_EQ(find(table, 0x2000, 4)->entryName, "Delta");
  EXPECT_EQ(find(table, 0x2000, 4)->objectCode, ObjectCode::Record);
}

TEST(EsiEntryTest, PairsRecordSubItemsByNameNotPosition) {
  // The XSD keys DataType/SubItem/Name, so names are unique within a type and by-name matching is
  // well-founded. Here <Info> lists the members in a different order than the DataType does;
  // pairing positionally would attach Beta's default to Alpha.
  const auto xml = wrapDictionary(std::format(R"(
      <DataTypes>
        {}
        <DataType><Name>DT2000</Name><BitSize>48</BitSize>
          <SubItem><SubIdx>0</SubIdx><Name>SubIndex 000</Name><Type>USINT</Type>
            <BitSize>8</BitSize><BitOffs>0</BitOffs></SubItem>
          <SubItem><SubIdx>1</SubIdx><Name>Alpha</Name><Type>UINT</Type>
            <BitSize>16</BitSize><BitOffs>16</BitOffs></SubItem>
          <SubItem><SubIdx>2</SubIdx><Name>Beta</Name><Type>UINT</Type>
            <BitSize>16</BitSize><BitOffs>32</BitOffs></SubItem>
        </DataType>
      </DataTypes>
      <Objects>
        <Object><Index>#x2000</Index><Name>Shuffled</Name><Type>DT2000</Type><BitSize>48</BitSize>
          <Info>
            <SubItem><Name>SubIndex 000</Name><Info><DefaultData>02</DefaultData></Info></SubItem>
            <SubItem><Name>Beta</Name><Info><DefaultData>BBBB</DefaultData></Info></SubItem>
            <SubItem><Name>Alpha</Name><Info><DefaultData>AAAA</DefaultData></Info></SubItem>
          </Info>
        </Object>
      </Objects>)",
                                              kPrimitiveDataTypes));
  const EsiEntryTable table = flatten(xml);

  const EsiEntry* alpha = find(table, 0x2000, 1);
  const EsiEntry* beta = find(table, 0x2000, 2);
  ASSERT_NE(alpha, nullptr);
  ASSERT_NE(beta, nullptr);
  EXPECT_EQ(alpha->entryName, "Alpha");
  EXPECT_EQ(*alpha->defaultData, (std::vector<uint8_t>{0xAA, 0xAA}));
  EXPECT_EQ(beta->entryName, "Beta");
  EXPECT_EQ(*beta->defaultData, (std::vector<uint8_t>{0xBB, 0xBB}));
}

TEST(EsiEntryTest, FallsBackToPositionWhenARecordNameDoesNotMatch) {
  const auto xml = wrapDictionary(std::format(R"(
      <DataTypes>
        {}
        <DataType><Name>DT2000</Name><BitSize>32</BitSize>
          <SubItem><SubIdx>0</SubIdx><Name>SubIndex 000</Name><Type>USINT</Type>
            <BitSize>8</BitSize><BitOffs>0</BitOffs></SubItem>
          <SubItem><SubIdx>1</SubIdx><Name>Alpha</Name><Type>UINT</Type>
            <BitSize>16</BitSize><BitOffs>16</BitOffs></SubItem>
        </DataType>
      </DataTypes>
      <Objects>
        <Object><Index>#x2000</Index><Name>Mismatched</Name><Type>DT2000</Type><BitSize>32</BitSize>
          <Info>
            <SubItem><Name>SubIndex 000</Name><Info><DefaultData>01</DefaultData></Info></SubItem>
            <SubItem><Name>Typo</Name><Info><DefaultData>1234</DefaultData></Info></SubItem>
          </Info>
        </Object>
      </Objects>)",
                                              kPrimitiveDataTypes));
  const EsiEntryTable table = flatten(xml);

  const EsiEntry* member = find(table, 0x2000, 1);
  ASSERT_NE(member, nullptr);
  EXPECT_EQ(member->entryName, "Alpha");  // The DataType names the entry.
  EXPECT_EQ(*member->defaultData, (std::vector<uint8_t>{0x12, 0x34}));
  EXPECT_TRUE(warnedAbout(table, "matched by position"));
}

TEST(EsiEntryTest, ClassifiesBeforePairingSoAnArrayIsNotCorrupted) {
  // An ARRAY's DataType has exactly two SubItems while its <Info> has N+1. Pairing them
  // positionally before classifying would give element 2 the *element helper type's* description
  // and drop elements 3+ entirely. Ten elements against two SubItems makes that unmissable.
  const EsiEntryTable table = flatten(arrayObject(10));
  EXPECT_EQ(table.entries.size(), 11u);
  for (uint8_t i = 1; i <= 10; ++i) {
    const EsiEntry* element = find(table, 0x1010, i);
    ASSERT_NE(element, nullptr) << int{i};
    EXPECT_EQ(element->dataTypeName, "UDINT") << int{i};
  }
}

// ---------------------------------------------------------------------------------------------
// Values.
// ---------------------------------------------------------------------------------------------

TEST(EsiEntryTest, CarriesSignedBoundsAsRawBytesAndFlagsTheirSignedness) {
  // MinData=FF MaxData=01 on a SINT is -1..+1. Compared as bytes the minimum sorts *above* the
  // maximum, which is why the entry reports isSigned rather than pretending the bytes are
  // orderable on their own.
  const auto xml = wrapDictionary(std::format(R"(
      <DataTypes>{}</DataTypes>
      <Objects>
        <Object><Index>#x2000</Index><Name>Direction</Name><Type>SINT</Type><BitSize>8</BitSize>
          <Info><MinData>FF</MinData><MaxData>01</MaxData><DefaultData>01</DefaultData></Info>
        </Object>
      </Objects>)",
                                              kPrimitiveDataTypes));
  const EsiEntryTable table = flatten(xml);

  const EsiEntry* entry = find(table, 0x2000, 0);
  ASSERT_NE(entry, nullptr);
  EXPECT_TRUE(entry->isSigned);
  EXPECT_EQ(*entry->minData, std::vector<uint8_t>{0xFF});
  EXPECT_EQ(*entry->maxData, std::vector<uint8_t>{0x01});
  EXPECT_TRUE(table.warnings.empty()) << table.warnings.front();
}

TEST(EsiEntryTest, EncodesTheObsoleteScalarBranchIntoTheSameByteForm) {
  // MinValue/DefaultValue are a decimal alternative to MinData/DefaultData. Normalising them to
  // the same little-endian bytes means a consumer never has to handle two representations.
  const auto xml = wrapDictionary(std::format(R"(
      <DataTypes>{}</DataTypes>
      <Objects>
        <Object><Index>#x2050</Index><Name>Gain</Name><Type>INT</Type><BitSize>16</BitSize>
          <Info><MinValue>-1</MinValue><MaxValue>256</MaxValue><DefaultValue>0</DefaultValue></Info>
        </Object>
      </Objects>)",
                                              kPrimitiveDataTypes));
  const EsiEntryTable table = flatten(xml);

  const EsiEntry* entry = find(table, 0x2050, 0);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(*entry->minData, (std::vector<uint8_t>{0xFF, 0xFF}));  // -1.
  EXPECT_EQ(*entry->maxData, (std::vector<uint8_t>{0x00, 0x01}));  // 256, little-endian.
  EXPECT_EQ(*entry->defaultData, (std::vector<uint8_t>{0x00, 0x00}));
}

TEST(EsiEntryTest, PadsAShortValueToItsDeclaredWidthAndSaysSo) {
  // Real ESIs write "01" for a 32-bit minimum. Padding is the useful behaviour, but it must be
  // reported: silently widening data is how a wrong bound becomes invisible.
  const auto xml = wrapDictionary(std::format(R"(
      <DataTypes>{}</DataTypes>
      <Objects>
        <Object><Index>#x6075</Index><Name>Current</Name><Type>UDINT</Type><BitSize>32</BitSize>
          <Info><MinData>01</MinData></Info>
        </Object>
        <Object><Index>#x6076</Index><Name>Signed</Name><Type>DINT</Type><BitSize>32</BitSize>
          <Info><MinData>FF</MinData></Info>
        </Object>
      </Objects>)",
                                              kPrimitiveDataTypes));
  const EsiEntryTable padded = flatten(xml);

  EXPECT_EQ(*find(padded, 0x6075, 0)->minData, (std::vector<uint8_t>{0x01, 0x00, 0x00, 0x00}));
  // A signed short value sign-extends, so -1 stays -1 rather than becoming 255.
  EXPECT_EQ(*find(padded, 0x6076, 0)->minData, (std::vector<uint8_t>{0xFF, 0xFF, 0xFF, 0xFF}));
  EXPECT_TRUE(warnedAbout(padded, "declared width"));

  EsiEntryOptions verbatim;
  verbatim.padShortValues = false;
  const EsiEntryTable unpadded = flatten(xml, verbatim);
  EXPECT_EQ(*find(unpadded, 0x6075, 0)->minData, std::vector<uint8_t>{0x01});
  EXPECT_TRUE(warnedAbout(unpadded, "left as written"));
}

TEST(EsiEntryTest, DecodesUnitsThroughTheDictionaryLocalTable) {
  const auto xml = wrapDictionary(std::format(R"(
      <UnitTypes>
        <UnitType><NotationIndex>#xB6</NotationIndex><Name>Bits</Name><Symbol>Bit</Symbol></UnitType>
      </UnitTypes>
      <DataTypes>{}</DataTypes>
      <Objects>
        <Object><Index>#x2000</Index><Name>Voltage</Name><Type>UDINT</Type><BitSize>32</BitSize>
          <Info><Unit>#xFD260000</Unit></Info>
        </Object>
        <Object><Index>#x2001</Index><Name>Width</Name><Type>UDINT</Type><BitSize>32</BitSize>
          <Info><Unit>#x00B60000</Unit></Info>
        </Object>
      </Objects>)",
                                              kPrimitiveDataTypes));
  const EsiEntryTable table = flatten(xml);

  EXPECT_EQ(find(table, 0x2000, 0)->unitSymbol, "mV");
  EXPECT_EQ(find(table, 0x2000, 0)->unit, 0xFD260000u);
  // The dictionary redefines 0xB6, which is "rpm" in the standard catalogue.
  EXPECT_EQ(find(table, 0x2001, 0)->unitSymbol, "Bit");
}

TEST(EsiEntryTest, PrefersAnInfoDisplayNameOverTheSubItemName) {
  const auto xml = wrapDictionary(std::format(R"(
      <DataTypes>{}</DataTypes>
      <Objects>
        <Object><Index>#x2000</Index><Name>Raw</Name><Type>UDINT</Type><BitSize>32</BitSize>
          <Info><DisplayName>Friendly name</DisplayName></Info>
        </Object>
      </Objects>)",
                                              kPrimitiveDataTypes));
  const EsiEntryTable table = flatten(xml);

  const EsiEntry* entry = find(table, 0x2000, 0);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->objectName, "Raw");
  EXPECT_EQ(entry->entryName, "Raw");
  EXPECT_EQ(entry->displayName, "Friendly name");
}

// ---------------------------------------------------------------------------------------------
// Properties, descriptions and enum options.
// ---------------------------------------------------------------------------------------------

TEST(EsiEntryTest, PutsObjectAnnotationOnSubindexZeroAndMemberAnnotationOnItsOwnRow) {
  const auto xml = wrapDictionary(std::format(R"(
      <DataTypes>
        {}
        <DataType><Name>DT2000</Name><BitSize>32</BitSize>
          <SubItem><SubIdx>0</SubIdx><Name>SubIndex 000</Name><Type>USINT</Type>
            <BitSize>8</BitSize><BitOffs>0</BitOffs></SubItem>
          <SubItem><SubIdx>1</SubIdx><Name>Member</Name><Type>UINT</Type>
            <BitSize>16</BitSize><BitOffs>16</BitOffs>
            <Property><Name>description</Name><Value>The member.</Value></Property></SubItem>
          <Properties>
            <Property><Name>description</Name><Value>The record.</Value></Property>
          </Properties>
        </DataType>
      </DataTypes>
      <Objects>
        <Object><Index>#x1000</Index><Name>Scalar</Name><Type>UDINT</Type><BitSize>32</BitSize>
          <Properties><Property><Name>description</Name><Value>The scalar.</Value></Property></Properties>
        </Object>
        <Object><Index>#x2000</Index><Name>Record</Name><Type>DT2000</Type><BitSize>32</BitSize>
          <Info>
            <SubItem><Name>SubIndex 000</Name><Info><DefaultData>01</DefaultData></Info></SubItem>
            <SubItem><Name>Member</Name><Info/></SubItem>
          </Info>
        </Object>
      </Objects>)",
                                              kPrimitiveDataTypes));
  const EsiEntryTable table = flatten(xml);

  // A VAR is its own subindex 0, and its annotation lives on the Object.
  const EsiEntry* scalar = find(table, 0x1000, 0);
  ASSERT_NE(scalar, nullptr);
  EXPECT_EQ(scalar->description, "The scalar.");

  // A composite's subindex 0 *is* the object, so it carries the DataType-level description...
  const EsiEntry* record = find(table, 0x2000, 0);
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->description, "The record.");

  // ...and each member carries only its own. Copying the object's onto every subindex is what
  // made a real device's JSON 4.7 MB, 83% of it the same HTML repeated.
  const EsiEntry* member = find(table, 0x2000, 1);
  ASSERT_NE(member, nullptr);
  EXPECT_EQ(member->description, "The member.");
}

TEST(EsiEntryTest, AnArrayElementCarriesNoDescriptionOfItsOwn) {
  // An ESI describes an array once, for the array as a whole — there is no per-element text. So
  // only subindex 0 has anything to say; an element row that echoed the object's would be noise
  // repeated N times.
  const EsiEntryTable table = flatten(wrapDictionary(std::format(R"(
      <DataTypes>
        {}
        <DataType><Name>DT1010ARR</Name><BaseType>UDINT</BaseType><BitSize>64</BitSize>
          <ArrayInfo><LBound>1</LBound><Elements>2</Elements></ArrayInfo></DataType>
        <DataType><Name>DT1010</Name><BitSize>80</BitSize>
          <SubItem><SubIdx>0</SubIdx><Name>SubIndex 000</Name><Type>USINT</Type>
            <BitSize>8</BitSize><BitOffs>0</BitOffs></SubItem>
          <SubItem><Name>Elements</Name><Type>DT1010ARR</Type>
            <BitSize>64</BitSize><BitOffs>16</BitOffs></SubItem>
          <Properties>
            <Property><Name>description</Name><Value>Stores parameters.</Value></Property>
          </Properties>
        </DataType>
      </DataTypes>
      <Objects>
        <Object><Index>#x1010</Index><Name>Store parameters</Name><Type>DT1010</Type>
          <BitSize>80</BitSize>
          <Info>
            <SubItem><Name>SubIndex 000</Name><Info><DefaultData>02</DefaultData></Info></SubItem>
            <SubItem><Name>SubIndex 001</Name><Info/></SubItem>
            <SubItem><Name>SubIndex 002</Name><Info/></SubItem>
          </Info>
        </Object>
      </Objects>)",
                                                                 kPrimitiveDataTypes)));

  EXPECT_EQ(find(table, 0x1010, 0)->description, "Stores parameters.");
  EXPECT_TRUE(find(table, 0x1010, 1)->description.empty());
  EXPECT_TRUE(find(table, 0x1010, 2)->description.empty());
}

TEST(EsiEntryTest, CarriesUnknownPropertiesVerbatim) {
  // "description" and "options" are conventions, not schema. A vendor using its own names must
  // keep its data — the decoded conveniences simply stay empty.
  const auto xml = wrapDictionary(std::format(R"(
      <DataTypes>{}</DataTypes>
      <Objects>
        <Object><Index>#x1000</Index><Name>Vendor thing</Name><Type>UDINT</Type><BitSize>32</BitSize>
          <Properties>
            <Property><Name>calibrationCurve</Name><Value>1,2,3</Value></Property>
            <Property><Name>engineeringNotes</Name><Value>See drawing A-42.</Value></Property>
          </Properties>
        </Object>
      </Objects>)",
                                              kPrimitiveDataTypes));
  const EsiEntryTable table = flatten(xml);

  const EsiEntry* entry = find(table, 0x1000, 0);
  ASSERT_NE(entry, nullptr);
  ASSERT_EQ(entry->properties.size(), 2u);
  EXPECT_EQ(entry->properties[0].name, "calibrationCurve");
  EXPECT_EQ(entry->properties[0].value, "1,2,3");
  EXPECT_EQ(entry->properties[1].value, "See drawing A-42.");
  EXPECT_TRUE(entry->description.empty());
  EXPECT_TRUE(entry->options.empty());
}

TEST(EsiEntryTest, MergesEnumInfoWithTheOptionsProperty) {
  const auto xml = wrapDictionary(std::format(R"(
      <DataTypes>
        {}
        <DataType><Name>DT2000EN08</Name><BaseType>USINT</BaseType><BitSize>8</BitSize>
          <EnumInfo><Text>Disabled</Text><Enum>0</Enum></EnumInfo>
          <EnumInfo><Text>Activated</Text><Enum>255</Enum></EnumInfo>
        </DataType>
      </DataTypes>
      <Objects>
        <Object><Index>#x2000</Index><Name>Mode</Name><Type>DT2000EN08</Type><BitSize>8</BitSize>
          <Properties>
            <Property><Name>options</Name><Value>{{"Disabled":0,"Special":7}}</Value></Property>
          </Properties>
        </Object>
      </Objects>)",
                                              kPrimitiveDataTypes));
  const EsiEntryTable table = flatten(xml);

  const EsiEntry* entry = find(table, 0x2000, 0);
  ASSERT_NE(entry, nullptr);
  // EnumInfo comes first and wins the duplicate label; the property contributes what is new.
  ASSERT_EQ(entry->options.size(), 3u);
  EXPECT_EQ(entry->options[0].label, "Disabled");
  EXPECT_EQ(entry->options[0].value, 0);
  EXPECT_EQ(entry->options[1].label, "Activated");
  EXPECT_EQ(entry->options[1].value, 255);
  EXPECT_EQ(entry->options[2].label, "Special");
  EXPECT_EQ(entry->options[2].value, 7);
}

TEST(EsiEntryTest, WarnsAboutMalformedOptionsJsonInsteadOfThrowing) {
  const auto xml = wrapDictionary(std::format(R"(
      <DataTypes>{}</DataTypes>
      <Objects>
        <Object><Index>#x2000</Index><Name>Mode</Name><Type>USINT</Type><BitSize>8</BitSize>
          <Properties><Property><Name>options</Name><Value>not json</Value></Property></Properties>
        </Object>
      </Objects>)",
                                              kPrimitiveDataTypes));
  const EsiEntryTable table = flatten(xml);

  ASSERT_NE(find(table, 0x2000, 0), nullptr);
  EXPECT_TRUE(find(table, 0x2000, 0)->options.empty());
  EXPECT_TRUE(warnedAbout(table, "options"));
}

// ---------------------------------------------------------------------------------------------
// Module merging, slot offsets and collisions.
// ---------------------------------------------------------------------------------------------

/// A device with one mandatory slot and a module dictionary declaring @p moduleObjects.
std::string deviceWithModule(std::string_view deviceObjects, std::string_view moduleObjects,
                             std::string_view slotsXml, uint32_t ident = 0x04020001) {
  return wrapDictionary(
      std::format("<DataTypes>{}</DataTypes><Objects>{}</Objects>", kPrimitiveDataTypes,
                  deviceObjects),
      slotsXml,
      moduleWithDictionary(std::format("#x{:08X}", ident), "Test Module",
                           std::format("<DataTypes>{}</DataTypes><Objects>{}</Objects>",
                                       kPrimitiveDataTypes, moduleObjects)));
}

constexpr std::string_view kOneSlot =
    R"(<Slots SlotIndexIncrement="16" SlotPdoIncrement="1">
         <Slot MinInstances="1" MaxInstances="1"><ModuleIdent Default="1">#x04020001</ModuleIdent></Slot>
       </Slots>)";

TEST(EsiEntryTest, MergesModuleDictionariesAndRecordsWhereEachEntryCameFrom) {
  const EsiEntryTable table = flatten(deviceWithModule(
      R"(<Object><Index>#x1000</Index><Name>Device type</Name><Type>UDINT</Type><BitSize>32</BitSize></Object>)",
      R"(<Object><Index>#x6040</Index><Name>Controlword</Name><Type>UINT</Type><BitSize>16</BitSize></Object>)",
      kOneSlot));

  ASSERT_EQ(table.entries.size(), 2u);

  const EsiEntry* deviceEntry = find(table, 0x1000, 0);
  ASSERT_NE(deviceEntry, nullptr);
  EXPECT_EQ(deviceEntry->source.kind, EsiEntrySource::Kind::Device);
  EXPECT_EQ(deviceEntry->source.slot, -1);

  const EsiEntry* moduleEntry = find(table, 0x6040, 0);
  ASSERT_NE(moduleEntry, nullptr);
  EXPECT_EQ(moduleEntry->source.kind, EsiEntrySource::Kind::Module);
  EXPECT_EQ(moduleEntry->source.moduleIdent, 0x04020001u);
  EXPECT_EQ(moduleEntry->source.slot, 0);
}

TEST(EsiEntryTest, RelocatesOnlyObjectsMarkedDependOnSlot) {
  // ETG.2000 gates the slot offset on Index/@DependOnSlot. Relocating unconditionally would invent
  // indices the device does not answer to, which is worse than omitting them — hence the
  // conservative default, with an opt-out for a vendor that relies on the looser reading.
  constexpr std::string_view kTwoSlots =
      R"(<Slots SlotIndexIncrement="16" SlotPdoIncrement="1">
           <Slot MinInstances="1" MaxInstances="1"><ModuleIdent Default="1">#x04020001</ModuleIdent></Slot>
           <Slot MinInstances="1" MaxInstances="1"><ModuleIdent Default="1">#x04020002</ModuleIdent></Slot>
         </Slots>)";
  const std::string xml = wrapDictionary(
      std::format("<DataTypes>{}</DataTypes><Objects/>", kPrimitiveDataTypes), kTwoSlots,
      moduleWithDictionary("#x04020001", "Slot 0 module",
                           std::format(R"(<DataTypes>{}</DataTypes><Objects>
             <Object><Index>#x6000</Index><Name>Fixed</Name><Type>UINT</Type><BitSize>16</BitSize></Object>
           </Objects>)",
                                       kPrimitiveDataTypes)) +
          moduleWithDictionary("#x04020002", "Slot 1 module",
                               std::format(R"(<DataTypes>{}</DataTypes><Objects>
                 <Object><Index DependOnSlot="1">#x7000</Index><Name>Movable</Name><Type>UINT</Type><BitSize>16</BitSize></Object>
                 <Object><Index>#x7100</Index><Name>Anchored</Name><Type>UINT</Type><BitSize>16</BitSize></Object>
               </Objects>)",
                                           kPrimitiveDataTypes)));

  const EsiEntryTable table = flatten(xml);

  // Slot 1 with increment 16: the marked object moves to 0x7010, the unmarked one stays put.
  const EsiEntry* movable = find(table, 0x7010, 0);
  ASSERT_NE(movable, nullptr);
  EXPECT_EQ(movable->rawIndex, 0x7000);
  EXPECT_EQ(movable->source.indexOffset, 16u);
  EXPECT_NE(find(table, 0x7100, 0), nullptr);
  EXPECT_EQ(find(table, 0x7100, 0)->source.indexOffset, 0u);

  // Slot 0's ordinal is zero, so its objects never move regardless of the flag.
  EXPECT_NE(find(table, 0x6000, 0), nullptr);

  // Opting out relocates every module object in a non-zero slot.
  EsiEntryOptions loose;
  loose.requireDependOnSlot = false;
  const EsiEntryTable relocated = flatten(xml, loose);
  EXPECT_NE(find(relocated, 0x7010, 0), nullptr);
  EXPECT_NE(find(relocated, 0x7110, 0), nullptr);

  // And disabling offsets entirely leaves everything at its declared index.
  EsiEntryOptions none;
  none.applySlotOffsets = false;
  const EsiEntryTable literal = flatten(xml, none);
  EXPECT_NE(find(literal, 0x7000, 0), nullptr);
  EXPECT_NE(find(literal, 0x7100, 0), nullptr);
}

TEST(EsiEntryTest, UsesTheSlotPdoIncrementForObjectsInThePdoAreas) {
  // ETG.5001 moves PDO mapping/assignment objects by SlotPdoIncrement and everything else by
  // SlotIndexIncrement. One increment for both would misplace one or the other.
  constexpr std::string_view kTwoSlots =
      R"(<Slots SlotIndexIncrement="16" SlotPdoIncrement="1">
           <Slot MinInstances="1" MaxInstances="1"><ModuleIdent Default="1">#x04020001</ModuleIdent></Slot>
           <Slot MinInstances="1" MaxInstances="1"><ModuleIdent Default="1">#x04020002</ModuleIdent></Slot>
         </Slots>)";
  const std::string xml = wrapDictionary(
      std::format("<DataTypes>{}</DataTypes><Objects/>", kPrimitiveDataTypes), kTwoSlots,
      moduleWithDictionary(
          "#x04020001", "Slot 0",
          std::format("<DataTypes>{}</DataTypes><Objects/>", kPrimitiveDataTypes)) +
          moduleWithDictionary("#x04020002", "Slot 1",
                               std::format(R"(<DataTypes>{}</DataTypes><Objects>
                 <Object><Index DependOnSlot="1">#x1700</Index><Name>Mapping</Name><Type>UDINT</Type><BitSize>32</BitSize></Object>
                 <Object><Index DependOnSlot="1">#x2600</Index><Name>Parameter</Name><Type>UDINT</Type><BitSize>32</BitSize></Object>
               </Objects>)",
                                           kPrimitiveDataTypes)));

  const EsiEntryTable table = flatten(xml);
  EXPECT_NE(find(table, 0x1701, 0), nullptr) << "PDO-area object should move by SlotPdoIncrement";
  EXPECT_NE(find(table, 0x2610, 0), nullptr) << "other object should move by SlotIndexIncrement";
}

TEST(EsiEntryTest, APerSlotIncrementOverridesTheSlotsLevelValue) {
  constexpr std::string_view kOverride =
      R"(<Slots SlotIndexIncrement="16" SlotPdoIncrement="1">
           <Slot MinInstances="1" MaxInstances="1"><ModuleIdent Default="1">#x04020001</ModuleIdent></Slot>
           <Slot MinInstances="1" MaxInstances="1" SlotIndexIncrement="256">
             <ModuleIdent Default="1">#x04020002</ModuleIdent></Slot>
         </Slots>)";
  const std::string xml = wrapDictionary(
      std::format("<DataTypes>{}</DataTypes><Objects/>", kPrimitiveDataTypes), kOverride,
      moduleWithDictionary(
          "#x04020001", "Slot 0",
          std::format("<DataTypes>{}</DataTypes><Objects/>", kPrimitiveDataTypes)) +
          moduleWithDictionary("#x04020002", "Slot 1",
                               std::format(R"(<DataTypes>{}</DataTypes><Objects>
                 <Object><Index DependOnSlot="1">#x2000</Index><Name>Movable</Name><Type>UINT</Type><BitSize>16</BitSize></Object>
               </Objects>)",
                                           kPrimitiveDataTypes)));

  const EsiEntryTable table = flatten(xml);
  EXPECT_NE(find(table, 0x2100, 0), nullptr) << "0x2000 + 1 * 256";
  EXPECT_EQ(find(table, 0x2010, 0), nullptr) << "the <Slots>-level 16 must not win";
}

TEST(EsiEntryTest, ResolvesACollisionByPolicyAndAggregatesTheReport) {
  constexpr std::string_view kChoiceSlot =
      R"(<Slots SlotIndexIncrement="16">
           <Slot MinInstances="0" MaxInstances="1">
             <ModuleIdent Default="1">#x00000001</ModuleIdent>
             <ModuleIdent>#x00000002</ModuleIdent>
           </Slot>
         </Slots>)";
  const auto moduleObjects = [](std::string_view name) {
    return std::format(R"(<DataTypes>{}</DataTypes><Objects>
        <Object><Index>#x6000</Index><Name>{}</Name><Type>UINT</Type><BitSize>16</BitSize></Object>
      </Objects>)",
                       kPrimitiveDataTypes, name);
  };
  const std::string xml = wrapDictionary(
      std::format("<DataTypes>{}</DataTypes><Objects/>", kPrimitiveDataTypes), kChoiceSlot,
      moduleWithDictionary("#x00000001", "Variant A", moduleObjects("From A")) +
          moduleWithDictionary("#x00000002", "Variant B", moduleObjects("From B")));

  // The default merges every ident the slot offers, which necessarily collides; last wins.
  const EsiEntryTable lastWins = flatten(xml);
  ASSERT_NE(find(lastWins, 0x6000, 0), nullptr);
  EXPECT_EQ(find(lastWins, 0x6000, 0)->objectName, "From B");
  EXPECT_TRUE(warnedAbout(lastWins, "each declare the same"));
  // One line per source pair, not one per colliding entry.
  EXPECT_EQ(lastWins.warnings.size(), 1u);

  EsiEntryOptions first;
  first.collisionPolicy = EsiCollisionPolicy::FirstWins;
  const EsiEntryTable firstWins = flatten(xml, first);
  EXPECT_EQ(find(firstWins, 0x6000, 0)->objectName, "From A");

  // Naming one ident models a real configuration and removes the collision entirely.
  EsiEntryOptions narrowed;
  narrowed.moduleIdents = {0x00000002};
  const EsiEntryTable single = flatten(xml, narrowed);
  EXPECT_EQ(find(single, 0x6000, 0)->objectName, "From B");
  EXPECT_TRUE(single.warnings.empty());

  // Error refuses the merge outright, for a caller that would rather not guess.
  auto file = parseEsi(xml);
  ASSERT_TRUE(file.has_value()) << file.error();
  EsiEntryOptions strict;
  strict.collisionPolicy = EsiCollisionPolicy::Error;
  const auto failed = buildDeviceEntries(*file, file->devices.front(), strict);
  EXPECT_FALSE(failed.has_value());
}

TEST(EsiEntryTest, AModuleWinsAnObjectTheDeviceMarkedOverwrittenByModule) {
  // ETG.2000's own override mechanism, and it is opt-in on the device side, so it outranks the
  // collision policy: a device that explicitly permits an override should get one.
  const std::string xml = deviceWithModule(
      R"(<Object><Index OverwrittenByModule="1">#x6000</Index><Name>Device version</Name><Type>UINT</Type><BitSize>16</BitSize></Object>)",
      R"(<Object><Index>#x6000</Index><Name>Module version</Name><Type>UINT</Type><BitSize>16</BitSize></Object>)",
      kOneSlot);

  EsiEntryOptions first;
  first.collisionPolicy = EsiCollisionPolicy::FirstWins;
  const EsiEntryTable table = flatten(xml, first);

  ASSERT_NE(find(table, 0x6000, 0), nullptr);
  EXPECT_EQ(find(table, 0x6000, 0)->objectName, "Module version");
  EXPECT_TRUE(warnedAbout(table, "OverwrittenByModule"));
}

TEST(EsiEntryTest, WarnsWhenASlotIdentResolvesToNothing) {
  const std::string xml = wrapDictionary(
      std::format("<DataTypes>{}</DataTypes><Objects><Object><Index>#x1000</Index><Name>Only</Name>"
                  "<Type>UDINT</Type><BitSize>32</BitSize></Object></Objects>",
                  kPrimitiveDataTypes),
      R"(<Slots><Slot MinInstances="1" MaxInstances="1">
           <ModuleIdent Default="1">#xDEADBEEF</ModuleIdent></Slot></Slots>)");
  const EsiEntryTable table = flatten(xml);

  EXPECT_EQ(table.entries.size(), 1u);  // The device's own dictionary still comes through.
  EXPECT_TRUE(warnedAbout(table, "0xDEADBEEF"));
}

// ---------------------------------------------------------------------------------------------
// Robustness.
// ---------------------------------------------------------------------------------------------

TEST(EsiEntryTest, EmitsADegenerateEntryForAnUnresolvableTypeAndKeepsTheRest) {
  const auto xml = wrapDictionary(std::format(R"(
      <DataTypes>{}</DataTypes>
      <Objects>
        <Object><Index>#x1000</Index><Name>Good</Name><Type>UDINT</Type><BitSize>32</BitSize></Object>
        <Object><Index>#x1001</Index><Name>Unknown type</Name><Type>DT9999</Type><BitSize>32</BitSize></Object>
        <Object><Index>#x1002</Index><Name>Also good</Name><Type>UINT</Type><BitSize>16</BitSize></Object>
      </Objects>)",
                                              kPrimitiveDataTypes));
  const EsiEntryTable table = flatten(xml);

  EXPECT_EQ(table.entries.size(), 3u);
  const EsiEntry* degenerate = find(table, 0x1001, 0);
  ASSERT_NE(degenerate, nullptr);
  EXPECT_EQ(degenerate->dataType, 0);  // Unresolved, and said so.
  EXPECT_EQ(degenerate->bitSize, 32);  // The object's own declared width still applies.
  EXPECT_TRUE(warnedAbout(table, "DT9999"));
  EXPECT_NE(find(table, 0x1002, 0), nullptr);
}

TEST(EsiEntryTest, TerminatesOnASelfReferentialBaseType) {
  // A malformed dictionary can make BaseType resolution cycle. Without a hop cap this recurses
  // until the stack runs out — a crash on a merely bad input file.
  const auto xml = wrapDictionary(std::format(R"(
      <DataTypes>
        {}
        <DataType><Name>Loop</Name><BaseType>Loop</BaseType><BitSize>16</BitSize></DataType>
        <DataType><Name>Ping</Name><BaseType>Pong</BaseType><BitSize>16</BitSize></DataType>
        <DataType><Name>Pong</Name><BaseType>Ping</BaseType><BitSize>16</BitSize></DataType>
      </DataTypes>
      <Objects>
        <Object><Index>#x1000</Index><Name>Self</Name><Type>Loop</Type><BitSize>16</BitSize></Object>
        <Object><Index>#x1001</Index><Name>Mutual</Name><Type>Ping</Type><BitSize>16</BitSize></Object>
      </Objects>)",
                                              kPrimitiveDataTypes));
  const EsiEntryTable table = flatten(xml);

  EXPECT_EQ(table.entries.size(), 2u);
  EXPECT_EQ(find(table, 0x1000, 0)->dataType, 0);
  EXPECT_EQ(find(table, 0x1001, 0)->dataType, 0);
}

TEST(EsiEntryTest, FailsWhenTheDeviceHasNoDictionaryAtAll) {
  // An empty vector would read as "this device has no objects", which is indistinguishable from
  // success. A structural absence deserves an error.
  const std::string xml = R"(<?xml version="1.0"?>
<EtherCATInfo>
  <Vendor><Id>#x22D2</Id></Vendor>
  <Descriptions><Devices><Device>
    <Type ProductCode="#x1" RevisionNo="#x1">Bare</Type>
  </Device></Devices></Descriptions>
</EtherCATInfo>)";
  auto file = parseEsi(xml);
  ASSERT_TRUE(file.has_value()) << file.error();

  const auto table = buildDeviceEntries(*file, file->devices.front());
  ASSERT_FALSE(table.has_value());
  EXPECT_NE(table.error().find("no object dictionary"), std::string::npos) << table.error();
}

TEST(EsiEntryTest, CapsAndSummarisesTheWarningList) {
  // A deliberately corrupt file must not be able to allocate an unbounded warning list.
  std::string objects;
  for (int i = 0; i < 50; ++i) {
    objects += std::format(
        R"(<Object><Index>#x{:04X}</Index><Name>Bad {}</Name><Type>DT9999</Type><BitSize>32</BitSize></Object>)",
        0x3000 + i, i);
  }
  const auto xml = wrapDictionary(
      std::format("<DataTypes>{}</DataTypes><Objects>{}</Objects>", kPrimitiveDataTypes, objects));

  EsiEntryOptions capped;
  capped.maxWarnings = 10;
  const EsiEntryTable table = flatten(xml, capped);

  EXPECT_EQ(table.entries.size(), 50u);   // Every entry still comes through.
  EXPECT_EQ(table.warnings.size(), 11u);  // 10 warnings plus the summary line.
  EXPECT_NE(table.warnings.back().find("further warnings suppressed"), std::string::npos);
}

TEST(EsiEntryTest, SortsByAddressByDefaultAndPreservesDocumentOrderOnRequest) {
  const auto xml = wrapDictionary(std::format(R"(
      <DataTypes>{}</DataTypes>
      <Objects>
        <Object><Index>#x6060</Index><Name>Later</Name><Type>SINT</Type><BitSize>8</BitSize></Object>
        <Object><Index>#x1000</Index><Name>Earlier</Name><Type>UDINT</Type><BitSize>32</BitSize></Object>
      </Objects>)",
                                              kPrimitiveDataTypes));

  const EsiEntryTable sorted = flatten(xml);
  ASSERT_EQ(sorted.entries.size(), 2u);
  EXPECT_EQ(sorted.entries[0].index, 0x1000);
  EXPECT_EQ(sorted.entries[1].index, 0x6060);

  EsiEntryOptions documentOrder;
  documentOrder.sortByAddress = false;
  const EsiEntryTable unsorted = flatten(xml, documentOrder);
  ASSERT_EQ(unsorted.entries.size(), 2u);
  EXPECT_EQ(unsorted.entries[0].index, 0x6060);
}

TEST(EsiEntryTest, SerialisesAnEntryWithRawBytesAsHexBinary) {
  const auto xml = wrapDictionary(std::format(R"(
      <DataTypes>{}</DataTypes>
      <Objects>
        <Object><Index>#x1000</Index><Name>Device type</Name><Type>UDINT</Type><BitSize>32</BitSize>
          <Info><DefaultData>92010200</DefaultData><Unit>#xFD260000</Unit></Info>
          <Flags><Access>rw</Access><Category>m</Category><PdoMapping>r</PdoMapping></Flags>
        </Object>
      </Objects>)",
                                              kPrimitiveDataTypes));
  const EsiEntryTable table = flatten(xml);
  const nlohmann::json j = *find(table, 0x1000, 0);

  EXPECT_EQ(j.at("index"), 0x1000);
  EXPECT_EQ(j.at("subindex"), 0);
  EXPECT_EQ(j.at("objectName"), "Device type");
  EXPECT_EQ(j.at("objectCode"), "VAR");
  EXPECT_EQ(j.at("objectCodeValue"), 0x07);
  EXPECT_EQ(j.at("dataTypeName"), "UDINT");
  EXPECT_EQ(j.at("dataType"), 0x0007);
  EXPECT_EQ(j.at("bitSize"), 32);
  EXPECT_EQ(j.at("category"), "m");
  EXPECT_EQ(j.at("pdoMapping"), "r");
  EXPECT_EQ(j.at("access").at("mode"), "rw");
  // Bytes go out in the same spelling the ESI itself uses, so a value round-trips visually
  // against the source file.
  EXPECT_EQ(j.at("defaultData"), "92010200");
  EXPECT_EQ(j.at("unitSymbol"), "mV");
  EXPECT_EQ(j.at("source").at("kind"), "device");
  // Absent optionals are omitted rather than emitted as null.
  EXPECT_FALSE(j.contains("minData"));
  EXPECT_FALSE(j.contains("attribute"));
}

TEST(EsiEntryTest, KeepsASubItemDescriptionWhenTheObjectAlsoHasOne) {
  // Subindex 0 of a composite carries the object's properties concatenated with its SubItem's, and
  // the description that gets surfaced is dropped from the vector so it is not sent twice. Only
  // that one: erasing every match would silently discard a SubItem's own description whenever the
  // object had one too, which is exactly the loss `properties` exists to prevent. Schema-legal and
  // absent from every real file seen, so nothing but this test would catch it.
  const EsiEntryTable table = flatten(wrapDictionary(std::format(R"(
      <DataTypes>
        {}
        <DataType><Name>DT2000</Name><BitSize>32</BitSize>
          <SubItem><SubIdx>0</SubIdx><Name>SubIndex 000</Name><Type>USINT</Type>
            <BitSize>8</BitSize><BitOffs>0</BitOffs>
            <Property><Name>description</Name><Value>Count of entries.</Value></Property>
          </SubItem>
          <SubItem><SubIdx>1</SubIdx><Name>Member</Name><Type>UINT</Type>
            <BitSize>16</BitSize><BitOffs>16</BitOffs></SubItem>
          <Properties>
            <Property><Name>description</Name><Value>The record.</Value></Property>
          </Properties>
        </DataType>
      </DataTypes>
      <Objects>
        <Object><Index>#x2000</Index><Name>Record</Name><Type>DT2000</Type><BitSize>32</BitSize>
          <Info>
            <SubItem><Name>SubIndex 000</Name><Info><DefaultData>01</DefaultData></Info></SubItem>
            <SubItem><Name>Member</Name><Info/></SubItem>
          </Info>
        </Object>
      </Objects>)",
                                                                 kPrimitiveDataTypes)));

  const EsiEntry* si0 = find(table, 0x2000, 0);
  ASSERT_NE(si0, nullptr);
  // The object's is the one surfaced, and the one removed from the raw vector...
  EXPECT_EQ(si0->description, "The record.");
  // ...while the SubItem's survives there rather than being erased along with it.
  ASSERT_EQ(si0->properties.size(), 1u);
  EXPECT_EQ(si0->properties[0].name, "description");
  EXPECT_EQ(si0->properties[0].value, "Count of entries.");
}

}  // namespace
}  // namespace mm::etg
