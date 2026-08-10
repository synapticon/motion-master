#include "etg/esi_entry.h"

#include <algorithm>
#include <format>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/util.h"

namespace mm::etg {

namespace {

constexpr std::string_view kDescriptionProperty = "description";
constexpr std::string_view kOptionsProperty = "options";

/// ETG.5001 relocates an object with one of two different increments depending on which index
/// area it lives in: the PDO mapping (0x1600-0x17FF) and PDO assignment/TxPDO (0x1A00-0x1BFF)
/// areas move by SlotPdoIncrement, everything else by SlotIndexIncrement.
bool isPdoAreaIndex(uint32_t index) {
  return (index >= 0x1600 && index <= 0x17FF) || (index >= 0x1A00 && index <= 0x1BFF);
}

/// Collects warnings under a cap, so a deliberately corrupt file cannot exhaust memory. The
/// summary line is appended once, at the point the cap is reached, and the counter keeps running
/// so the final message is accurate.
class WarningSink {
 public:
  explicit WarningSink(std::size_t cap) : cap_(cap) {}

  void add(std::string message) {
    ++total_;
    if (messages_.size() < cap_) {
      messages_.push_back(std::move(message));
    }
  }

  std::vector<std::string> take() {
    if (total_ > messages_.size()) {
      messages_.push_back(
          std::format("... {} further warnings suppressed", total_ - messages_.size()));
    }
    return std::move(messages_);
  }

 private:
  std::size_t cap_;
  std::size_t total_ = 0;
  std::vector<std::string> messages_;
};

/// One dictionary to merge, with everything the expansion needs to interpret it. The type table is
/// rebuilt per source because the XSD scopes DataType/Name to a single <Dictionary> — a name like
/// DT1600 means different things in a device and a module dictionary of the same file.
struct Source {
  const EsiDictionary* dictionary = nullptr;
  EsiEntrySource origin;
  bool completeAccessDefault = false;
  uint32_t indexOffset = 0;
  uint32_t pdoIndexOffset = 0;
  std::unordered_map<std::string_view, const EsiDataType*> types;
  std::string label;  ///< For warning messages: "device" or "module 0x04020001 slot 1".
};

const EsiDataType* findType(const Source& source, std::string_view name) {
  const auto it = source.types.find(name);
  return it != source.types.end() ? it->second : nullptr;
}

/// ETG.2000 Table 12 "Data Type Composition": an ARRAY type has exactly two SubItems, the second
/// of which references a helper type named `<name>ARR`. Everything else with SubItems is a RECORD.
bool isArrayType(const EsiDataType& type) {
  return type.subItems.size() == 2 && type.subItems[1].type == type.name + "ARR";
}

ObjectCode classify(const EsiDataType* type) {
  if (type == nullptr || type->subItems.empty()) {
    return ObjectCode::Var;
  }
  return isArrayType(*type) ? ObjectCode::Array : ObjectCode::Record;
}

/// Resolves an ESI type name to a primitive, following <BaseType> when the name is a dictionary
/// alias rather than a primitive. The hop count is capped and visited names tracked so a
/// self-referential or mutually-referential BaseType terminates instead of recursing forever.
std::optional<PrimitiveType> resolveType(const Source& source, std::string_view name) {
  constexpr int kMaxHops = 8;
  std::string_view current = name;
  for (int hop = 0; hop < kMaxHops; ++hop) {
    if (auto primitive = resolvePrimitiveType(current)) {
      return primitive;
    }
    const EsiDataType* type = findType(source, current);
    if (type == nullptr || !type->baseType || *type->baseType == current) {
      return std::nullopt;
    }
    current = *type->baseType;
  }
  return std::nullopt;
}

/// The declared width of a type name, preferring the dictionary's own <BitSize> over the table's
/// (a dictionary may legitimately declare STRING(50) once and reuse it).
uint16_t typeBitSize(const Source& source, std::string_view name,
                     const std::optional<PrimitiveType>& primitive) {
  if (const EsiDataType* type = findType(source, name); type != nullptr && type->bitSize > 0) {
    return static_cast<uint16_t>(type->bitSize);
  }
  return primitive ? primitive->bitSize : uint16_t{0};
}

std::string_view propertyValue(const std::vector<Property>& properties, std::string_view name) {
  const auto it = std::find_if(properties.begin(), properties.end(),
                               [name](const Property& p) { return p.name == name; });
  return it != properties.end() ? std::string_view{it->value} : std::string_view{};
}

/// Merges the DataType's own <EnumInfo> enumerators with an "options" property, if either exists.
/// EnumInfo wins on a duplicate label, being the schema's own mechanism.
std::vector<EsiEnumOption> readOptions(const EsiDataType* type,
                                       const std::vector<Property>& properties, uint32_t lcId,
                                       std::string_view where, WarningSink& warnings) {
  std::vector<EsiEnumOption> options;
  if (type != nullptr) {
    for (const EsiDataType::EnumInfo& e : type->enumInfo) {
      options.push_back(EsiEnumOption{std::string(esiText(e.texts, lcId)), e.value});
    }
  }

  const std::string_view json = propertyValue(properties, kOptionsProperty);
  if (json.empty()) {
    return options;
  }
  const nlohmann::json parsed = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    warnings.add(std::format("{}: 'options' property is not a JSON object; ignored", where));
    return options;
  }
  for (const auto& [label, value] : parsed.items()) {
    if (!value.is_number_integer()) {
      continue;
    }
    const bool known = std::any_of(options.begin(), options.end(),
                                   [&label](const EsiEnumOption& o) { return o.label == label; });
    if (!known) {
      options.push_back(EsiEnumOption{label, value.get<int64_t>()});
    }
  }
  return options;
}

/// ETG.2000 Figure 37, "Content of Dictionary/Object", transcribed:
///
///     Start: Object is...
///       |- Enum / Base Type ------------------------------+
///       |                                                 v
///       |                     Corresponding Object/Flags/FLAG exists?
///       |                       Yes -> Used value:         Object/Flags/FLAG
///       |                       No  -> Used default value: Object/Flags/FLAG
///       |                                                 ^
///       \- Record / Array -> Corresponding DataType/SubItem/Flags exists?
///                              No ------------------------+
///                              Yes -> Corresponding DataType/SubItem/Flags/FLAG exists?
///                                       Yes -> Used value:         DataType/SubItem/Flags/FLAG
///                                       No  -> Used default value: DataType/SubItem/Flags/FLAG
///
/// The asymmetry that makes this worth transcribing rather than paraphrasing: **once the
/// SubItem branch is entered it never returns to the object.** A flag missing there resolves to
/// that flag's own default — the spec's "Used default value" — not to whatever the object said.
/// Only the outer "Flags exists?" question routes back up. Reading it as per-flag fallback makes
/// a member inherit Category="m" from its object and tells a config tool the entry is mandatory
/// when the ESI says it is not.
///
/// The spec's NOTE fixes the scope exactly, and repeats it identically at all fourteen places the
/// figure is referenced: *"FLAG is a placeholder for Flags: Access, Category, PdoMapping,
/// SafetyMapping, Backup, and Setting."* Six. `Attribute`, `SdoAccess` and `Transition` are
/// **outside** the rule — `SdoAccess` and `Transition` structurally so, since EtherCATBase.xsd
/// gives them to ObjectType/Flags alone.
///
/// Templated over the member pointers because Object::Flags and SubItem::Flags are separate types
/// (the SubItem one genuinely lacks SdoAccess and Transition) with otherwise parallel members.
template <typename T, typename ObjMember, typename SubMember>
T adoptFlag(const std::optional<EsiObject::Flags>& objectFlags,
            const std::optional<EsiDataType::SubItem::Flags>& subFlags, ObjMember objMember,
            SubMember subMember, T specDefault) {
  if (subFlags) {
    return (subFlags.value().*subMember).value_or(specDefault);
  }
  if (objectFlags) {
    return (objectFlags.value().*objMember).value_or(specDefault);
  }
  return specDefault;
}

/// Builds the ETG.1000.6 ObjAccess bitfield from the resolved flags. A restriction narrows which
/// AL states the access is valid in; absent means all three.
uint16_t makeObjAccess(const Access& access, PdoMapping pdoMapping, bool backup, bool setting) {
  const auto stateBits = [](StateRestriction r) -> uint16_t {
    switch (r) {
      case StateRestriction::None:
        return 0b111;
      case StateRestriction::PreOp:
        return 0b001;
      case StateRestriction::PreOpSafeOp:
        return 0b011;
      case StateRestriction::PreOpOp:
        return 0b101;
      case StateRestriction::SafeOp:
        return 0b010;
      case StateRestriction::SafeOpOp:
        return 0b110;
      case StateRestriction::Op:
        return 0b100;
    }
    return 0b111;
  };

  uint16_t bits = 0;
  if (access.mode == AccessMode::Ro || access.mode == AccessMode::Rw) {
    bits |= stateBits(access.readRestrictions);
  }
  if (access.mode == AccessMode::Wo || access.mode == AccessMode::Rw) {
    bits |= static_cast<uint16_t>(stateBits(access.writeRestrictions) << 3);
  }
  if (pdoMapping == PdoMapping::Rx || pdoMapping == PdoMapping::TxRx) {
    bits |= 1u << 6;
  }
  if (pdoMapping == PdoMapping::Tx || pdoMapping == PdoMapping::TxRx) {
    bits |= 1u << 7;
  }
  if (backup) {
    bits |= 1u << 8;
  }
  if (setting) {
    bits |= 1u << 9;
  }
  return bits;
}

/// Encodes an obsolete-branch <MinValue>/<DefaultValue> scalar into the little-endian byte form
/// the rest of the pipeline uses, so a consumer never has to handle two representations.
std::vector<uint8_t> encodeScalar(int64_t value, uint16_t bitSize) {
  const std::size_t bytes = bitSize > 0 ? static_cast<std::size_t>((bitSize + 7) / 8) : 8u;
  auto full = mm::core::toBytes<int64_t>(value);
  return std::vector<uint8_t>(full.begin(), full.begin() + std::min(bytes, full.size()));
}

/// Normalises a raw hexBinary value to the entry's declared width. Sign-extends a signed type so a
/// negative bound stays negative, zero-fills otherwise. Always warns when it has to act: a
/// mismatch means the ESI disagrees with itself and the reader should know.
std::optional<std::vector<uint8_t>> normaliseData(std::vector<uint8_t> data, uint16_t bitSize,
                                                  bool isSigned, bool pad, std::string_view what,
                                                  std::string_view where, WarningSink& warnings) {
  if (data.empty()) {
    return std::nullopt;
  }
  const std::size_t expected = bitSize > 0 ? static_cast<std::size_t>((bitSize + 7) / 8) : 0u;
  if (expected == 0 || data.size() == expected) {
    return data;
  }
  warnings.add(std::format("{}: {} is {} byte(s) but the declared width is {} bit(s){}", where,
                           what, data.size(), bitSize, pad ? "; padded" : "; left as written"));
  if (!pad) {
    return data;
  }
  if (data.size() > expected) {
    data.resize(expected);
    return data;
  }
  const bool negative = isSigned && !data.empty() && (data.back() & 0x80) != 0;
  data.resize(expected, negative ? uint8_t{0xFF} : uint8_t{0x00});
  return data;
}

/// Everything one subindex needs, gathered before the entry is built. Keeps the three expansion
/// paths (VAR / ARRAY / RECORD) from each repeating the shared tail.
struct EntryInput {
  uint8_t subindex = 0;
  std::string entryName;
  std::string_view typeName;
  uint16_t bitSize = 0;
  uint16_t bitOffset = 0;
  const EsiObject::Info* info = nullptr;                   ///< Value metadata, may be null.
  const EsiDataType::SubItem* flagSource = nullptr;        ///< Flags + per-entry properties.
  const std::vector<Property>* entryProperties = nullptr;  ///< Overrides flagSource's, if set.
};

/// Every source that claimed one address, in the order they were merged.
///
/// Reported only after the whole merge, never during it: a chain of three mutually exclusive
/// module variants collides pairwise against whichever one is currently incumbent, so a
/// report written as it happens names a different "winner" at each step and only the last is
/// the truth. Recording the claimants and resolving the winner at the end says what actually
/// ended up in the table.
struct Claim {
  std::vector<std::string> sources;
  bool moduleOverride = false;
};

/// One "these sources claimed the same addresses, this one won" group, so a variant that
/// collides on hundreds of subindices is reported once.
struct CollisionGroup {
  std::size_t count = 0;
  uint16_t firstIndex = 0;
  uint8_t firstSubindex = 0;
  bool moduleOverride = false;
};

std::string sourceLabel(const EsiEntrySource& source) {
  return source.kind == EsiEntrySource::Kind::Device
             ? std::string("the device dictionary")
             : std::format("module 0x{:08X} slot {}", source.moduleIdent, source.slot);
}

}  // namespace

std::expected<EsiEntryTable, std::string> buildDeviceEntries(const EsiFile& file,
                                                             const EsiDevice& device,
                                                             const EsiEntryOptions& options) {
  WarningSink warnings(options.maxWarnings);
  std::vector<Source> sources;

  // ---- Step 1: collect the dictionaries to merge, device first, then slots in order. ----------
  const bool deviceCompleteAccess = device.coe && device.coe->completeAccess;
  for (const EsiProfile& profile : device.profiles) {
    if (!profile.dictionary) {
      continue;
    }
    Source source;
    source.dictionary = &*profile.dictionary;
    source.origin.kind = EsiEntrySource::Kind::Device;
    source.completeAccessDefault = deviceCompleteAccess;
    source.label = "device";
    sources.push_back(std::move(source));
  }

  for (std::size_t slotIndex = 0; slotIndex < device.slots.slots.size(); ++slotIndex) {
    const EsiSlots::Slot& slot = device.slots.slots[slotIndex];
    for (const uint32_t ident : slot.moduleIdents) {
      if (!options.moduleIdents.empty() &&
          std::find(options.moduleIdents.begin(), options.moduleIdents.end(), ident) ==
              options.moduleIdents.end()) {
        continue;
      }
      const EsiModule* module = findEsiModule(file, ident);
      if (module == nullptr) {
        warnings.add(
            std::format("slot {}: no <Module> with ModuleIdent 0x{:08X}", slotIndex, ident));
        continue;
      }
      if (!module->profile || !module->profile->dictionary) {
        warnings.add(std::format("slot {}: module 0x{:08X} has no dictionary", slotIndex, ident));
        continue;
      }

      // A per-slot increment overrides the <Slots>-level one; both are multiplied by the slot
      // ordinal, so slot 0 is always a no-op regardless of the declared step.
      const uint32_t indexIncrement =
          slot.slotIndexIncrement.value_or(device.slots.slotIndexIncrement);
      const uint32_t pdoIncrement = slot.slotPdoIncrement.value_or(device.slots.slotPdoIncrement);
      const auto ordinal = static_cast<uint32_t>(slotIndex);

      Source source;
      source.dictionary = &*module->profile->dictionary;
      source.origin.kind = EsiEntrySource::Kind::Module;
      source.origin.moduleIdent = ident;
      source.origin.slot = static_cast<int32_t>(slotIndex);
      source.completeAccessDefault =
          module->coe ? module->coe->completeAccess : deviceCompleteAccess;
      source.indexOffset = options.applySlotOffsets ? ordinal * indexIncrement : 0;
      source.pdoIndexOffset = options.applySlotOffsets ? ordinal * pdoIncrement : 0;
      source.label = std::format("module 0x{:08X} slot {}", ident, slotIndex);
      sources.push_back(std::move(source));
    }
  }

  if (sources.empty()) {
    return std::unexpected(std::format(
        "device '{}' has no object dictionary: neither the device nor any slot-attached module "
        "declares one",
        device.type));
  }

  // ---- Step 2: per-source type table (DataType/Name is scoped to one <Dictionary>). -----------
  for (Source& source : sources) {
    for (const EsiDataType& type : source.dictionary->dataTypes) {
      const auto [it, inserted] = source.types.emplace(type.name, &type);
      if (!inserted) {
        warnings.add(std::format("{}: duplicate <DataType> name '{}'; the first is kept",
                                 source.label, type.name));
      }
    }
  }

  std::vector<EsiEntry> entries;

  /// Where an already-emitted entry sits, plus whether the object that produced it invited a
  /// module to replace it. The flag belongs to the *incumbent* — @c OverwrittenByModule is set by
  /// the device on the object it is willing to give up — so it has to be remembered here rather
  /// than read off the incoming object.
  struct Placed {
    std::size_t position = 0;
    bool overwritable = false;
  };
  std::unordered_map<uint32_t, Placed> byKey;
  std::unordered_map<uint32_t, Claim> claims;
  bool collided = false;

  // ---- Steps 3-9: expand every object of every source into per-subindex entries. --------------
  for (const Source& source : sources) {
    for (const EsiObject& object : source.dictionary->objects) {
      // Step 3: slot relocation. ETG.2000 gates this on Index/@DependOnSlot; honouring that by
      // default keeps us from inventing indices a device does not answer to.
      const bool relocate =
          options.applySlotOffsets && (!options.requireDependOnSlot || object.dependOnSlot);
      const uint32_t offset =
          relocate ? (isPdoAreaIndex(object.index) ? source.pdoIndexOffset : source.indexOffset)
                   : 0;
      uint32_t effectiveIndex = object.index + offset;
      uint32_t appliedOffset = offset;
      if (effectiveIndex > 0xFFFF) {
        warnings.add(
            std::format("{}: 0x{:04X} + slot offset 0x{:X} overflows the 16-bit index "
                        "space; the offset is dropped",
                        source.label, object.index, offset));
        effectiveIndex = object.index;
        appliedOffset = 0;
      }

      const std::string where = std::format("0x{:04X} [{}]", effectiveIndex, source.label);
      const EsiDataType* objectType = findType(source, object.type);
      if (objectType == nullptr) {
        warnings.add(
            std::format("{}: <Type> '{}' is not declared in this dictionary; emitted as a "
                        "single untyped entry",
                        where, object.type));
      }

      // Step 4: classify BEFORE pairing anything. An ARRAY's DataType has two SubItems while its
      // Info has N+1, so pairing them positionally without classifying first corrupts every array.
      const ObjectCode objectCode = classify(objectType);
      const std::string objectName(esiText(object.names, options.lcId));

      // Step 7 (object level): a VAR's annotation lives on the Object, a composite's on the
      // DataType. It is attached to **subindex 0 only** — that row *is* the object — rather than
      // copied onto every subindex. Repeating it was measured at 83% of a device's JSON, since a
      // description is often kilobytes of HTML and a RECORD can have twenty members.
      const std::vector<Property>& objectProperties =
          (objectCode == ObjectCode::Var || objectType == nullptr) ? object.properties
                                                                   : objectType->properties;

      const uint8_t numberOfEntries =
          object.info.subItems.empty()
              ? uint8_t{0}
              : static_cast<uint8_t>(object.info.subItems.front().info.defaultData.empty()
                                         ? object.info.subItems.size() - 1
                                         : object.info.subItems.front().info.defaultData.front());

      // Step 5: build the per-subindex inputs for whichever shape this object is.
      std::vector<EntryInput> inputs;

      if (objectCode == ObjectCode::Var) {
        inputs.push_back(EntryInput{
            .subindex = 0,
            .entryName = objectName,
            .typeName = object.type,
            .bitSize = static_cast<uint16_t>(object.bitSize),
            .bitOffset = 0,
            .info = &object.info,
            .flagSource = nullptr,
            .entryProperties = &object.properties,
        });
      } else if (objectCode == ObjectCode::Array) {
        // ETG.2000: subindex 0 of an ARRAY is always USINT, whatever the DataType says; the
        // element type comes one hop through the "...ARR" helper type's BaseType.
        const EsiDataType::SubItem& countSub = objectType->subItems[0];
        const EsiDataType::SubItem& elementSub = objectType->subItems[1];
        const EsiDataType* arrayInfoType = findType(source, elementSub.type);
        const std::string_view elementTypeName =
            (arrayInfoType != nullptr && arrayInfoType->baseType)
                ? std::string_view(*arrayInfoType->baseType)
                : std::string_view(elementSub.type);
        const auto elementPrimitive = resolveType(source, elementTypeName);
        const uint16_t elementBits = typeBitSize(source, elementTypeName, elementPrimitive);

        const std::size_t count =
            object.info.subItems.empty()
                ? (arrayInfoType != nullptr && !arrayInfoType->arrayInfo.empty()
                       ? static_cast<std::size_t>(arrayInfoType->arrayInfo.front().elements) + 1
                       : 0)
                : object.info.subItems.size();

        for (std::size_t i = 0; i < count; ++i) {
          const EsiObject::Info::SubItem* infoSub =
              i < object.info.subItems.size() ? &object.info.subItems[i] : nullptr;
          const bool isCount = i == 0;
          inputs.push_back(EntryInput{
              // An ARRAY's subindex is positional: the element SubItem describes all of 1..N and
              // carries no SubIdx of its own.
              .subindex = static_cast<uint8_t>(i),
              .entryName = infoSub != nullptr
                               ? infoSub->name
                               : (isCount ? countSub.name : std::format("SubIndex {:03}", i)),
              .typeName = isCount ? std::string_view("USINT") : elementTypeName,
              .bitSize = isCount ? uint16_t{8} : elementBits,
              .bitOffset = isCount
                               ? static_cast<uint16_t>(countSub.bitOffs)
                               : static_cast<uint16_t>(elementSub.bitOffs +
                                                       static_cast<int32_t>(i - 1) * elementBits),
              .info = infoSub != nullptr ? &infoSub->info : nullptr,
              .flagSource = isCount ? &countSub : &elementSub,
              .entryProperties = nullptr,
          });
        }
      } else {
        // RECORD. Pair Info/SubItem to DataType/SubItem by name — the XSD puts an xs:key on
        // DataType/SubItem/Name, so names are unique within a type and the match is well-founded.
        const std::size_t count =
            std::max(object.info.subItems.size(), objectType->subItems.size());
        if (!object.info.subItems.empty() &&
            object.info.subItems.size() != objectType->subItems.size()) {
          warnings.add(
              std::format("{}: <Info> declares {} sub-item(s) but the DataType declares "
                          "{}; expanding the union",
                          where, object.info.subItems.size(), objectType->subItems.size()));
        }

        for (std::size_t i = 0; i < count; ++i) {
          const EsiObject::Info::SubItem* infoSub =
              i < object.info.subItems.size() ? &object.info.subItems[i] : nullptr;

          const EsiDataType::SubItem* typeSub = nullptr;
          if (infoSub != nullptr) {
            const auto match = std::find_if(
                objectType->subItems.begin(), objectType->subItems.end(),
                [&infoSub](const EsiDataType::SubItem& s) { return s.name == infoSub->name; });
            if (match != objectType->subItems.end()) {
              typeSub = &*match;
            } else if (i < objectType->subItems.size()) {
              typeSub = &objectType->subItems[i];
              warnings.add(std::format("{}: no DataType SubItem named '{}'; matched by position",
                                       where, infoSub->name));
            }
          } else if (i < objectType->subItems.size()) {
            typeSub = &objectType->subItems[i];
          }

          if (typeSub == nullptr) {
            warnings.add(std::format("{}: sub-item {} has no DataType SubItem; skipped", where, i));
            continue;
          }
          if (!typeSub->subIdx) {
            warnings.add(std::format("{}: DataType SubItem '{}' has no <SubIdx>; using position {}",
                                     where, typeSub->name, i));
          }

          inputs.push_back(EntryInput{
              .subindex = typeSub->subIdx.value_or(static_cast<uint8_t>(i)),
              .entryName = typeSub->name,
              .typeName = typeSub->type,
              .bitSize = static_cast<uint16_t>(typeSub->bitSize),
              .bitOffset = static_cast<uint16_t>(typeSub->bitOffs),
              .info = infoSub != nullptr ? &infoSub->info : nullptr,
              .flagSource = typeSub,
              .entryProperties = nullptr,
          });
        }
      }

      // ---- Shared tail: turn each input into an entry. ----------------------------------------
      for (const EntryInput& input : inputs) {
        EsiEntry entry;
        entry.index = static_cast<uint16_t>(effectiveIndex);
        entry.rawIndex = static_cast<uint16_t>(object.index);
        entry.subindex = input.subindex;
        entry.objectName = objectName;
        entry.entryName = input.entryName;
        entry.objectCode = objectCode;
        entry.numberOfEntries = objectCode == ObjectCode::Var ? uint8_t{0} : numberOfEntries;

        const auto primitive = resolveType(source, input.typeName);
        entry.dataTypeName = std::string(input.typeName);
        entry.dataType = primitive ? primitive->code : uint16_t{0};
        entry.isSigned = primitive && primitive->isSigned;
        entry.bitSize =
            input.bitSize > 0 ? input.bitSize : typeBitSize(source, input.typeName, primitive);
        entry.bitOffset = input.bitOffset;
        // After bitSize, because the width is half of what decides this (see ValueKind).
        entry.valueKind = resolveValueKind(entry.dataType, entry.bitSize);
        if (!primitive) {
          warnings.add(
              std::format("0x{:04X}:{:02X} [{}]: type '{}' is not a known primitive; the "
                          "ETG.1020 code is left unset",
                          entry.index, entry.subindex, source.label, input.typeName));
        }

        const std::string valueWhere =
            std::format("0x{:04X}:{:02X} [{}]", entry.index, entry.subindex, source.label);

        // Values. The obsolete Min/Max/DefaultValue branch is encoded into the same little-endian
        // byte form as the hexBinary branch, so a consumer never sees two representations.
        if (input.info != nullptr) {
          const EsiObject::Info& info = *input.info;
          auto pick = [&](const std::vector<uint8_t>& data, const std::optional<int64_t>& scalar,
                          std::string_view what) -> std::optional<std::vector<uint8_t>> {
            if (!data.empty()) {
              return normaliseData(data, entry.bitSize, entry.isSigned, options.padShortValues,
                                   what, valueWhere, warnings);
            }
            if (scalar) {
              return encodeScalar(*scalar, entry.bitSize);
            }
            return std::nullopt;
          };
          entry.defaultData = pick(info.defaultData, info.defaultValue, "<DefaultData>");
          entry.minData = pick(info.minData, info.minValue, "<MinData>");
          entry.maxData = pick(info.maxData, info.maxValue, "<MaxData>");

          if (!entry.defaultData && info.defaultString) {
            entry.defaultData =
                std::vector<uint8_t>(info.defaultString->begin(), info.defaultString->end());
          }
          entry.unit = info.unit;
          entry.displayName = info.displayName.value_or(input.entryName);
        } else if (input.flagSource != nullptr) {
          // No Info for this subindex: the DataType SubItem may still carry a default.
          if (!input.flagSource->defaultData.empty()) {
            entry.defaultData = input.flagSource->defaultData;
          } else if (input.flagSource->defaultValue) {
            entry.defaultData = encodeScalar(*input.flagSource->defaultValue, entry.bitSize);
          }
          if (input.flagSource->minValue) {
            entry.minData = encodeScalar(*input.flagSource->minValue, entry.bitSize);
          }
          if (input.flagSource->maxValue) {
            entry.maxData = encodeScalar(*input.flagSource->maxValue, entry.bitSize);
          }
          entry.displayName = input.entryName;
        } else {
          entry.displayName = input.entryName;
        }

        if (entry.unit) {
          entry.unitSymbol = esiUnitSymbol(*entry.unit, source.dictionary->unitTypes);
        }

        // Step 6: flag adoption, ETG.2000 Figure 37.
        const std::optional<EsiDataType::SubItem::Flags>& subFlags =
            input.flagSource != nullptr ? input.flagSource->flags
                                        : std::optional<EsiDataType::SubItem::Flags>{};
        entry.access = adoptFlag(object.flags, subFlags, &EsiObject::Flags::access,
                                 &EsiDataType::SubItem::Flags::access, Access{});
        entry.category = adoptFlag(object.flags, subFlags, &EsiObject::Flags::category,
                                   &EsiDataType::SubItem::Flags::category, Category::Optional);
        entry.pdoMapping = adoptFlag(object.flags, subFlags, &EsiObject::Flags::pdoMapping,
                                     &EsiDataType::SubItem::Flags::pdoMapping, PdoMapping::None);
        entry.safetyMapping =
            adoptFlag(object.flags, subFlags, &EsiObject::Flags::safetyMapping,
                      &EsiDataType::SubItem::Flags::safetyMapping, SafetyMapping::None);
        entry.backup = adoptFlag(object.flags, subFlags, &EsiObject::Flags::backup,
                                 &EsiDataType::SubItem::Flags::backup, 0) != 0;
        entry.setting = adoptFlag(object.flags, subFlags, &EsiObject::Flags::setting,
                                  &EsiDataType::SubItem::Flags::setting, 0) != 0;
        entry.attribute = subFlags ? subFlags->attribute
                                   : (object.flags ? object.flags->attribute : std::nullopt);

        // SdoAccess is a deliberate carve-out from the loop above: SubItemType/Flags has no such
        // element, so every subindex inherits the object's, defaulting to the dictionary's
        // Mailbox/CoE/@CompleteAccess.
        entry.sdoAccess = (object.flags && object.flags->sdoAccess)
                              ? *object.flags->sdoAccess
                              : (source.completeAccessDefault ? SdoAccess::CompleteAccess
                                                              : SdoAccess::SubIndexAccess);

        entry.objAccess =
            makeObjAccess(entry.access, entry.pdoMapping, entry.backup, entry.setting);

        // Step 7: properties, carried verbatim; description/options decoded as a convenience.
        //
        // Subindex 0 *is* the object, so it carries the object's annotation. Every other row
        // carries only what that entry itself declares — a RECORD member's own description, or
        // nothing for an ARRAY element, which the ESI describes once for the whole array rather
        // than per element. A consumer wanting an object's text for subindex 5 reads subindex 0
        // of the same index.
        if (entry.subindex == 0) {
          entry.properties = objectProperties;
        }
        const std::vector<Property>* own = input.entryProperties != nullptr ? input.entryProperties
                                           : input.flagSource != nullptr
                                               ? &input.flagSource->properties
                                               : nullptr;
        if (own != nullptr && own != &objectProperties) {
          // Schema-legal though absent from every real file seen: a composite's SubItem 0 may
          // carry its own properties alongside the object's. Append rather than choose.
          entry.properties.insert(entry.properties.end(), own->begin(), own->end());
        }
        // Surfaced verbatim on `description`, so keeping the identical string in `properties` too
        // would send the same (often multi-kilobyte) HTML twice per row. Only the *one* that was
        // surfaced is dropped: subindex 0 of a composite concatenates the object's properties with
        // its SubItem's, and if both declared a description, erasing every match would discard the
        // second one entirely — the opposite of what this vector is for.
        const auto described =
            std::find_if(entry.properties.begin(), entry.properties.end(),
                         [](const Property& p) { return p.name == kDescriptionProperty; });
        if (described != entry.properties.end()) {
          entry.description = described->value;
          entry.properties.erase(described);
        }

        const EsiDataType* entryType = findType(source, input.typeName);
        entry.options =
            readOptions(entryType, entry.properties, options.lcId, valueWhere, warnings);

        entry.source = source.origin;
        entry.source.indexOffset = appliedOffset;

        // Step 9: merge. An object the device marked OverwrittenByModule always yields to a
        // module — that is ETG.2000's own, opt-in override mechanism, so it outranks the policy.
        const uint32_t key = entry.key();
        const auto existing = byKey.find(key);
        if (existing == byKey.end()) {
          byKey.emplace(key, Placed{entries.size(), object.overwrittenByModule});
          entries.push_back(std::move(entry));
          continue;
        }

        collided = true;
        EsiEntry& incumbent = entries[existing->second.position];
        const bool moduleOverride = incumbent.source.kind == EsiEntrySource::Kind::Device &&
                                    existing->second.overwritable &&
                                    entry.source.kind == EsiEntrySource::Kind::Module;
        const bool replace =
            moduleOverride || options.collisionPolicy == EsiCollisionPolicy::LastWins;

        // Recorded, not reported: see Claim. Which source actually won is on the entry itself in
        // EsiEntry::source either way.
        Claim& claim = claims[key];
        if (claim.sources.empty()) {
          claim.sources.push_back(sourceLabel(incumbent.source));
        }
        claim.sources.push_back(source.label);
        claim.moduleOverride = claim.moduleOverride || moduleOverride;
        if (replace) {
          // Overwrite in place so the merged table keeps a stable, first-seen ordering.
          incumbent = std::move(entry);
          existing->second.overwritable = object.overwrittenByModule;
        }
      }
    }
  }

  // Now that the merge has settled, group the collisions by "who claimed it" plus "who ended up
  // owning it" and report each group once, naming the real winner.
  std::map<std::pair<std::string, std::string>, CollisionGroup> grouped;
  for (const auto& [key, claim] : claims) {
    const EsiEntry& winner = entries[byKey.at(key).position];
    std::string claimants;
    for (const std::string& s : claim.sources) {
      claimants += claimants.empty() ? s : ", " + s;
    }
    CollisionGroup& group = grouped[{claimants, sourceLabel(winner.source)}];
    if (group.count == 0 ||
        winner.key() < (static_cast<uint32_t>(group.firstIndex) << 8 | group.firstSubindex)) {
      group.firstIndex = winner.index;
      group.firstSubindex = winner.subindex;
    }
    ++group.count;
    group.moduleOverride = group.moduleOverride || claim.moduleOverride;
  }
  for (const auto& [who, group] : grouped) {
    warnings.add(std::format(
        "{} each declare the same {} entr{} (first 0x{:04X}:{:02X}); {} wins{}", who.first,
        group.count, group.count == 1 ? "y" : "ies", group.firstIndex, group.firstSubindex,
        who.second,
        group.moduleOverride ? " (the device marked the object OverwrittenByModule)" : ""));
  }

  if (collided && options.collisionPolicy == EsiCollisionPolicy::Error) {
    return std::unexpected(
        std::format("device '{}': its dictionaries declare the same (index, subindex) more than "
                    "once and the collision policy is Error",
                    device.type));
  }

  if (options.sortByAddress) {
    std::sort(entries.begin(), entries.end(), [](const EsiEntry& a, const EsiEntry& b) {
      return a.index != b.index ? a.index < b.index : a.subindex < b.subindex;
    });
  }

  return EsiEntryTable{.entries = std::move(entries), .warnings = warnings.take()};
}

void to_json(nlohmann::json& j, const EsiEnumOption& v) {
  j = nlohmann::json{{"label", v.label}, {"value", v.value}};
}

void to_json(nlohmann::json& j, const EsiEntrySource& v) {
  j = nlohmann::json{{"kind", v.kind == EsiEntrySource::Kind::Device ? "device" : "module"}};
  if (v.kind == EsiEntrySource::Kind::Module) {
    j["moduleIdent"] = v.moduleIdent;
    j["slot"] = v.slot;
  }
  if (v.indexOffset != 0) {
    j["indexOffset"] = v.indexOffset;
  }
}

void to_json(nlohmann::json& j, const EsiEntry& v) {
  j = nlohmann::json{
      {"index", v.index},
      {"subindex", v.subindex},
      {"objectName", v.objectName},
      {"entryName", v.entryName},
      {"displayName", v.displayName},
      {"objectCode", objectCodeName(v.objectCode)},
      {"objectCodeValue", static_cast<uint16_t>(v.objectCode)},
      {"dataTypeName", v.dataTypeName},
      {"dataType", v.dataType},
      {"cxxType", cxxTypeName(v.valueKind)},
      {"isSigned", v.isSigned},
      {"bitSize", v.bitSize},
      {"bitOffset", v.bitOffset},
      {"access", v.access},
      {"category", categoryName(v.category)},
      {"pdoMapping", pdoMappingName(v.pdoMapping)},
      {"sdoAccess", sdoAccessName(v.sdoAccess)},
      {"backup", v.backup},
      {"setting", v.setting},
      {"objAccess", v.objAccess},
      {"source", v.source},
  };
  if (v.rawIndex != v.index) {
    j["rawIndex"] = v.rawIndex;
  }
  if (v.numberOfEntries != 0) {
    j["numberOfEntries"] = v.numberOfEntries;
  }
  if (v.safetyMapping != SafetyMapping::None) {
    j["safetyMapping"] = safetyMappingName(v.safetyMapping);
  }
  if (v.attribute) {
    j["attribute"] = *v.attribute;
  }
  // Raw bytes go out as uppercase hexBinary, the same spelling the ESI itself uses, so a value
  // round-trips visually against the source file.
  if (v.defaultData) {
    j["defaultData"] = mm::core::toHex(*v.defaultData);
  }
  if (v.minData) {
    j["minData"] = mm::core::toHex(*v.minData);
  }
  if (v.maxData) {
    j["maxData"] = mm::core::toHex(*v.maxData);
  }
  if (v.unit) {
    j["unit"] = *v.unit;
    j["unitSymbol"] = v.unitSymbol;
  }
  if (!v.description.empty()) {
    j["description"] = v.description;
  }
  if (!v.options.empty()) {
    j["options"] = v.options;
  }
  if (!v.properties.empty()) {
    j["properties"] = v.properties;
  }
}

void to_json(nlohmann::json& j, const EsiEntryTable& v) {
  j = nlohmann::json{{"entries", v.entries}};
  if (!v.warnings.empty()) {
    j["warnings"] = v.warnings;
  }
}

}  // namespace mm::etg
