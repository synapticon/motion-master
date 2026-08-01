#include "etg/esi_unit.h"

#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>
#include <string>

namespace mm::etg {

namespace {

/// SI decimal prefixes (ETG.1004). The notation byte is a two's-complement power-of-ten exponent,
/// so 0x03 is kilo (1E3) and 0xFD is milli (1E-3). Only the exponents with a conventional symbol
/// are listed; an unlisted one (e.g. 1E4) renders without a prefix, which is the honest outcome —
/// inventing "10k" would misstate the scale.
constexpr auto kPrefixes = std::to_array<UnitNotation>({
    {0x01, "da", "deca"},
    {0x02, "h", "hecto"},
    {0x03, "k", "kilo"},
    {0x06, "M", "mega"},
    {0x09, "G", "giga"},
    {0x0C, "T", "tera"},
    {0x0F, "P", "peta"},
    {0x12, "E", "exa"},
    {0xEE, "a", "atto"},
    {0xF1, "f", "femto"},
    {0xF4, "p", "pico"},
    {0xF7, "n", "nano"},
    {0xFA, "μ", "micro"},
    {0xFD, "m", "milli"},
    {0xFE, "c", "centi"},
    {0xFF, "d", "deci"},
});

/// The ETG.1004 unit notation catalogue: SI base units (0x00-0x07), derived units with special
/// names (0x10-0x33), permitted non-SI units (0x40-0x5F) and the motion-control additions
/// (0xA0-0xB6). Index 0x00 is the dimensionless "1", which is why a denominator of 0 renders as
/// no denominator at all rather than "/1".
constexpr auto kNotations = std::to_array<UnitNotation>({
    {0x00, "1", "none"},
    {0x01, "m", "metre"},
    {0x02, "kg", "kilogram"},
    {0x03, "s", "second"},
    {0x04, "A", "ampere"},
    {0x05, "K", "kelvin"},
    {0x06, "mol", "mole"},
    {0x07, "cd", "candela"},
    {0x10, "rad", "radian"},
    {0x11, "sr", "steradian"},
    {0x20, "Hz", "hertz"},
    {0x21, "N", "newton"},
    {0x22, "Pa", "pascal"},
    {0x23, "J", "joule"},
    {0x24, "W", "watt"},
    {0x25, "C", "coulomb"},
    {0x26, "V", "volt"},
    {0x27, "F", "farad"},
    {0x28, "Ω", "ohm"},
    {0x29, "S", "siemens"},
    {0x2A, "Wb", "weber"},
    {0x2B, "T", "tesla"},
    {0x2C, "H", "henry"},
    {0x2D, "°C", "degree celsius"},
    {0x2E, "lm", "lumen"},
    {0x2F, "lx", "lux"},
    {0x30, "Bq", "becquerel"},
    {0x31, "Gy", "gray"},
    {0x32, "Sv", "sievert"},
    {0x33, "kat", "katal"},
    {0x40, "g*", "grade"},
    {0x41, "°", "angle degree"},
    {0x42, "'", "angle minute"},
    {0x43, "''", "angle second"},
    {0x44, "l", "litre"},
    {0x45, "a", "are"},
    {0x46, "ha", "hectare"},
    {0x47, "min", "minute"},
    {0x48, "h", "hour"},
    {0x49, "d", "day"},
    {0x4A, "a", "year"},
    {0x4B, "g", "gram"},
    {0x4C, "t", "ton"},
    {0x4E, "bar", "bar"},
    {0x4F, "P", "poise"},
    {0x50, "St", "stokes"},
    {0x51, "eV", "electronvolt"},
    {0x52, "u", "atomic mass unit"},
    {0x53, "AU", "astronomic unit"},
    {0x54, "pc", "parsec"},
    {0x55, "m/s²", "metre per square second"},
    {0x56, "Nm", "newtonmetre"},
    {0x57, "s²", "square second"},
    {0x58, "m²", "square metre"},
    {0x59, "m³", "cubic metre"},
    {0x5A, "Pa s", "pascal second"},
    {0x5B, "J/(kg K)", "joule per kilogram kelvin"},
    {0x5C, "W/(m K)", "watt per metre kelvin"},
    {0x5D, "J/(m K)", "joule per mole kelvin"},
    {0x5E, "W/(m² sr)", "watt per square metre steradian"},
    {0x5F, "kat/m³", "katal per cubic metre"},
    {0xA0, "s³", "cubic second"},
    {0xAC, "Step", "step"},
    {0xB4, "Revolution", "revolution"},
    {0xB5, "Inc", "increment"},
    {0xB6, "rpm", "revolutions per minute"},
});

std::string_view builtinSymbol(std::span<const UnitNotation> table, uint8_t notationIndex) {
  const auto it = std::find_if(table.begin(), table.end(), [notationIndex](const UnitNotation& n) {
    return n.notationIndex == notationIndex;
  });
  return it != table.end() ? it->symbol : std::string_view{};
}

/// Resolves one notation byte, preferring the dictionary's own definition.
std::string_view resolve(std::span<const UnitType> local, uint8_t notationIndex) {
  const auto it = std::find_if(local.begin(), local.end(), [notationIndex](const UnitType& u) {
    return u.notationIndex == notationIndex;
  });
  if (it != local.end() && !it->symbol.empty()) {
    return it->symbol;
  }
  return builtinSymbol(kNotations, notationIndex);
}

}  // namespace

std::string esiUnitSymbol(uint32_t unit, std::span<const UnitType> local) {
  if (unit == 0) {
    return {};
  }

  const UnitParts parts = esiUnitParts(unit);

  // 0x00B44700 composes literally to "Revolution/min". Every drive vendor writes that value
  // meaning rotational speed, and "rpm" is what a user expects to read, so collapse it. This is
  // the one composition special-cased; note it is *not* applied when a dictionary has redefined
  // either byte locally, since then the composition means whatever that dictionary says.
  const bool overridden = std::any_of(local.begin(), local.end(), [&parts](const UnitType& u) {
    return u.notationIndex == parts.numerator || u.notationIndex == parts.denominator;
  });
  if (!overridden && unit == 0x00B44700) {
    return "rpm";
  }

  // The dimensionless unit ("1", notation 0x00) is an absence rather than a factor, in either
  // position: 0x00B50000 renders "Inc", never "Inc/1".
  auto dimensioned = [&local](uint8_t notationIndex) -> std::string_view {
    const std::string_view s = resolve(local, notationIndex);
    return s == "1" ? std::string_view{} : s;
  };

  const std::string_view numerator = dimensioned(parts.numerator);
  const std::string_view denominator = dimensioned(parts.denominator);

  // A prefix on nothing is meaningless — report no unit rather than a bare "m".
  if (numerator.empty() && denominator.empty()) {
    return {};
  }

  std::string symbol;
  symbol += builtinSymbol(kPrefixes, parts.prefix);
  symbol += numerator;
  if (!denominator.empty()) {
    symbol += '/';
    symbol += denominator;
  }
  return symbol;
}

void to_json(nlohmann::json& j, const UnitType& unit) {
  j = nlohmann::json{
      {"notationIndex", unit.notationIndex},
      {"index", unit.index},
      {"name", unit.name},
      {"symbol", unit.symbol},
  };
}

}  // namespace mm::etg
