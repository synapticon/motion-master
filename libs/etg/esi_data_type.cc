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
  const auto* end = digits.data() + digits.size();
  const auto [ptr, ec] = std::from_chars(digits.data(), end, value);
  if (ec != std::errc() || ptr != end) {
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

void to_json(nlohmann::json& j, const PrimitiveType& type) {
  j = nlohmann::json{
      {"name", type.name},
      {"code", type.code},
      {"bitSize", type.bitSize},
      {"isSigned", type.isSigned},
  };
}

}  // namespace mm::etg
