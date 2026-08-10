#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <vector>

#include "etg/esi.h"
#include "etg/esi_data_type.h"

namespace mm::etg {

/// @brief One label/value pair of an enumerated entry.
///
/// Merged from two sources: the DataType's @c <EnumInfo> children (the schema's own mechanism) and
/// an @c "options" @c Property whose value is a JSON object mapping label to integer (the
/// convention Synapticon uses instead). @c EnumInfo entries come first; the property contributes
/// any label not already present.
struct EsiEnumOption {
  std::string label;
  int64_t value = 0;
};

/// @brief Which dictionary an entry came from.
///
/// A flat table cannot be read back by index alone — 0x6040 looks the same whether the device or a
/// module declared it — so every entry records where it came from.
struct EsiEntrySource {
  /// @brief The device's own dictionary, or a module plugged into one of its slots.
  enum class Kind : uint8_t { Device, Module };

  Kind kind = Kind::Device;
  uint32_t moduleIdent = 0;  ///< 0 for the device dictionary.
  int32_t slot = -1;         ///< −1 for the device dictionary; the 0-based slot number otherwise.
  uint32_t indexOffset = 0;  ///< Offset actually applied to @c rawIndex to arrive at @c index.

  // The module's *name* is deliberately not here: it would be the same string on every one of
  // that module's entries. Resolve `moduleIdent` against the file's module list instead.
};

/// @brief One (index, subindex) row of a device's fully expanded object dictionary.
///
/// This is the library's deliverable: a flat vector of these describes every addressable CoE entry
/// a configured device exposes, merged across its own dictionary and every module in its slots,
/// with the ETG.2000 rules for flag inheritance, subindex derivation and type resolution already
/// applied.
///
/// **Values are raw little-endian bytes plus an ETG.1020 type code, deliberately** — the same
/// convention @c mm::comm::OdEntry documents. Decoding into a typed variant belongs to the node
/// layer (@c mm::node::decodeSdoBytes(dataType, bytes)), which is what keeps @c mm::etg free of
/// any dependency on @c mm::comm or @c mm::node. Note that a byte-wise comparison of @c minData
/// against @c maxData is **wrong** for a signed type — @c minData @c "FF" on a SINT is −1, which
/// sorts above @c maxData @c "01" as bytes. Interpret both through @c dataType first.
struct EsiEntry {
  uint16_t index = 0;     ///< Effective CoE index, with any slot offset applied.
  uint8_t subindex = 0;   ///< CoE subindex.
  uint16_t rawIndex = 0;  ///< The index exactly as written in the source dictionary.

  std::string objectName;   ///< @c Object/Name — identical across every subindex of an object.
  std::string entryName;    ///< This subindex's own name; equals @c objectName for a VAR.
  std::string displayName;  ///< @c Info/DisplayName when the ESI overrides it, else @c entryName.

  /// VAR / ARRAY / RECORD. The enum's underlying value *is* the CoE object code (7/8/9), so it
  /// assigns straight to @c mm::node::DeviceParameter::objectCode.
  ObjectCode objectCode = ObjectCode::Var;
  uint8_t numberOfEntries = 0;  ///< The subindex-0 value (highest subindex); 0 for a VAR.

  std::string dataTypeName;  ///< Resolved ESI type name, e.g. @c "UDINT", @c "STRING(50)".
  uint16_t dataType = 0;     ///< ETG.1020 code; 0 when the name could not be resolved.

  /// @brief The C++ type this entry's value maps to (@c resolveValueKind of @c dataType and
  ///        @c bitSize).
  ///
  /// Precomputed here for the same reason @c unitSymbol is: it is derived from two other fields by
  /// a rule with a trap in it — a width that contradicts its type code means the entry is bytes,
  /// not the scalar the code names — and every consumer deriving it independently is how one of
  /// them gets it wrong.
  ValueKind valueKind = ValueKind::Bytes;
  bool isSigned = false;   ///< Whether @c minData / @c maxData / @c defaultData are two's
                           ///< complement — needed to compare or render them correctly.
  uint16_t bitSize = 0;    ///< Width of this entry.
  uint16_t bitOffset = 0;  ///< Offset within the object's complete-access image.

  std::optional<std::vector<uint8_t>> defaultData;  ///< Raw bytes, first byte least significant.
  std::optional<std::vector<uint8_t>> minData;
  std::optional<std::vector<uint8_t>> maxData;

  std::optional<uint32_t> unit;  ///< Packed ETG.1004 notation value, e.g. 0xFD260000.
  std::string unitSymbol;        ///< Rendered symbol, e.g. @c "mV"; empty when unresolvable.

  Access access{};  ///< Resolved mode plus read/write AL-state restrictions.
  Category category = Category::Optional;
  PdoMapping pdoMapping = PdoMapping::None;
  SafetyMapping safetyMapping = SafetyMapping::None;
  SdoAccess sdoAccess = SdoAccess::SubIndexAccess;
  bool backup = false;
  bool setting = false;
  std::optional<uint32_t> attribute;  ///< SoE IDN, when declared.

  /// @brief ETG.1000.6 @c ObjAccess bitfield synthesised from the resolved flags.
  ///
  /// Bits 0-2 readable in PreOP/SafeOP/OP, 3-5 writable in PreOP/SafeOP/OP, 6 RxPDO-mappable,
  /// 7 TxPDO-mappable, 8 backup, 9 setting. Provided so an entry drops straight into
  /// @c mm::node::DeviceParameter::access and its @c isReadable() without a second conversion.
  uint16_t objAccess = 0;

  /// @brief Every @c <Property> as written, for whatever this row describes.
  ///
  /// @c <Property> is the format's generic annotation mechanism, so these are carried verbatim
  /// and a vendor that uses names other than Synapticon's loses nothing. @c description and
  /// @c options below are *conveniences* decoded from this vector for the two conventional names;
  /// both are empty for such a vendor, whose data is still right here.
  ///
  /// **Subindex 0 describes the object itself**, so on that row this is the object's own
  /// annotation (the Object's for a VAR, the DataType's for an ARRAY or RECORD). Every other row
  /// carries only what that specific entry declares — which for a RECORD member is its own
  /// description, and for an ARRAY element is nothing, since the ESI describes an array once
  /// rather than per element. Object-level text is therefore stored **once**, not repeated onto
  /// every subindex: look it up on subindex 0 of the same index.
  std::vector<Property> properties;

  std::string description;  ///< The @c "description" property of @c properties, decoded to text.
  std::vector<EsiEnumOption> options;

  EsiEntrySource source;

  /// @brief Packed @c (index << 8) | subindex — the same key as @c mm::node::makeParameterKey.
  uint32_t key() const { return (static_cast<uint32_t>(index) << 8) | subindex; }
};

/// @brief What to do when two dictionaries declare the same (index, subindex).
enum class EsiCollisionPolicy : uint8_t {
  LastWins,   ///< The later definition replaces the earlier one.
  FirstWins,  ///< The earlier definition is kept (device dictionary, then slots in order).
  Error,      ///< Fail the whole build.
};

/// @brief Knobs for @c buildDeviceEntries.
struct EsiEntryOptions {
  uint32_t lcId = kDefaultLcId;  ///< Preferred locale for names and descriptions.

  /// @brief Restricts the merge to these module idents, in the order the slots declare them.
  ///
  /// Empty — the default — merges **every** ident any slot references. That is the right default
  /// for offline inspection, where which module is physically fitted is unknowable: the result is
  /// the union of everything the device could expose. Where a slot offers mutually exclusive
  /// alternatives this necessarily produces collisions, resolved by @c collisionPolicy. Naming
  /// idents here models one concrete configuration instead.
  std::vector<uint32_t> moduleIdents;

  bool applySlotOffsets = true;  ///< Apply @c SlotIndexIncrement / @c SlotPdoIncrement.

  /// @brief Only relocate objects whose @c <Index> carries @c DependOnSlot="1".
  ///
  /// @c true — the default — is the literal ETG.2000 reading, and is a no-op on a file that sets
  /// the attribute nowhere, which matches what such a device's firmware actually answers to. Set
  /// @c false to relocate every module object unconditionally. Getting this wrong invents indices
  /// the drive will reject, which is worse than omitting them, hence the conservative default.
  bool requireDependOnSlot = true;

  EsiCollisionPolicy collisionPolicy = EsiCollisionPolicy::LastWins;

  /// @brief Zero-pad or sign-extend a hexBinary value shorter than its declared width.
  ///
  /// Real files disagree with themselves here, so padding is on by default and always warns,
  /// naming the address. @c false leaves the bytes exactly as written.
  bool padShortValues = true;

  bool sortByAddress = true;  ///< Sort by (index, subindex); @c false keeps document order.

  std::size_t maxWarnings = 1000;  ///< Cap before warnings are summarised.
};

/// @brief A device's flat dictionary, plus everything questionable found while building it.
struct EsiEntryTable {
  std::vector<EsiEntry> entries;
  std::vector<std::string> warnings;
};

/// @brief Expands a device into a flat, slot-merged vector of object-dictionary entries.
///
/// Walks the device's own dictionary first, then each slot in order; for every object it derives
/// one entry per subindex, applying the ETG.2000 rules for flag inheritance (§Figure 37), subindex
/// derivation, per-subindex type resolution, and description lookup. Per-entry problems become
/// warnings, never failures — see @c EsiEntryTable::warnings.
///
/// @param file    The parsed ESI; needed to resolve the slots' @c ModuleIdent references.
/// @param device  A device belonging to @p file.
/// @param options Locale, module selection, offsets, collision policy.
/// @return The flat table, or an error when the device resolves to no dictionary at all, or when
///         @c EsiCollisionPolicy::Error is set and a collision occurred.
std::expected<EsiEntryTable, std::string> buildDeviceEntries(const EsiFile& file,
                                                             const EsiDevice& device,
                                                             const EsiEntryOptions& options = {});

/// @brief Serialises an entry to JSON. Raw byte vectors are emitted as uppercase hexBinary
///        strings, the same spelling the ESI itself uses; absent optionals are omitted.
/// @{
void to_json(nlohmann::json& j, const EsiEnumOption& v);
void to_json(nlohmann::json& j, const EsiEntrySource& v);
void to_json(nlohmann::json& j, const EsiEntry& v);
void to_json(nlohmann::json& j, const EsiEntryTable& v);
/// @}

}  // namespace mm::etg
