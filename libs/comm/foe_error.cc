#include "comm/foe_error.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <string>

#include "comm/foe_error_codes.h"

namespace mm::comm {

FoeError makeFoeRefusal(uint32_t code, std::string_view text, std::string_view op, uint16_t slave,
                        std::string_view filename) {
  constexpr uint32_t kNotFound = 0x8001;
  const FoeErrorKind kind = code == kNotFound ? FoeErrorKind::FileNotFound : FoeErrorKind::Protocol;
  const std::string_view name = foeErrorCodeName(code);
  std::string reason = std::format("FoE error 0x{:04X}", code);
  if (!name.empty()) {
    reason += std::format(": {}", name);
  }
  const bool textRepeatsName = std::ranges::equal(text, name, [](char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
  });
  if (!text.empty() && !textRepeatsName) {
    reason += std::format(", \"{}\"", text);
  }
  return {kind, foeRetry(kind),
          std::format("{} slave {} '{}' failed ({})", op, slave, filename, reason)};
}

bool isTransientReadRefusal(uint32_t code) {
  constexpr uint32_t kIllegal = 0x8004;
  constexpr uint32_t kProgramError = 0x800B;
  return code == kIllegal || code == kProgramError;
}

}  // namespace mm::comm
