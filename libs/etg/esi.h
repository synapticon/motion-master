#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "etg/esi_unit.h"

namespace mm::etg {

/// @brief EtherCAT Slave Information (ESI) — the vendor's XML description of a device family.
///
/// Every EtherCAT device ships with an ESI file: the machine-readable device description a
/// configuration tool reads to know what the device is, how its process data is laid out, and —
/// the part this library exists for — what its CoE object dictionary contains. The CoE
/// SDO-Information service can enumerate that dictionary off a live device, but returns only
/// index/subindex/type/access; everything a human needs (descriptions, enum labels, engineering
/// units, min/max bounds, display names) exists **only** in the ESI. @c parseEsi decodes the file
/// into the structures below; @c buildDeviceEntries (etg/esi_entry.h) flattens one device into the
/// per-(index, subindex) table that is this library's deliverable.
///
/// Reference: **ETG.2000** (EtherCAT Slave Information Specification) is the prose authority, and
/// the XML Schema 1.19.1 set (@c EtherCATBase.xsd, @c EtherCATInfo.xsd) is the exact contract —
/// individual structures below cite their schema type. Two conventions run through the whole
/// format and are handled once, here:
///
///   - **@c HexDecValue** — a number written either as signed decimal (@c "402") or with a @c "#x"
///     hex prefix (@c "#x1A00"). Hex-digit case is *not* consistent even within one file, so
///     values are compared as parsed integers, never as strings. Decoded with
///     @c mm::core::parseHexOrDec, which already accepts the @c #x prefix.
///   - **@c xs:hexBinary** — a raw byte string (@c "92010200") that is **little-endian: the first
///     byte is the least significant**. So @c "92010200" is 0x00020192 and @c "FFFFFF7F" is
///     INT32_MAX. Carried as raw bytes throughout; see @c EsiObject::Info.
///
/// The parser is a pure, hardware-independent transform over text — it is unit-tested against
/// both hand-written fragments and a real vendor ESI, with no fieldbus present.

// ---------------------------------------------------------------------------------------------
// Shared vocabulary.
//
// These types are used by more than one parent element, so they live at namespace scope rather
// than nested: `Property` appears under Object, DataType and DataType/SubItem; `Text` appears
// under a dozen elements. Nesting them under any one parent would misstate ownership.
// ---------------------------------------------------------------------------------------------

/// @brief A localised text node (ETG.2000 @c NameType).
///
/// Every human-readable string in an ESI is a @c NameType: element text plus an optional @c LcId
/// Windows locale identifier. The schema permits one element per locale, so this is always stored
/// as a vector and read through @c esiText.
struct Text {
  std::string value;
  uint32_t lcId = 0;  ///< 0 when the @c LcId attribute is absent (treated as the default locale).
};

/// @brief The default @c LcId — 1033, en-US.
inline constexpr uint32_t kDefaultLcId = 1033;

/// @brief Picks the @p lcId text from @p texts, else the unlocalised/default one, else the first.
///
/// Returns an empty view when @p texts is empty. The result borrows from @p texts.
std::string_view esiText(const std::vector<Text>& texts, uint32_t lcId = kDefaultLcId);

/// @brief A name/value annotation (ETG.2000 @c PropertyType).
///
/// The format's generic extension point, and how Synapticon carries everything the schema has no
/// field for. Two names are conventional — @c "description" (XML-escaped HTML) and @c "options"
/// (a JSON object mapping an enum label to an integer) — but the mechanism is open, so properties
/// are always carried verbatim and any decoding of a *particular* name is a convenience layered
/// on top (see @c EsiEntry::description / @c EsiEntry::options).
///
/// @warning Appears wrapped in @c <Properties> under @c Object and @c DataType, but **bare**
/// under @c DataType/SubItem. That asymmetry is in the XSD (@c EtherCATBase.xsd:172 vs :57/:403),
/// not a vendor bug, and the parser handles both placements.
struct Property {
  std::string name;
  std::string value;
};

/// @brief SDO access mode — the text of a @c <Access> element. Spec default @c Ro.
enum class AccessMode : uint8_t { Ro, Rw, Wo };

/// @brief AL-state restriction on a read or a write (@c Access/\@ReadRestrictions,
///        @c Access/\@WriteRestrictions).
///
/// @c None means the attribute was absent: no restriction beyond the mailbox being up. The XSD
/// enumerates both the modern @c "PreOP" and the legacy @c "PreOp" spelling, and ETG.2000 requires
/// a tool to treat them identically, so parsing is case-insensitive.
enum class StateRestriction : uint8_t { None, PreOp, PreOpSafeOp, PreOpOp, SafeOp, SafeOpOp, Op };

/// @brief Whether an entry must be implemented (@c Flags/Category). Spec default @c Optional.
enum class Category : uint8_t { Mandatory, Optional, Conditional };

/// @brief PDO mappability (@c Flags/PdoMapping). Spec default @c None — not mappable.
///
/// The XSD accepts @c "T R TR RT t r tr rt"; neither case nor order is significant, so all eight
/// spellings fold onto these four. @c Tx is @c "t" — slave to master, a TxPDO.
enum class PdoMapping : uint8_t { None, Tx, Rx, TxRx };

/// @brief Safety mappability (@c Flags/SafetyMapping, ETG.5120). Spec default @c None.
enum class SafetyMapping : uint8_t { None, SafeIn, SafeOut, SafeInOut, SafeParam };

/// @brief SDO transfer granularity (@c Object/Flags/SdoAccess).
///
/// Exists only on @c Object/Flags — @c SubItemType/Flags has no such element
/// (@c EtherCATBase.xsd:115-169) — so every subindex of an object necessarily shares the object's
/// value. When the element is absent the default comes from the device's or module's
/// @c Mailbox/CoE/\@CompleteAccess.
enum class SdoAccess : uint8_t { SubIndexAccess, CompleteAccess };

/// @brief An @c <Access> element together with its two restriction attributes.
///
/// Adopted as a unit during flag inheritance: an @c <Access> that carries @c WriteRestrictions
/// overrides both the mode and the restriction, never one without the other.
struct Access {
  AccessMode mode = AccessMode::Ro;
  StateRestriction readRestrictions = StateRestriction::None;
  StateRestriction writeRestrictions = StateRestriction::None;
};

/// @brief CoE mailbox capabilities (@c Mailbox/CoE attributes).
///
/// @c completeAccess does more than advertise a capability: it is the **default** for
/// @c Object/Flags/SdoAccess whenever that element is absent.
struct MailboxCoe {
  bool sdoInfo = false;
  bool pdoAssign = false;
  bool pdoConfig = false;
  bool pdoUpload = false;
  bool completeAccess = false;
};

/// @brief Symbolic names for the flag enums, in their ESI spelling (@c "ro", @c "m", @c "tr", …).
/// @{
constexpr std::string_view accessModeName(AccessMode v) {
  switch (v) {
    case AccessMode::Ro:
      return "ro";
    case AccessMode::Rw:
      return "rw";
    case AccessMode::Wo:
      return "wo";
  }
  return "ro";
}

constexpr std::string_view categoryName(Category v) {
  switch (v) {
    case Category::Mandatory:
      return "m";
    case Category::Optional:
      return "o";
    case Category::Conditional:
      return "c";
  }
  return "o";
}

constexpr std::string_view pdoMappingName(PdoMapping v) {
  switch (v) {
    case PdoMapping::None:
      return "";
    case PdoMapping::Tx:
      return "t";
    case PdoMapping::Rx:
      return "r";
    case PdoMapping::TxRx:
      return "tr";
  }
  return "";
}

constexpr std::string_view safetyMappingName(SafetyMapping v) {
  switch (v) {
    case SafetyMapping::None:
      return "";
    case SafetyMapping::SafeIn:
      return "si";
    case SafetyMapping::SafeOut:
      return "so";
    case SafetyMapping::SafeInOut:
      return "sio";
    case SafetyMapping::SafeParam:
      return "sp";
  }
  return "";
}

constexpr std::string_view sdoAccessName(SdoAccess v) {
  return v == SdoAccess::CompleteAccess ? "CompleteAccess" : "SubIndexAccess";
}

constexpr std::string_view stateRestrictionName(StateRestriction v) {
  switch (v) {
    case StateRestriction::None:
      return "";
    case StateRestriction::PreOp:
      return "PreOP";
    case StateRestriction::PreOpSafeOp:
      return "PreOP_SafeOP";
    case StateRestriction::PreOpOp:
      return "PreOP_OP";
    case StateRestriction::SafeOp:
      return "SafeOP";
    case StateRestriction::SafeOpOp:
      return "SafeOP_OP";
    case StateRestriction::Op:
      return "OP";
  }
  return "";
}
/// @}

// ---------------------------------------------------------------------------------------------
// Dictionary elements.
// ---------------------------------------------------------------------------------------------

/// @brief One object-dictionary object (ETG.2000 @c ObjectType).
struct EsiObject {
  /// @brief The @c <Flags> block of an object (@c ObjectType/Flags, @c EtherCATBase.xsd:323).
  ///
  /// Every member is @c std::optional because **absence is semantically distinct from the spec
  /// default**, and that distinction is the entire basis of ETG.2000 Figure 37, transcribed in
  /// @c buildDeviceEntries: for a composite object a @c <Flags> on the DataType SubItem shadows
  /// this block *wholesale*, so a flag missing from the SubItem resolves to that flag's own
  /// default — the spec's "Used default value" — rather than to the value here. Collapsing the
  /// optional into a default at parse time would erase the very information the rule branches on.
  ///
  /// @c Transition and @c SdoAccess have no counterpart on @c EsiDataType::SubItem::Flags
  /// (EtherCATBase.xsd gives them to @c ObjectType only), and neither, along with @c attribute,
  /// is covered by Figure 37 — its NOTE scopes the rule to Access, Category, PdoMapping,
  /// SafetyMapping, Backup and Setting.
  ///
  /// Deliberately **not** shared with @c EsiDataType::SubItem::Flags: that one has no
  /// @c SdoAccess and no @c Transition, and two faithful definitions beat one type that lies
  /// about which fields can occur where.
  struct Flags {
    std::optional<Access> access;
    std::optional<Category> category;
    std::optional<PdoMapping> pdoMapping;
    std::optional<SafetyMapping> safetyMapping;
    std::optional<uint32_t> attribute;      ///< SoE IDN attribute (IEC 61158-4-16).
    std::optional<std::string> transition;  ///< Obsolete per ETG.2000; carried, never interpreted.
    std::optional<SdoAccess> sdoAccess;
    std::optional<int32_t> backup;
    std::optional<int32_t> setting;
  };

  /// @brief Value metadata for an object or one of its subindices (ETG.2000 @c ObjectInfoType).
  ///
  /// Recursive by schema: an @c Info holds @c SubItem*, each of which holds another @c Info. The
  /// XSD models the payload as an @c xs:choice of four alternatives, flattened here:
  ///   - @c defaultString — a plain-text default;
  ///   - @c minData / @c maxData / @c defaultData — @c xs:hexBinary, **little-endian**. This is
  ///     the branch real files overwhelmingly use;
  ///   - @c minValue / @c maxValue / @c defaultValue — the obsolete @c HexDecValue branch, still
  ///     emitted occasionally by vendor toolchains;
  ///   - @c subItems — the composite branch.
  ///
  /// An empty byte vector means the element was absent. The byte length is **not** guaranteed to
  /// equal @c ceil(bitSize/8) — real files disagree — which @c buildDeviceEntries reports as a
  /// warning rather than a failure.
  struct Info {
    /// @brief One @c ObjectInfoType/SubItem — a bare @c {Name, Info} pair.
    ///
    /// Note there is no @c SubIdx here: the schema does not provide one. The subindex is derived,
    /// positionally for an ARRAY and from the paired @c DataType/SubItem for a RECORD.
    ///
    /// Defined out of line below @c EsiObject, because a @c SubItem holds an @c Info **by value**
    /// and so needs @c Info complete. @c std::vector tolerates the incomplete element type here.
    struct SubItem;

    std::vector<uint8_t> minData;
    std::vector<uint8_t> maxData;
    std::vector<uint8_t> defaultData;
    std::optional<int64_t> minValue;
    std::optional<int64_t> maxValue;
    std::optional<int64_t> defaultValue;
    std::optional<std::string> defaultString;
    std::optional<std::string> displayName;  ///< Plain @c xs:string at this level, not a NameType.
    std::optional<uint32_t> unit;            ///< Packed ETG.1004 notation; see @c esiUnitSymbol.
    bool hasScaling = false;                 ///< A @c <Scaling> child was seen; not modelled.
    std::vector<SubItem> subItems;
  };

  /// The three attributes below live on the @c <Index> **element**, not on @c <Object>.
  /// @c dependOnSlot is what gates the ETG.5001 slot-offset relocation of a module's objects.
  uint32_t index = 0;
  bool dependOnSlot = false;
  bool dependOnSlotGroup = false;
  bool overwrittenByModule = false;

  std::vector<Text> names;
  std::vector<Text> comments;
  std::string type;  ///< A @c DataType/Name reference, scoped to the enclosing dictionary.
  int32_t bitSize = 0;
  Info info;
  std::optional<Flags> flags;  ///< @c std::nullopt when the object has no @c <Flags> at all.
  std::vector<Property> properties;
};

/// @brief Out-of-line definition of the recursive @c ObjectInfoType/SubItem; see the declaration
///        inside @c EsiObject::Info.
struct EsiObject::Info::SubItem {
  std::string name;
  Info info;
};

/// @brief A dictionary-scoped type definition (ETG.2000 @c DataTypeType).
///
/// @c arrayInfo, @c subItems and @c enumInfo form an @c xs:choice — at most one is non-empty —
/// which yields four shapes:
///   - **scalar / enum**: no @c subItems; @c enumInfo lists the enumerators if any;
///   - **ARRAY**: exactly two @c subItems where @c subItems[1].type is @c name + @c "ARR"
///     (ETG.2000 Table 12, "Data Type Composition");
///   - **ARRAY INFO**: the helper type that reference names, carrying @c baseType + @c arrayInfo;
///   - **RECORD**: any other non-empty @c subItems.
///
/// @warning The type name space is **per dictionary** — @c EtherCATBase.xsd puts an @c xs:key on
/// @c DataType/Name scoped to a single @c <Dictionary>, and a name like @c DT1600 is redefined
/// independently in every device and module dictionary of the same file. Never build one global
/// type table across dictionaries.
struct EsiDataType {
  /// @brief A member of a composite type (ETG.2000 @c SubItemType).
  struct SubItem {
    /// @brief The @c <Flags> block of a SubItem (@c SubItemType/Flags,
    ///        @c EtherCATBase.xsd:115-169).
    ///
    /// Same optional-means-absent rule as @c EsiObject::Flags. Note the two genuine differences
    /// from that type: no @c SdoAccess and no @c Transition.
    struct Flags {
      std::optional<Access> access;
      std::optional<Category> category;
      std::optional<PdoMapping> pdoMapping;
      std::optional<SafetyMapping> safetyMapping;
      std::optional<uint32_t> attribute;
      std::optional<int32_t> backup;
      std::optional<int32_t> setting;
    };

    /// @c std::nullopt when @c <SubIdx> is absent — legal, and common on the element SubItem of
    /// an array composition, where the subindex is positional anyway.
    std::optional<uint8_t> subIdx;
    std::string name;
    std::vector<Text> displayNames;
    std::string type;  ///< A @c DataType/Name reference, scoped to the enclosing dictionary.
    std::vector<Text> comments;
    int32_t bitSize = 0;
    int32_t bitOffs = 0;
    std::vector<uint8_t> defaultData;
    std::optional<int64_t> minValue;
    std::optional<int64_t> maxValue;
    std::optional<int64_t> defaultValue;
    std::optional<std::string> defaultString;
    std::optional<Flags> flags;        ///< @c std::nullopt when there is no @c <Flags> at all.
    std::vector<Property> properties;  ///< Bare @c <Property> children — no wrapper element.
  };

  /// @brief One dimension of an array type (ETG.2000 @c ArrayInfoType).
  ///
  /// @c DataType/ArrayInfo is @c maxOccurs="3", so multi-dimensional arrays are schema-legal and
  /// this is stored as a vector even though one dimension is the norm.
  struct ArrayInfo {
    int64_t lBound = 0;
    int64_t elements = 0;  ///< 0 denotes a variable-length array.
  };

  /// @brief One enumerator of an enumerated type (ETG.2000 @c EnumInfoType).
  struct EnumInfo {
    std::vector<Text> texts;  ///< @c <Text> is @c maxOccurs="unbounded" and may carry @c LcId.
    int64_t value = 0;        ///< @c <Enum>, a HexDecValue.
  };

  std::optional<uint32_t> index;
  std::string name;
  std::optional<std::string> baseType;
  std::vector<Text> comments;
  int32_t bitSize = 0;
  std::vector<ArrayInfo> arrayInfo;
  std::vector<SubItem> subItems;
  std::vector<EnumInfo> enumInfo;
  std::vector<Property> properties;
};

/// @brief A CoE object dictionary (ETG.2000 @c DictionaryType).
struct EsiDictionary {
  /// A dictionary-local override of the ETG.1004 unit catalogue; see @c UnitType.
  std::vector<UnitType> unitTypes;
  std::vector<EsiDataType> dataTypes;
  std::vector<EsiObject> objects;
};

/// @brief A device or module profile (ETG.2000 @c ProfileType).
///
/// @c dictionary and @c dictionaryFile are an @c xs:choice. An external @c <DictionaryFile> is
/// recorded but **not followed** — @c parseEsi is a pure transform over one document and does no
/// filesystem access of its own — and produces a warning so the omission is never silent.
struct EsiProfile {
  std::optional<int32_t> profileNo;  ///< Optional: real modules omit it.
  std::optional<int32_t> addInfo;
  std::optional<int32_t> subAddInfo;
  std::optional<std::string> dictionaryFile;
  std::optional<EsiDictionary> dictionary;
};

// ---------------------------------------------------------------------------------------------
// Process data and modular-device elements.
// ---------------------------------------------------------------------------------------------

/// @brief A process-data object (ETG.2000 @c PdoType) — an @c <RxPdo>, @c <TxPdo>, or
///        @c <SafetyParaMapping>, which all reuse the same schema type.
struct EsiPdo {
  /// @brief One entry mapped into a PDO (ETG.2000 @c EntryType).
  ///
  /// A device-level PDO may reference an index that exists only in a **module** dictionary (a
  /// SOMANET device's RxPDO maps 0x6040, which the CiA402 module declares), so resolving an entry
  /// against the dictionary means resolving against the merged table @c buildDeviceEntries
  /// produces — never against one dictionary alone.
  ///
  /// @c index 0 marks an alignment **padding** entry: @c subIndex and @c dataType are then absent
  /// and only @c bitLen is meaningful. The schema makes both optional precisely for this.
  struct Entry {
    uint32_t index = 0;
    uint8_t subIndex = 0;
    int32_t bitLen = 0;
    std::vector<Text> names;
    std::optional<std::string> dataType;

    /// @brief True for an alignment gap rather than a real mapped object.
    bool isPadding() const { return index == 0; }
  };

  uint32_t index = 0;
  std::vector<Text> names;
  std::vector<uint32_t> excludes;  ///< Indices of PDOs that cannot be enabled alongside this one.
  std::vector<Entry> entries;
  std::optional<int32_t> sm;  ///< Sync-manager this PDO is assigned to.
  std::optional<int32_t> su;
  bool fixed = false;
  bool mandatory = false;
  bool isVirtual = false;
  bool overwrittenByModule = false;
};

/// @brief The @c <Slots> block of a modular device (ETG.5001).
struct EsiSlots {
  /// @brief One slot: a position that a module plugs into (ETG.2000 @c SlotType).
  ///
  /// A slot may offer a **choice** of modules (several @c <ModuleIdent> children, at most one of
  /// which is physically fitted), and @c minInstances == 0 marks the slot itself as optional.
  /// The per-slot increments, when present, override the @c <Slots>-level values.
  struct Slot {
    std::vector<Text> names;
    std::vector<uint32_t> moduleIdents;
    std::optional<uint32_t> defaultModuleIdent;  ///< The ident marked @c Default="1", if any.
    uint32_t minInstances = 0;
    uint32_t maxInstances = 0;
    std::optional<uint32_t> slotGroup;
    std::optional<uint32_t> slotPdoIncrement;
    std::optional<uint32_t> slotIndexIncrement;
  };

  /// @brief A PDO index base for one module PDO group (@c Slots/ModulePdoGroup).
  ///
  /// Selected by @c Module/Type/\@ModulePdoGroup (a 0-based position among these elements).
  ///
  /// The group's @c rxPdo / @c txPdo is an **additional** PDO the group contributes — typically
  /// the alignment PDO named by @c alignment — not a relocation of the member module's own. A
  /// SOMANET safe-motion device settles it: its FSoE module declares @c RxPdo 0x1700 and belongs
  /// to a group whose @c rxPdo is 0x1701, and the device's @c 0x1C12 SM-assignment default lists
  /// **both**. So a module's objects keep the indices it declares, and nothing here relocates
  /// them. Modelled because a config tool assigning PDOs needs the group's base; the object
  /// dictionary is unaffected.
  struct ModulePdoGroup {
    std::optional<int32_t> alignment;
    std::optional<uint32_t> rxPdo;
    std::optional<uint32_t> txPdo;
  };

  std::vector<Slot> slots;
  std::vector<ModulePdoGroup> modulePdoGroups;
  uint32_t slotPdoIncrement = 0;    ///< Index step between PDO objects of consecutive slots.
  uint32_t slotIndexIncrement = 0;  ///< Index step between all other objects of consecutive slots.
  std::optional<uint32_t> maxSlotCount;
  bool downloadModuleIdentList = false;
};

// ---------------------------------------------------------------------------------------------
// Top-level descriptions.
// ---------------------------------------------------------------------------------------------

/// @brief One device description (ETG.2000 @c DeviceType) — the dictionary-relevant subset.
///
/// @c type is the @c <Type> element's own text (e.g. @c "SOMANET Node"); @c productCode and
/// @c revisionNo are that element's attributes. There is no @c Name attribute: display names are
/// the repeated, locale-keyed @c <Name> elements in @c names.
struct EsiDevice {
  std::string type;
  std::optional<uint32_t> productCode;
  std::optional<uint32_t> revisionNo;
  std::optional<uint32_t> serialNo;
  std::vector<Text> names;
  std::vector<Text> comments;
  std::string groupType;
  std::string physics;
  bool invisible = false;
  std::optional<MailboxCoe> coe;

  /// @c DeviceType/Profile is @c maxOccurs="unbounded" in the XSD. Real files carry exactly one,
  /// but the model follows the schema rather than the sample.
  std::vector<EsiProfile> profiles;

  std::vector<EsiPdo> rxPdos;
  std::vector<EsiPdo> txPdos;
  EsiSlots slots;
};

/// @brief One module description (ETG.2000 @c ModuleType).
///
/// Where a device's dictionary typically holds only the communication area, a module's holds the
/// application profile — on a SOMANET drive every CiA402 object (0x6040, 0x6060, 0x607A, …) lives
/// in a module, not in the device. A flat device dictionary is therefore always a *merge*.
struct EsiModule {
  uint32_t moduleIdent = 0;
  std::string type;
  std::optional<std::string> moduleClass;
  std::optional<int32_t> modulePdoGroup;  ///< Index into @c EsiSlots::modulePdoGroups.
  std::vector<Text> names;
  std::optional<MailboxCoe> coe;
  std::optional<EsiProfile> profile;  ///< A single optional, unlike @c EsiDevice::profiles.
  std::vector<EsiPdo> rxPdos;
  std::vector<EsiPdo> txPdos;
  std::vector<EsiPdo> safetyParaMappings;
};

/// @brief A parsed ESI file.
///
/// @c warnings collects every recoverable inconsistency found while parsing — a skipped malformed
/// object, an unparsable HexDecValue, an unfollowed @c <DictionaryFile>. A file with warnings
/// still parses successfully; see @c parseEsi for what does *not*.
struct EsiFile {
  /// @brief The vendor block (ETG.2000 @c VendorType).
  struct Vendor {
    uint32_t id = 0;
    std::vector<Text> names;
    std::vector<Text> urls;
  };

  std::optional<std::string> version;  ///< The @c EtherCATInfo/\@Version attribute.
  Vendor vendor;
  std::vector<EsiDevice> devices;
  std::vector<EsiModule> modules;
  std::vector<std::string> warnings;
};

// ---------------------------------------------------------------------------------------------
// API.
// ---------------------------------------------------------------------------------------------

/// @brief Parses an ESI XML document.
///
/// Tolerates a UTF-8 BOM and either line ending. **Recoverable inconsistencies do not fail the
/// parse**: a malformed object is skipped and recorded in @c EsiFile::warnings, because one bad
/// element in a 50 000-line vendor file must not cost the other five hundred objects. Only three
/// conditions are hard failures — an XML syntax error, a root element other than
/// @c <EtherCATInfo>, and a missing @c Descriptions/Devices.
///
/// @param xml The complete document. Not retained; every string is copied.
/// @return The parsed file, or an error naming the failure and (for a syntax error) its offset.
std::expected<EsiFile, std::string> parseEsi(std::string_view xml);

/// @brief Reads @p path and parses it with @c parseEsi.
/// @return The parsed file, or an error on an unreadable or empty file, or on a parse failure.
std::expected<EsiFile, std::string> parseEsiFile(const std::filesystem::path& path);

/// @brief Finds the device whose @c <Type> text equals @p type (case-sensitive).
/// @return A pointer into @p file, or @c nullptr. Valid only while @p file lives and is unmodified.
const EsiDevice* findEsiDevice(const EsiFile& file, std::string_view type);

/// @brief Finds the device with product code @p productCode, optionally pinned to a revision.
///
/// With @p revisionNo unset, returns the **highest-revision** match — the ESI convention for "the
/// newest description of this product".
///
/// @return A pointer into @p file, or @c nullptr. Valid only while @p file lives and is unmodified.
const EsiDevice* findEsiDeviceByProductCode(const EsiFile& file, uint32_t productCode,
                                            std::optional<uint32_t> revisionNo = std::nullopt);

/// @brief Finds the module with the given @c ModuleIdent.
/// @return A pointer into @p file, or @c nullptr. Valid only while @p file lives and is unmodified.
const EsiModule* findEsiModule(const EsiFile& file, uint32_t moduleIdent);

/// @brief Returns every @c ModuleIdent referenced by any of @p device's slots, in slot order.
///
/// This is the default module set @c buildDeviceEntries merges. Duplicates are removed, keeping
/// the first occurrence.
std::vector<uint32_t> esiSlotModuleIdents(const EsiDevice& device);

/// @brief Serialises the model to JSON. Keys are lowerCamelCase mirrors of the member names;
///        absent optionals are omitted rather than emitted as null.
/// @{
void to_json(nlohmann::json& j, const Text& v);
void to_json(nlohmann::json& j, const Property& v);
void to_json(nlohmann::json& j, const Access& v);
void to_json(nlohmann::json& j, const MailboxCoe& v);
void to_json(nlohmann::json& j, const EsiObject::Flags& v);
void to_json(nlohmann::json& j, const EsiObject::Info& v);
void to_json(nlohmann::json& j, const EsiObject::Info::SubItem& v);
void to_json(nlohmann::json& j, const EsiObject& v);
void to_json(nlohmann::json& j, const EsiDataType::SubItem::Flags& v);
void to_json(nlohmann::json& j, const EsiDataType::SubItem& v);
void to_json(nlohmann::json& j, const EsiDataType::ArrayInfo& v);
void to_json(nlohmann::json& j, const EsiDataType::EnumInfo& v);
void to_json(nlohmann::json& j, const EsiDataType& v);
void to_json(nlohmann::json& j, const EsiDictionary& v);
void to_json(nlohmann::json& j, const EsiProfile& v);
void to_json(nlohmann::json& j, const EsiPdo::Entry& v);
void to_json(nlohmann::json& j, const EsiPdo& v);
void to_json(nlohmann::json& j, const EsiSlots::Slot& v);
void to_json(nlohmann::json& j, const EsiSlots::ModulePdoGroup& v);
void to_json(nlohmann::json& j, const EsiSlots& v);
void to_json(nlohmann::json& j, const EsiDevice& v);
void to_json(nlohmann::json& j, const EsiModule& v);
void to_json(nlohmann::json& j, const EsiFile::Vendor& v);
void to_json(nlohmann::json& j, const EsiFile& v);
/// @}

}  // namespace mm::etg
