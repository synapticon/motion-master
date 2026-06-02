#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <optional>
#include <string_view>

namespace mm::core {

/// @brief Whether @p s is a valid URL-safe identifier usable as a path segment and pub/sub topic.
///
/// Accepts a non-empty string of at most 64 characters drawn from @c [A-Za-z0-9._-] — the
/// unreserved subset that needs no percent-encoding in a URL path segment and carries no
/// special meaning to the uWebSockets topic parser (notably no @c '/', which it treats as a
/// hierarchy delimiter). Comparison is case-sensitive, so @c "Motor" and @c "motor" are
/// distinct identifiers.
///
/// @param s  Candidate identifier.
/// @return @c true if every character is allowed and the length is in @c [1, 64].
inline bool isUrlSafeId(std::string_view s) {
  constexpr std::size_t kMaxLength = 64;
  if (s.empty() || s.size() > kMaxLength) {
    return false;
  }
  return std::all_of(s.begin(), s.end(), [](const char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
           c == '_' || c == '-';
  });
}

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
