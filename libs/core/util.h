#pragma once

#include <charconv>
#include <optional>
#include <string_view>

namespace mm::core {

/// @brief Parses an unsigned integer from a string in decimal or hexadecimal notation.
///
/// Accepts an optional @c 0x / @c 0X prefix to select hexadecimal; any other input
/// is parsed as decimal. Trailing characters and out-of-range values are rejected.
///
/// @tparam T  Unsigned integer type to parse into (e.g. @c uint16_t, @c uint8_t).
/// @param s   Input string view.
/// @return The parsed value on success, or @c std::nullopt if @p s is not a valid
///         decimal or hexadecimal representation of a @c T value.
template <typename T>
std::optional<T> parseHexOrDec(std::string_view s) {
  T value{};
  std::from_chars_result r;
  if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    r = std::from_chars(s.data() + 2, s.data() + s.size(), value, 16);
  } else {
    r = std::from_chars(s.data(), s.data() + s.size(), value);
  }
  if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) {  // NOLINT(whitespace/braces)
    return std::nullopt;
  }
  return value;
}

}  // namespace mm::core
