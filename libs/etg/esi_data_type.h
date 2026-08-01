#pragma once

#include <array>
#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string_view>

// <cmath>/<math.h> — pulled in transitively by the STL headers above — defines the obsolete SVID
// matherr macro DOMAIN on macOS/Clang and Windows/MSVC. Left in place it would expand inside the
// kPrimitiveTypes table below and break the build. glibc gates the macro behind a feature test
// that -std=c++2b disables, so only the non-Linux toolchains hit this. See the identical guard in
// libs/comm/object_data_types.h.
#ifdef DOMAIN
#undef DOMAIN
#endif

namespace mm::etg {

/// @brief CoE object code (ETG.1000.6 §5 Table 41 "Object Code").
///
/// The enumerator values *are* the wire codes, so @c static_cast<uint16_t> yields the value a CoE
/// SDO-Information "Get Object Description" response carries in its ObjectCode field, and an
/// @c EsiEntry drops straight into @c mm::node::DeviceParameter::objectCode.
///
/// ETG.1000.6 also defines DEFTYPE (0x05) and DEFSTRUCT (0x06) for type definitions. Those describe
/// entries of the *data type* dictionary, not the object dictionary, and an ESI expresses them as
/// @c <DataType> elements rather than @c <Object> elements — so they never reach this enum.
enum class ObjectCode : uint16_t {
  Var = 0x07,     ///< A single value: no subindices beyond 0.
  Array = 0x08,   ///< Subindex 0 is the entry count; 1..N share one element type.
  Record = 0x09,  ///< Subindex 0 is the entry count; 1..N each have their own type.
};

/// @brief Returns the ESI/CiA spelling of @p code — @c "VAR", @c "ARRAY" or @c "RECORD".
constexpr std::string_view objectCodeName(ObjectCode code) {
  switch (code) {
    case ObjectCode::Var:
      return "VAR";
    case ObjectCode::Array:
      return "ARRAY";
    case ObjectCode::Record:
      return "RECORD";
  }
  return "VAR";
}

/// @brief One primitive type: its ESI spelling and the CoE numbering it maps onto.
///
/// ESI files name types with the **IEC 61131-3** spellings (@c UDINT, @c USINT, @c REAL,
/// @c STRING(50)), while the CoE wire and @c mm::node::decodeSdoBytes speak the **ETG.1020**
/// numbering whose symbolic names are different (@c UNSIGNED32, @c UNSIGNED8, @c REAL32,
/// @c VISIBLE_STRING). This table is the bridge, and it is the whole reason @c mm::etg needs no
/// dependency on @c mm::comm: the codes below are cross-checked against
/// @c mm::comm::kObjectDataTypes but deliberately duplicated as literals, because linking
/// @c mm::comm would drag SOEM into a pure XML parser and its tests.
struct PrimitiveType {
  std::string_view name;  ///< Canonical ESI name (the base name for parameterised types).
  uint16_t code = 0;      ///< ETG.1020 data type code.
  uint16_t bitSize = 0;   ///< Bit width; @c 0 for types whose width comes from a parameter.
  bool isSigned = false;  ///< True for two's-complement signed integers.
};

/// @brief Catalogue of the ESI primitive types, keyed by their IEC 61131-3 name.
///
/// Parameterised names (@c STRING(n), @c OCTET_STRING(n), @c UNICODE_STRING(n)) appear here with
/// their base name and @c bitSize 0; @c resolvePrimitiveType computes the real width from the
/// parenthesised element count. Composite types (RECORD/ARRAY definitions such as @c DT1018) are
/// *not* here — they are declared per dictionary by the ESI itself.
inline constexpr auto kPrimitiveTypes = std::to_array<PrimitiveType>({
    {"BOOL", 0x0001, 1, false},
    {"BOOLEAN", 0x0001, 1, false},

    {"SINT", 0x0002, 8, true},
    {"INT", 0x0003, 16, true},
    {"INT24", 0x0010, 24, true},
    {"DINT", 0x0004, 32, true},
    {"INT40", 0x0012, 40, true},
    {"INT48", 0x0013, 48, true},
    {"INT56", 0x0014, 56, true},
    {"LINT", 0x0015, 64, true},

    {"USINT", 0x0005, 8, false},
    {"UINT", 0x0006, 16, false},
    {"UINT24", 0x0016, 24, false},
    {"UDINT", 0x0007, 32, false},
    {"UINT40", 0x0018, 40, false},
    {"UINT48", 0x0019, 48, false},
    {"UINT56", 0x001A, 56, false},
    {"ULINT", 0x001B, 64, false},

    {"REAL", 0x0008, 32, false},
    {"LREAL", 0x0011, 64, false},

    // Bit-string types. BYTE/WORD/DWORD are distinct CoE codes from the same-width unsigned
    // integers: a device reporting WORD (0x001F) is describing a bitfield, and rendering it as
    // UNSIGNED16 (0x0006) would lose that. v5.6.6.xml uses WORD, so this distinction is live.
    {"BYTE", 0x001E, 8, false},
    {"WORD", 0x001F, 16, false},
    {"DWORD", 0x0020, 32, false},
    {"BITARR8", 0x002D, 8, false},
    {"BITARR16", 0x002E, 16, false},
    {"BITARR32", 0x002F, 32, false},
    {"BIT1", 0x0030, 1, false},
    {"BIT2", 0x0031, 2, false},
    {"BIT3", 0x0032, 3, false},
    {"BIT4", 0x0033, 4, false},
    {"BIT5", 0x0034, 5, false},
    {"BIT6", 0x0035, 6, false},
    {"BIT7", 0x0036, 7, false},
    {"BIT8", 0x0037, 8, false},
    {"BIT9", 0x0038, 9, false},
    {"BIT10", 0x0039, 10, false},
    {"BIT11", 0x003A, 11, false},
    {"BIT12", 0x003B, 12, false},
    {"BIT13", 0x003C, 13, false},
    {"BIT14", 0x003D, 14, false},
    {"BIT15", 0x003E, 15, false},
    {"BIT16", 0x003F, 16, false},

    {"GUID", 0x001D, 128, false},
    {"TIME_OF_DAY", 0x000C, 48, false},
    {"TIME_DIFFERENCE", 0x000D, 48, false},
    {"DOMAIN", 0x000F, 0, false},

    // Parameterised: width comes from the element count in the name.
    {"STRING", 0x0009, 0, false},          // VISIBLE_STRING, 8 bits per element.
    {"OCTET_STRING", 0x000A, 0, false},    // 8 bits per element.
    {"UNICODE_STRING", 0x000B, 0, false},  // 16 bits per element.
});

/// @brief Resolves an ESI type name to its CoE code, width and signedness.
///
/// Handles three shapes:
///   - a plain table name, e.g. @c "UDINT" → {0x0007, 32, false};
///   - a parameterised string, e.g. @c "STRING(50)" → {0x0009, 400, false} and
///     @c "OCTET_STRING(8)" → {0x000A, 64, false} (@c UNICODE_STRING counts 16 bits per element);
///   - an array-composition name, e.g. @c "ARRAY [0..7] OF BYTE", which resolves to its **element**
///     type (@c BYTE here). Such a name denotes the ETG.2000 Table 12 "ARRAY INFO" helper type,
///     which never becomes an entry's own type — the entry takes the element type via
///     @c <BaseType> — so returning the element keeps a caller that asks anyway from getting a
///     surprise.
///
/// @param esiName The @c <Type> / @c <BaseType> / @c <Name> text. Leading and trailing whitespace
///                is **not** trimmed; callers pass an already-trimmed view.
/// @return The resolved primitive, or @c std::nullopt when the name is not a primitive (a composite
///         @c DTxxxx name, or a spelling this table does not know).
std::optional<PrimitiveType> resolvePrimitiveType(std::string_view esiName);

/// @brief Returns the element type name of an @c "ARRAY [a..b] OF T" composition, else empty.
///
/// The bounds are not validated or returned: an ESI carries them redundantly in
/// @c DataType/ArrayInfo, which is the authoritative source.
std::string_view arrayElementTypeName(std::string_view esiName);

/// @brief True when @p esiName is a parameterised string type — @c STRING(n) / @c OCTET_STRING(n) /
///        @c UNICODE_STRING(n).
bool isStringTypeName(std::string_view esiName);

/// @brief Serialises a PrimitiveType to JSON (@c name, @c code, @c bitSize, @c isSigned).
void to_json(nlohmann::json& j, const PrimitiveType& type);

}  // namespace mm::etg
