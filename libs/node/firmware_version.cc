#include "node/firmware_version.h"

#include <cstddef>
#include <string>

namespace mm::node {

std::string trimFirmwareVersion(std::span<const uint8_t> bytes) {
  std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  text = text.substr(0, text.find('\0'));
  const auto isPadding = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!text.empty() && isPadding(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && isPadding(text.back())) {
    text.remove_suffix(1);
  }
  return std::string(text);
}

std::string sanitizeFirmwareVersion(std::string_view version) {
  constexpr std::size_t kMaxLength = 64;
  std::string out;
  for (const char c : version.substr(0, kMaxLength)) {
    const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '.' || c == '_' || c == '-';
    out.push_back(safe ? c : '_');
  }
  return out;
}

}  // namespace mm::node
