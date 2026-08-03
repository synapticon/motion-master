#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace mm::core {

/// @brief Whether @p s is a valid URL-safe identifier usable as a path segment and pub/sub topic.
///
/// Accepts a non-empty string of at most 64 characters drawn from @c [A-Za-z0-9._-] — the
/// unreserved subset that needs no percent-encoding in a URL path segment and carries no
/// special meaning to the uWebSockets topic parser. Rejecting @c '/' is doubly load-bearing:
/// the id is a single REST path segment (e.g. @c GET @c /api/monitorings/{topic}), so a @c '/'
/// would spill into extra segments and miss the route; and uWebSockets treats it as a topic
/// hierarchy delimiter. Comparison is case-sensitive, so @c "Motor" and @c "motor" are
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
/// Accepts a @c 0x / @c 0X prefix (C-style) or a @c #x / @c #X prefix (as EtherCAT ESI/XML
/// files write hex constants, e.g. @c "#x00000201") to select hexadecimal; any other input
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
  if (s.size() >= 2 && (s[0] == '0' || s[0] == '#') && (s[1] == 'x' || s[1] == 'X')) {
    r = std::from_chars(s.data() + 2, s.data() + s.size(), value, 16);
  } else {
    r = std::from_chars(s.data(), s.data() + s.size(), value);
  }
  if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) {  // NOLINT(whitespace/braces)
    return std::nullopt;
  }
  return value;
}

/// @brief Formats bytes as uppercase hexadecimal.
///
/// For the two shapes this codebase needs: a continuous string (the ESI @c hexBinary XML encoding)
/// and a separated one (a log line meant to be read against a specification's byte table).
///
/// @param bytes      Source buffer (a @c std::vector<uint8_t> binds implicitly).
/// @param separator  Placed between bytes; empty (the default) for a continuous string.
/// @return @c "0A1B2C", or @c "0A 1B 2C" given a @c " " separator. Empty for empty input.
inline std::string toHex(std::span<const uint8_t> bytes, std::string_view separator = {}) {
  std::string out;
  out.reserve(bytes.size() * (2 + separator.size()));
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (i != 0) {
      out += separator;
    }
    out += std::format("{:02X}", bytes[i]);
  }
  return out;
}

/// @brief Encodes an integer into a fixed-width byte array in the given byte order.
///
/// The width is @c sizeof(T), so @c toBytes<uint8_t> yields one byte, @c uint16_t two, and so on.
/// Signed values are encoded through their unsigned representation (two's complement), so the shift
/// is always well-defined. Defaults to little-endian (EtherCAT/CoE wire order); pass
/// @c std::endian::big for the reverse. The returned array is the natural argument for a byte-span
/// sink such as @c FieldbusDriver::writeSdo — a temporary result lives to the end of the enclosing
/// call.
///
/// @tparam T   Integral type whose size fixes the byte count.
/// @param value  Value to encode.
/// @param order  Byte order of the output (default little-endian).
/// @return A @c std::array of @c sizeof(T) bytes in @p order.
template <typename T>
std::array<uint8_t, sizeof(T)> toBytes(T value, std::endian order = std::endian::little) {
  static_assert(std::is_integral_v<T>, "toBytes is for integral types");
  using U = std::make_unsigned_t<T>;
  const auto u = static_cast<U>(value);
  std::array<uint8_t, sizeof(T)> out{};
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    const std::size_t byte = (order == std::endian::little) ? i : (sizeof(T) - 1 - i);
    out[byte] = static_cast<uint8_t>((u >> (8 * i)) & 0xFFu);
  }
  return out;
}

/// @brief Decodes an integer from a raw byte buffer interpreted in the given byte order.
///
/// Reads up to @c sizeof(T) bytes. A short buffer is treated as if zero-padded (the missing
/// most-significant bytes read as zero) — defensive against a slave returning fewer bytes than the
/// type implies; excess bytes are ignored. Accumulation is done in the unsigned representation so
/// the shift is well-defined for signed @c T. Defaults to little-endian (EtherCAT/CoE wire order).
///
/// @tparam T   Integral result type whose size fixes how many bytes are consumed.
/// @param bytes  Source buffer (a @c std::vector<uint8_t> binds implicitly).
/// @param order  Byte order to interpret @p bytes in (default little-endian).
/// @return The decoded value.
template <typename T>
T fromBytes(std::span<const uint8_t> bytes, std::endian order = std::endian::little) {
  static_assert(std::is_integral_v<T>, "fromBytes is for integral types");
  using U = std::make_unsigned_t<T>;
  U v = 0;
  const std::size_t n = std::min(sizeof(T), bytes.size());
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t shift = (order == std::endian::little) ? (8 * i) : (8 * (n - 1 - i));
    v |= static_cast<U>(bytes[i]) << shift;
  }
  return static_cast<T>(v);
}

}  // namespace mm::core
