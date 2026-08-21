#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mm::core {

/// @file
/// @brief Standard base64 (RFC 4648 §4), the encoding a JSON request body uses to carry bytes.
///
/// JSON has no binary type, so anything that must travel as a value rather than as a whole request
/// body — a firmware package inside a procedure's parameters, say — is base64 in a string. This is
/// the plain alphabet with `+` and `/` and mandatory `=` padding; the URL-safe variant is not
/// supported because nothing here puts base64 in a URL.

/// @brief Encodes @p bytes as base64 with padding, on one line.
///
/// OpenSSL's encoder wraps at 64 characters (it is built for PEM); the newlines are stripped, since
/// this produces one JSON string value rather than a PEM block.
///
/// @return The encoded text; empty for empty input.
std::string base64Encode(std::span<const uint8_t> bytes);

/// @brief Decodes base64 @p text.
///
/// Line breaks are tolerated wherever they appear, because a base64 payload that went through a
/// text editor or a PEM-shaped tool is routinely wrapped. Everything else is rejected: characters
/// outside the alphabet, and a length that is not a whole number of four-character quanta.
/// Strictness is deliberate — a silently mis-decoded firmware image would be written to a drive.
///
/// @return The decoded bytes, or a message naming what was wrong with @p text.
std::expected<std::vector<uint8_t>, std::string> base64Decode(std::string_view text);

}  // namespace mm::core
