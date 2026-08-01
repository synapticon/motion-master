#pragma once

#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <span>
#include <string>
#include <string_view>

namespace mm::etg {

/// @brief A dictionary-local unit definition (ETG.2000 @c UnitType, @c Dictionary/UnitTypes).
///
/// An ESI may redefine, per dictionary, what a notation index means. This is not hypothetical:
/// Synapticon's FSoE module dictionaries redefine notation index @c 0xB6 from the standard
/// @c "rpm" to @c "Bit". A dictionary's own table therefore **wins** over the built-in ETG.1004
/// catalogue — see @c esiUnitSymbol.
struct UnitType {
  uint8_t notationIndex = 0;  ///< The byte this definition overrides.
  uint32_t index = 0;         ///< Object index of the unit in the 0x0400.. area (informational).
  std::string name;           ///< Human-readable name, e.g. @c "Increments".
  std::string symbol;         ///< Rendered symbol, e.g. @c "Inc".
};

/// @brief One entry of the built-in ETG.1004 notation catalogue.
struct UnitNotation {
  uint8_t notationIndex = 0;
  std::string_view symbol;
  std::string_view name;
};

/// @brief The four bytes of an ETG.1004 unit notation value, split out.
///
/// A @c <Unit> value such as @c #xFD260000 packs, **most significant byte first**:
/// @c [prefix, numerator, denominator, reserved]. Here @c 0xFD is the SI prefix milli (the byte is
/// a two's-complement power-of-ten exponent, so @c 0xFD is −3), @c 0x26 is volt and @c 0x00 is the
/// dimensionless denominator — together @c "mV".
struct UnitParts {
  uint8_t prefix = 0;
  uint8_t numerator = 0;
  uint8_t denominator = 0;
  uint8_t reserved = 0;
};

/// @brief Splits a packed ETG.1004 unit notation value into its four bytes.
constexpr UnitParts esiUnitParts(uint32_t unit) {
  return UnitParts{
      .prefix = static_cast<uint8_t>((unit >> 24) & 0xFF),
      .numerator = static_cast<uint8_t>((unit >> 16) & 0xFF),
      .denominator = static_cast<uint8_t>((unit >> 8) & 0xFF),
      .reserved = static_cast<uint8_t>(unit & 0xFF),
  };
}

/// @brief Renders a packed unit notation value as a display symbol.
///
/// Composes @c prefix + @c numerator + (@c "/" + @c denominator, unless the denominator is
/// dimensionless). Every byte is looked up in @p local first and in the built-in ETG.1004
/// catalogue second, so a dictionary that redefines a notation index gets its own meaning.
///
/// Examples (with no local table): @c 0xFD260000 → @c "mV", @c 0x00B50000 → @c "Inc",
/// @c 0xF756B500 → @c "nNm/Inc", @c 0x03200000 → @c "kHz", @c 0x00B44700 → @c "rpm".
///
/// @param unit  The packed value from @c ObjectInfo/Unit. @c 0 yields an empty string.
/// @param local The enclosing dictionary's @c <UnitTypes>, or empty when it declares none.
/// @return The composed symbol, or an empty string when @p unit is 0 or names nothing known.
std::string esiUnitSymbol(uint32_t unit, std::span<const UnitType> local = {});

/// @brief Serialises a UnitType to JSON (@c notationIndex, @c index, @c name, @c symbol).
void to_json(nlohmann::json& j, const UnitType& unit);

}  // namespace mm::etg
