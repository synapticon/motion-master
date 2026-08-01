#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "etg/esi.h"
#include "etg/esi_entry.h"

namespace mm::etg {
namespace {

// A real Synapticon ESI (firmware v5.6.6), committed verbatim. The hand-written fixtures in the
// other test files pin one semantic rule each; this file pins the parser against a document no
// test author shaped — 50 000 lines, four devices, five modules, and every real-world
// irregularity the format permits.
//
// Assertions favour structural invariants and standards-fixed values (CiA 402 object semantics)
// over vendor-specific detail, so a firmware bump that adds objects does not turn the suite red
// for no reason.
std::filesystem::path fixturePath() {
  return std::filesystem::path(MM_ETG_TEST_DATA_DIR) / "somanet-v5.6.6.xml";
}

const EsiFile& somanetEsi() {
  // Parsing 1.9 MB per test would dominate the suite's runtime; the document is immutable, so
  // one parse serves every case.
  static const EsiFile* file = [] {
    auto parsed = parseEsiFile(fixturePath());
    EXPECT_TRUE(parsed.has_value()) << (parsed ? std::string{} : parsed.error());
    return new EsiFile(parsed ? std::move(*parsed) : EsiFile{});
  }();
  return *file;
}

const EsiEntry* findEntry(const EsiEntryTable& table, uint16_t index, uint8_t subindex) {
  const auto it = std::find_if(
      table.entries.begin(), table.entries.end(),
      [index, subindex](const EsiEntry& e) { return e.index == index && e.subindex == subindex; });
  return it != table.entries.end() ? &*it : nullptr;
}

const EsiEntry* findNamedEntry(const EsiEntryTable& table, uint16_t index, std::string_view name) {
  const auto it = std::find_if(
      table.entries.begin(), table.entries.end(),
      [index, name](const EsiEntry& e) { return e.index == index && e.entryName == name; });
  return it != table.entries.end() ? &*it : nullptr;
}

TEST(EsiSomanetTest, ParsesTheVendorAndAllFourDevices) {
  const EsiFile& esi = somanetEsi();

  EXPECT_EQ(esi.vendor.id, 0x22D2u);  // Synapticon.
  ASSERT_EQ(esi.devices.size(), 4u);

  EXPECT_EQ(esi.devices[0].type, "SOMANET Node");
  EXPECT_EQ(esi.devices[1].type, "SOMANET Circulo");
  EXPECT_EQ(esi.devices[2].type, "SOMANET Circulo SMM");
  EXPECT_EQ(esi.devices[3].type, "SOMANET Integro");

  EXPECT_EQ(esi.devices[0].productCode, 0x00000201u);
  EXPECT_EQ(esi.devices[1].productCode, 0x00000301u);
  EXPECT_EQ(esi.devices[2].productCode, 0x00000302u);
  EXPECT_EQ(esi.devices[3].productCode, 0x00000401u);

  for (const EsiDevice& device : esi.devices) {
    EXPECT_EQ(device.revisionNo, 0x11000003u) << device.type;
    ASSERT_EQ(device.profiles.size(), 1u) << device.type;
    ASSERT_TRUE(device.profiles.front().dictionary.has_value()) << device.type;
    EXPECT_EQ(device.profiles.front().profileNo, 402) << device.type;
  }
}

TEST(EsiSomanetTest, ParsesAllFiveModules) {
  const EsiFile& esi = somanetEsi();
  ASSERT_EQ(esi.modules.size(), 5u);

  const EsiModule* cia402 = findEsiModule(esi, 0x04020001);
  ASSERT_NE(cia402, nullptr);
  EXPECT_EQ(cia402->type, "CiA402 Dictionary");
  ASSERT_TRUE(cia402->profile.has_value());
  ASSERT_TRUE(cia402->profile->dictionary.has_value());
  EXPECT_EQ(cia402->profile->dictionary->objects.size(), 128u);

  // The four FSoE variants: the two "safe Torque" ones are supersets of the two without it.
  for (const auto& [ident, objects] : std::vector<std::pair<uint32_t, std::size_t>>{
           {0x22D20001u, 59u}, {0x22D20002u, 59u}, {0x22D20003u, 67u}, {0x22D20004u, 67u}}) {
    const EsiModule* module = findEsiModule(esi, ident);
    ASSERT_NE(module, nullptr) << std::format("0x{:08X}", ident);
    ASSERT_TRUE(module->profile.has_value()) << std::format("0x{:08X}", ident);
    ASSERT_TRUE(module->profile->dictionary.has_value()) << std::format("0x{:08X}", ident);
    EXPECT_EQ(module->profile->dictionary->objects.size(), objects)
        << std::format("0x{:08X}", ident);
    // A module's <Profile> carries only a <Dictionary> — ProfileNo is optional and these omit it.
    EXPECT_FALSE(module->profile->profileNo.has_value()) << std::format("0x{:08X}", ident);
  }
}

TEST(EsiSomanetTest, ParsesSlotsIncludingTheOptionalFsoeSlotOfTheSmmDevice) {
  const EsiFile& esi = somanetEsi();

  // Three of the four devices have one mandatory slot holding the CiA402 dictionary.
  for (const std::string_view type : {"SOMANET Node", "SOMANET Circulo", "SOMANET Integro"}) {
    const EsiDevice* device = findEsiDevice(esi, type);
    ASSERT_NE(device, nullptr) << type;
    ASSERT_EQ(device->slots.slots.size(), 1u) << type;
    EXPECT_EQ(device->slots.slots[0].minInstances, 1u) << type;
    EXPECT_EQ(device->slots.slots[0].moduleIdents, std::vector<uint32_t>{0x04020001u}) << type;
    EXPECT_EQ(device->slots.slotIndexIncrement, 16u) << type;
    EXPECT_EQ(device->slots.slotPdoIncrement, 1u) << type;
  }

  // The safe-motion device adds a second, *optional* slot offering four mutually exclusive FSoE
  // modules. This is the only place in the file where a slot ordinal is non-zero, and the only
  // place a slot presents a choice.
  const EsiDevice* smm = findEsiDevice(esi, "SOMANET Circulo SMM");
  ASSERT_NE(smm, nullptr);
  ASSERT_EQ(smm->slots.slots.size(), 2u);
  EXPECT_EQ(smm->slots.slots[0].minInstances, 1u);
  EXPECT_EQ(smm->slots.slots[1].minInstances, 0u);
  EXPECT_EQ(smm->slots.slots[1].maxInstances, 1u);
  EXPECT_EQ(smm->slots.slots[1].moduleIdents,
            (std::vector<uint32_t>{0x22D20001u, 0x22D20002u, 0x22D20003u, 0x22D20004u}));
  EXPECT_EQ(smm->slots.slots[1].defaultModuleIdent, 0x22D20001u);
  EXPECT_TRUE(smm->slots.downloadModuleIdentList);
  EXPECT_EQ(smm->slots.modulePdoGroups.size(), 2u);
}

TEST(EsiSomanetTest, FlattensADeviceIntoItsMergedDictionary) {
  const EsiFile& esi = somanetEsi();
  const EsiDevice* node = findEsiDevice(esi, "SOMANET Node");
  ASSERT_NE(node, nullptr);

  const auto table = buildDeviceEntries(esi, *node);
  ASSERT_TRUE(table.has_value()) << (table ? std::string{} : table.error());

  // 40 communication objects from the device dictionary plus 128 CiA402 objects from the module.
  std::unordered_set<uint16_t> indices;
  for (const EsiEntry& entry : table->entries) {
    indices.insert(entry.index);
  }
  EXPECT_EQ(indices.size(), 168u);

  // Communication objects come from the device; every CiA402 object exists only in the module.
  const EsiEntry* deviceType = findEntry(*table, 0x1000, 0);
  ASSERT_NE(deviceType, nullptr);
  EXPECT_EQ(deviceType->source.kind, EsiEntrySource::Kind::Device);

  const EsiEntry* controlword = findEntry(*table, 0x6040, 0);
  ASSERT_NE(controlword, nullptr);
  EXPECT_EQ(controlword->source.kind, EsiEntrySource::Kind::Module);
  EXPECT_EQ(controlword->source.moduleIdent, 0x04020001u);
  EXPECT_EQ(controlword->source.slot, 0);
}

TEST(EsiSomanetTest, ResolvesStandardCia402ObjectsCorrectly) {
  const EsiFile& esi = somanetEsi();
  const EsiDevice* node = findEsiDevice(esi, "SOMANET Node");
  ASSERT_NE(node, nullptr);
  const auto table = buildDeviceEntries(esi, *node);
  ASSERT_TRUE(table.has_value()) << (table ? std::string{} : table.error());

  // 0x6040 Controlword: a read/write, RxPDO-mappable UINT. Fixed by CiA 402, not by the vendor.
  const EsiEntry* controlword = findEntry(*table, 0x6040, 0);
  ASSERT_NE(controlword, nullptr);
  EXPECT_EQ(controlword->objectName, "Controlword");
  EXPECT_EQ(controlword->objectCode, ObjectCode::Var);
  EXPECT_EQ(controlword->dataTypeName, "UINT");
  EXPECT_EQ(controlword->dataType, 0x0006);  // ETG.1020 UNSIGNED16.
  EXPECT_EQ(controlword->bitSize, 16);
  EXPECT_FALSE(controlword->isSigned);
  EXPECT_EQ(controlword->access.mode, AccessMode::Rw);
  EXPECT_EQ(controlword->pdoMapping, PdoMapping::Rx);

  // 0x6041 Statusword is the read-only, TxPDO-mappable counterpart.
  const EsiEntry* statusword = findEntry(*table, 0x6041, 0);
  ASSERT_NE(statusword, nullptr);
  EXPECT_EQ(statusword->access.mode, AccessMode::Ro);
  EXPECT_EQ(statusword->pdoMapping, PdoMapping::Tx);

  // 0x6064 Position actual value: a signed 32-bit input.
  const EsiEntry* position = findEntry(*table, 0x6064, 0);
  ASSERT_NE(position, nullptr);
  EXPECT_EQ(position->dataTypeName, "DINT");
  EXPECT_EQ(position->dataType, 0x0004);  // ETG.1020 INTEGER32.
  EXPECT_TRUE(position->isSigned);
  EXPECT_EQ(position->bitSize, 32);

  // 0x1018 Identity: a RECORD whose subindex 0 is the entry count and whose members are named.
  const EsiEntry* identityCount = findEntry(*table, 0x1018, 0);
  ASSERT_NE(identityCount, nullptr);
  EXPECT_EQ(identityCount->objectCode, ObjectCode::Record);
  EXPECT_EQ(identityCount->dataTypeName, "USINT");
  const EsiEntry* vendorId = findEntry(*table, 0x1018, 1);
  ASSERT_NE(vendorId, nullptr);
  EXPECT_EQ(vendorId->entryName, "Vendor ID");
  EXPECT_EQ(vendorId->dataTypeName, "UDINT");
  EXPECT_EQ(vendorId->access.mode, AccessMode::Ro);
}

TEST(EsiSomanetTest, CarriesPerSubitemBoundsUnitsDescriptionsAndOptions) {
  const EsiFile& esi = somanetEsi();
  const EsiDevice* node = findEsiDevice(esi, "SOMANET Node");
  ASSERT_NE(node, nullptr);
  const auto table = buildDeviceEntries(esi, *node);
  ASSERT_TRUE(table.has_value()) << (table ? std::string{} : table.error());

  // 0x2004 "Brake options" is the richest RECORD in the file: per-subitem defaults, bounds and
  // engineering units — exactly the metadata the CoE SDO-Information service cannot supply, and
  // therefore the reason this library exists.
  const EsiEntry* brakeCount = findEntry(*table, 0x2004, 0);
  ASSERT_NE(brakeCount, nullptr);
  EXPECT_EQ(brakeCount->objectName, "Brake options");
  EXPECT_EQ(brakeCount->objectCode, ObjectCode::Record);

  const EsiEntry* releaseStrategy = findNamedEntry(*table, 0x2004, "Release strategy");
  ASSERT_NE(releaseStrategy, nullptr);
  ASSERT_TRUE(releaseStrategy->minData.has_value());
  ASSERT_TRUE(releaseStrategy->maxData.has_value());
  EXPECT_EQ(*releaseStrategy->minData, std::vector<uint8_t>{0x00});
  EXPECT_EQ(*releaseStrategy->maxData, std::vector<uint8_t>{0x02});

  // A signed bound: "Initial movement direction (pin brake)" runs -1..+1 on a SINT, which is
  // MinData=FF MaxData=01 — bytes that compare backwards unless read through the type.
  const EsiEntry* direction =
      findNamedEntry(*table, 0x2004, "Initial movement direction (pin brake)");
  ASSERT_NE(direction, nullptr);
  EXPECT_TRUE(direction->isSigned);
  ASSERT_TRUE(direction->minData.has_value());
  EXPECT_EQ(*direction->minData, std::vector<uint8_t>{0xFF});

  // 0x6086 Motion profile type is the file's proof that padding has to be sign-aware. It is a
  // 16-bit INT whose bounds are written one byte short: MinData=80, MaxData=00. Zero-filled those
  // read 128 and 0 — a minimum above its maximum, which is not a range at all. Sign-extended they
  // read -128 and 0: linear ramp (0) plus the manufacturer-specific negative range CiA 402
  // reserves. Same bytes, one coherent reading.
  const EsiEntry* profileType = findEntry(*table, 0x6086, 0);
  ASSERT_NE(profileType, nullptr);
  EXPECT_EQ(profileType->dataTypeName, "INT");
  EXPECT_TRUE(profileType->isSigned);
  ASSERT_TRUE(profileType->minData.has_value());
  ASSERT_TRUE(profileType->maxData.has_value());
  EXPECT_EQ(*profileType->minData, (std::vector<uint8_t>{0x80, 0xFF}));  // -128, sign-extended.
  EXPECT_EQ(*profileType->maxData, (std::vector<uint8_t>{0x00, 0x00}));  // 0, zero-filled.

  const bool anyUnit =
      std::any_of(table->entries.begin(), table->entries.end(), [](const EsiEntry& e) {
        return e.index == 0x2004 && e.unit.has_value() && !e.unitSymbol.empty();
      });
  EXPECT_TRUE(anyUnit);

  const bool anyDescription = std::any_of(table->entries.begin(), table->entries.end(),
                                          [](const EsiEntry& e) { return !e.description.empty(); });
  EXPECT_TRUE(anyDescription);

  // Synapticon carries enum labels in an "options" property rather than <EnumInfo>; both feed the
  // same field.
  const bool anyOptions = std::any_of(table->entries.begin(), table->entries.end(),
                                      [](const EsiEntry& e) { return !e.options.empty(); });
  EXPECT_TRUE(anyOptions);

  // Whatever the convention, the raw properties survive so a vendor using other names loses
  // nothing.
  const bool anyRawProperty = std::any_of(table->entries.begin(), table->entries.end(),
                                          [](const EsiEntry& e) { return !e.properties.empty(); });
  EXPECT_TRUE(anyRawProperty);
}

TEST(EsiSomanetTest, MergesEveryFsoeVariantByDefaultResolvingCollisionsToTheLast) {
  const EsiFile& esi = somanetEsi();
  const EsiDevice* smm = findEsiDevice(esi, "SOMANET Circulo SMM");
  ASSERT_NE(smm, nullptr);

  // The default merges every ident any slot references. Slot 1 offers four mutually exclusive
  // FSoE modules, so this necessarily collides — and last-wins lands on 0x22D20004, the variant
  // with both Parameter and safe Torque, whose object set is a superset of the other three.
  const auto table = buildDeviceEntries(esi, *smm);
  ASSERT_TRUE(table.has_value()) << (table ? std::string{} : table.error());

  const EsiEntry* fsoeCommand = findEntry(*table, 0x6770, 1);
  ASSERT_NE(fsoeCommand, nullptr);
  EXPECT_EQ(fsoeCommand->source.moduleIdent, 0x22D20004u);
  EXPECT_EQ(fsoeCommand->source.slot, 1);

  EXPECT_TRUE(std::any_of(table->warnings.begin(), table->warnings.end(), [](const std::string& w) {
    return w.find("each declare the same") != std::string::npos;
  })) << "a silent merge would hide which variant won";

  // Four mutually exclusive variants collide on every subindex of every shared object — several
  // hundred individual clashes. They are tallied per source pair, so the report stays readable;
  // one line per clash would drown the genuine warnings and blow up the HTTP response.
  EXPECT_LT(table->warnings.size(), 30u)
      << table->warnings.size() << " warning(s) — collisions are meant to be aggregated";

  // Naming idents models one real configuration instead, and then nothing collides.
  EsiEntryOptions single;
  single.moduleIdents = {0x04020001u, 0x22D20001u};
  const auto narrowed = buildDeviceEntries(esi, *smm, single);
  ASSERT_TRUE(narrowed.has_value()) << (narrowed ? std::string{} : narrowed.error());
  const EsiEntry* narrowedCommand = findEntry(*narrowed, 0x6770, 1);
  ASSERT_NE(narrowedCommand, nullptr);
  EXPECT_EQ(narrowedCommand->source.moduleIdent, 0x22D20001u);
  EXPECT_FALSE(std::any_of(
      narrowed->warnings.begin(), narrowed->warnings.end(),
      [](const std::string& w) { return w.find("each declare the same") != std::string::npos; }));
}

TEST(EsiSomanetTest, EveryDeviceFlattensWithoutStructuralDefects) {
  const EsiFile& esi = somanetEsi();

  for (const EsiDevice& device : esi.devices) {
    const auto table = buildDeviceEntries(esi, device);
    ASSERT_TRUE(table.has_value())
        << device.type << ": " << (table ? std::string{} : table.error());
    EXPECT_FALSE(table->entries.empty()) << device.type;

    // A flat table is addressed by (index, subindex); a duplicate would make a lookup ambiguous.
    std::unordered_set<uint32_t> keys;
    for (const EsiEntry& entry : table->entries) {
      EXPECT_TRUE(keys.insert(entry.key()).second)
          << device.type << ": duplicate "
          << std::format("0x{:04X}:{:02X}", entry.index, entry.subindex);
    }

    EXPECT_TRUE(
        std::is_sorted(table->entries.begin(), table->entries.end(),
                       [](const EsiEntry& a, const EsiEntry& b) { return a.key() < b.key(); }))
        << device.type;

    // Every entry either resolved to an ETG.1020 code or said why not. An unresolved type passing
    // silently would reach mm::node::decodeSdoBytes as code 0 and decode as garbage.
    for (const EsiEntry& entry : table->entries) {
      EXPECT_FALSE(entry.objectName.empty())
          << device.type << ": " << std::format("0x{:04X}:{:02X}", entry.index, entry.subindex);
      if (entry.dataType != 0) {
        continue;
      }
      const std::string address = std::format("0x{:04X}:{:02X}", entry.index, entry.subindex);
      EXPECT_TRUE(std::any_of(
          table->warnings.begin(), table->warnings.end(),
          [&address](const std::string& w) { return w.find(address) != std::string::npos; }))
          << device.type << ": unresolved type '" << entry.dataTypeName << "' at " << address
          << " with no warning";
    }
  }
}

TEST(EsiSomanetTest, ParsesProcessDataIncludingPaddingEntries) {
  const EsiFile& esi = somanetEsi();
  const EsiDevice* node = findEsiDevice(esi, "SOMANET Node");
  ASSERT_NE(node, nullptr);

  ASSERT_FALSE(node->rxPdos.empty());
  ASSERT_FALSE(node->txPdos.empty());
  EXPECT_EQ(node->rxPdos.front().index, 0x1600u);
  EXPECT_EQ(node->rxPdos.front().sm, 2);
  EXPECT_EQ(node->txPdos.front().index, 0x1A00u);
  EXPECT_EQ(node->txPdos.front().sm, 3);

  // The first RxPDO maps Controlword, whose object lives in the CiA402 *module* — so a PDO entry
  // resolves against the merged dictionary, never against the element that encloses it.
  const EsiPdo& rxPdo = node->rxPdos.front();
  EXPECT_TRUE(std::any_of(rxPdo.entries.begin(), rxPdo.entries.end(), [](const EsiPdo::Entry& e) {
    return e.index == 0x6040 && e.bitLen == 16;
  }));

  // Alignment padding is written as index 0 with neither SubIndex nor DataType.
  bool anyPadding = false;
  for (const EsiModule& module : esi.modules) {
    for (const std::vector<EsiPdo>* group : {&module.rxPdos, &module.txPdos}) {
      for (const EsiPdo& pdo : *group) {
        anyPadding =
            anyPadding || std::any_of(pdo.entries.begin(), pdo.entries.end(),
                                      [](const EsiPdo::Entry& e) { return e.isPadding(); });
      }
    }
  }
  EXPECT_TRUE(anyPadding);
}

TEST(EsiSomanetTest, ReadsTheDictionaryLocalUnitTypeOverride) {
  const EsiFile& esi = somanetEsi();

  // The FSoE dictionaries redefine notation index 0xB6 — "rpm" in the ETG.1004 catalogue — as
  // "Bit". A parser ignoring <UnitTypes> would mislabel every entry that uses it.
  const EsiModule* fsoe = findEsiModule(esi, 0x22D20001);
  ASSERT_NE(fsoe, nullptr);
  ASSERT_TRUE(fsoe->profile.has_value());
  ASSERT_TRUE(fsoe->profile->dictionary.has_value());
  const std::vector<UnitType>& local = fsoe->profile->dictionary->unitTypes;
  ASSERT_FALSE(local.empty());

  const auto bit = std::find_if(local.begin(), local.end(),
                                [](const UnitType& u) { return u.notationIndex == 0xB6; });
  ASSERT_NE(bit, local.end());
  EXPECT_EQ(bit->symbol, "Bit");

  EXPECT_EQ(esiUnitSymbol(0x00B60000, local), "Bit");
  EXPECT_EQ(esiUnitSymbol(0x00B60000), "rpm");  // Same value, no local table.
}

TEST(EsiSomanetTest, WarnsOnlyAboutRealIrregularities) {
  const EsiFile& esi = somanetEsi();

  // The document itself parses without complaint; everything questionable is per-entry and
  // surfaces during flattening instead.
  EXPECT_TRUE(esi.warnings.empty())
      << esi.warnings.size() << " warning(s), first: " << esi.warnings.front();

  const EsiDevice* node = findEsiDevice(esi, "SOMANET Node");
  ASSERT_NE(node, nullptr);
  const auto table = buildDeviceEntries(esi, *node);
  ASSERT_TRUE(table.has_value()) << (table ? std::string{} : table.error());

  // A handful of hexBinary values in this file disagree with their declared width. Reporting that
  // is the point — but it must stay a handful; a flood would mean the parser is misreading
  // something systematically rather than the file being slightly wrong.
  EXPECT_LT(table->warnings.size(), 40u)
      << table->warnings.size() << " warning(s), first: " << table->warnings.front();
}

}  // namespace
}  // namespace mm::etg
