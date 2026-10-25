#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace mm::node {

/// @brief The firmware version in a device's 0x100A, with the padding of a VISIBLE_STRING removed.
///
/// A firmware pads the string to its declared size with NULs or spaces, and that padding is not
/// part of the version. Everything from the first NUL on is dropped, then leading and trailing
/// whitespace.
std::string trimFirmwareVersion(std::span<const uint8_t> bytes);

/// @brief Reduces a firmware version to the form a cache file name carries.
///
/// Keeps the characters of @c mm::core::isUrlSafeId, turns every other character into @c '_', and
/// keeps at most 64 of them. Idempotent, so the version part of a file name maps to the same file
/// as the string it came from. Two versions can reduce to one name.
std::string sanitizeFirmwareVersion(std::string_view version);

}  // namespace mm::node
