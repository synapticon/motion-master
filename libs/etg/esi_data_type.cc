#include "etg/esi_data_type.h"

#include <algorithm>
#include <charconv>
#include <nlohmann/json.hpp>

namespace mm::etg {

namespace {

/// Finds @p name in kPrimitiveTypes. Exact, case-sensitive: ESI type names are fixed spellings
/// from IEC 61131-3, and a case-folded match would conflate a vendor's own `Uint` composite with
/// the primitive.
const PrimitiveType* findPrimitive(std::string_view name) {
  const auto it = std::find_if(kPrimitiveTypes.begin(), kPrimitiveTypes.end(),
                               [name](const PrimitiveType& t) { return t.name == name; });
  return it != kPrimitiveTypes.end() ? &*it : nullptr;
}

/// Splits `BASE(n)` into its base name and element count. Returns false when @p name is not of
/// that shape, or when the parenthesised text is not a bare decimal count.
bool splitParameterised(std::string_view name, std::string_view& base, uint32_t& elements) {
  const auto open = name.find('(');
  if (open == std::string_view::npos || name.back() != ')') {
    return false;
  }
  const std::string_view digits = name.substr(open + 1, name.size() - open - 2);
  if (digits.empty()) {
    return false;
  }
  uint32_t value = 0;
  const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
  if (ec != std::errc() || ptr != digits.data() + digits.size()) {
    return false;
  }
  base = name.substr(0, open);
  elements = value;
  return true;
}

}  // namespace

std::string_view arrayElementTypeName(std::string_view esiName) {
  // ETG.2000 Table 12 spells these "ARRAY [0..7] OF BYTE". Match on the " OF " separator rather
  // than parsing the bounds, which DataType/ArrayInfo already carries authoritatively.
  if (!esiName.starts_with("ARRAY")) {
    return {};
  }
  const auto of = esiName.rfind(" OF ");
  if (of == std::string_view::npos) {
    return {};
  }
  return esiName.substr(of + 4);
}

bool isStringTypeName(std::string_view esiName) {
  std::string_view base;
  uint32_t elements = 0;
  if (!splitParameterised(esiName, base, elements)) {
    return false;
  }
  return base == "STRING" || base == "OCTET_STRING" || base == "UNICODE_STRING";
}

std::optional<PrimitiveType> resolvePrimitiveType(std::string_view esiName) {
  if (esiName.empty()) {
    return std::nullopt;
  }

  if (const PrimitiveType* exact = findPrimitive(esiName); exact != nullptr) {
    return *exact;
  }

  std::string_view base;
  uint32_t elements = 0;
  if (splitParameterised(esiName, base, elements)) {
    const PrimitiveType* entry = findPrimitive(base);
    if (entry != nullptr) {
      // UNICODE_STRING counts UCS-2 code units; the other two count bytes. Saturate rather than
      // wrap: a declared element count above 8191 cannot be expressed in the 16-bit bit width and
      // is a malformed ESI, which the caller reports as a warning against the declared BitSize.
      const uint32_t bitsPerElement = base == "UNICODE_STRING" ? 16u : 8u;
      const uint64_t bits = static_cast<uint64_t>(elements) * bitsPerElement;
      PrimitiveType resolved = *entry;
      resolved.bitSize = bits <= 0xFFFF ? static_cast<uint16_t>(bits) : uint16_t{0xFFFF};
      return resolved;
    }
    return std::nullopt;
  }

  if (const std::string_view element = arrayElementTypeName(esiName); !element.empty()) {
    return resolvePrimitiveType(element);
  }

  return std::nullopt;
}

ValueKind resolveValueKind(uint16_t dataType, uint16_t bitSize) {
  // The code decides the family; the declared width then has to agree with it, or the entry is not
  // really that type at all (see the header — ARRAY OF BYTE is the case that matters).
  const auto agrees = [bitSize](uint16_t declared) { return bitSize == 0 || bitSize == declared; };

  switch (dataType) {
    case 0x0001:  // BOOL — declared 1 bit, but devices commonly write 8; both are a bool.
      return (bitSize == 0 || bitSize == 1 || bitSize == 8) ? ValueKind::Bool : ValueKind::Bytes;

    case 0x0002:  // SINT
      return agrees(8) ? ValueKind::Int8 : ValueKind::Bytes;
    case 0x0003:  // INT
      return agrees(16) ? ValueKind::Int16 : ValueKind::Bytes;
    case 0x0004:  // DINT
      return agrees(32) ? ValueKind::Int32 : ValueKind::Bytes;
    case 0x0015:  // LINT
      return agrees(64) ? ValueKind::Int64 : ValueKind::Bytes;

    case 0x0005:  // USINT
    case 0x001E:  // BYTE
    case 0x002D:  // BITARR8
      return agrees(8) ? ValueKind::Uint8 : ValueKind::Bytes;
    case 0x0006:  // UINT
    case 0x001F:  // WORD
    case 0x002E:  // BITARR16
      return agrees(16) ? ValueKind::Uint16 : ValueKind::Bytes;
    case 0x0007:  // UDINT
    case 0x0020:  // DWORD
    case 0x002F:  // BITARR32
      return agrees(32) ? ValueKind::Uint32 : ValueKind::Bytes;
    case 0x001B:  // ULINT
      return agrees(64) ? ValueKind::Uint64 : ValueKind::Bytes;

    case 0x0008:  // REAL
      return agrees(32) ? ValueKind::Real32 : ValueKind::Bytes;
    case 0x0011:  // LREAL
      return agrees(64) ? ValueKind::Real64 : ValueKind::Bytes;

    case 0x0009:  // VISIBLE_STRING / STRING(n)
    case 0x000B:  // UNICODE_STRING(n)
      // Width is the parameterised element count, so there is nothing to cross-check against.
      return ValueKind::String;

    default:
      // BIT1..BIT16 land here alongside OCTET_STRING, GUID, DOMAIN, TIME_OF_DAY, the
      // 24/40/48/56-bit integers, and every composite: sub-byte bit types have no distinct C++
      // type, and the rest have no scalar equivalent at all. Bytes is what they are.
      if (dataType >= 0x0030 && dataType <= 0x003F) {  // BIT1..BIT16
        return bitSize <= 8 ? ValueKind::Uint8 : ValueKind::Uint16;
      }
      return ValueKind::Bytes;
  }
}

void to_json(nlohmann::json& j, const PrimitiveType& type) {
  j = nlohmann::json{
      {"name", type.name},
      {"code", type.code},
      {"bitSize", type.bitSize},
      {"isSigned", type.isSigned},
  };
}

}  // namespace mm::etg
