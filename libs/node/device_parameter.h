#pragma once

#include <cstdint>
#include <expected>
#include <format>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mm::node {

/// @brief Decoded value of a single device parameter (CoE object dictionary entry).
///
/// The variant alternatives cover the standard ETG.1020 data types that can be
/// transferred via SDO upload. Use @c std::visit to dispatch on the active type.
using DeviceParameterValue =
    std::variant<int8_t, int16_t, int32_t, int64_t,      // NOLINT(whitespace/indent_namespace)
                 uint8_t, uint16_t, uint32_t, uint64_t,  // NOLINT(whitespace/indent_namespace)
                 float, double,                          // NOLINT(whitespace/indent_namespace)
                 std::string, std::vector<uint8_t>>;     // NOLINT(whitespace/indent_namespace)

/// @brief Packs an object dictionary @p index and @p subindex into a single 32-bit key.
///
/// Layout: @c (index << 8) | subindex. Used as the key of @c Device::parameters().
constexpr uint32_t makeParameterKey(uint16_t index, uint8_t subindex) {
  return (static_cast<uint32_t>(index) << 8) | static_cast<uint32_t>(subindex);
}

/// @brief Returns a zero-equivalent value for the given ETG.1020 @p dataType.
///
/// Used to initialise @c DeviceParameter::value so the active variant alternative
/// matches the parameter's declared type before the first read.
/// Unknown data types fall back to @c std::vector<uint8_t>{}.
DeviceParameterValue defaultValueForDataType(uint16_t dataType);

/// @brief Coerces a @c DeviceParameterValue to a @c double, when it holds a number.
///
/// @return The value as a @c double for any arithmetic alternative, or
///         @c std::nullopt for @c std::string / @c std::vector<uint8_t>.
std::optional<double> numericValue(const DeviceParameterValue& value);

/// @brief Tracks how a cached parameter value relates to the device.
///
/// A device may be edited while offline (powered off, or in INIT with no mailbox);
/// this flag lets callers tell a freshly-read value from a stale or pending one
/// (see NEXTGEN.md — "tag offline values with a freshness flag").
enum class SyncState : uint8_t {
  Unknown,  ///< Never read; @c value is the type-appropriate default.
  Synced,   ///< @c value matches the device (last successful read or write).
  Pending,  ///< @c value was set locally while offline / after a failed write.
};

/// @brief Returns the lowercase string form of @p state (@c "unknown" etc.).
std::string_view syncStateName(SyncState state);

/// @brief A single object dictionary entry held by a @c Device.
///
/// Combines immutable schema (index, subindex, name, data type, bit length,
/// access flags, object code) with a @c value that is overwritten by SDO upload
/// or PDO exchange. @c value is initialised to the type-appropriate zero so
/// callers can @c std::visit it directly without checking for a read.
struct DeviceParameter {
  uint16_t index{};            ///< CoE object index.
  uint8_t subindex{};          ///< CoE object subindex.
  std::string name;            ///< Textual description from @c ecx_readOE.
  uint16_t objectCode{};       ///< OTYPE_VAR / OTYPE_ARRAY / OTYPE_RECORD (ETG.1000.6 §5).
  uint16_t dataType{};         ///< ETG.1020 data type code (e.g. 0x0007 = UNSIGNED32).
  uint16_t bitLength{};        ///< Bit length of the value.
  uint16_t access{};           ///< ObjAccess bitfield (read/write per-state flags).
  DeviceParameterValue value;  ///< Last-known value; type-appropriate zero before first read.
  SyncState syncState{SyncState::Unknown};  ///< Freshness of @c value relative to the device.
  std::optional<uint32_t> unit;             ///< ETG.1004 unit code, when reported.
  std::optional<DeviceParameterValue> defaultValue;  ///< Slave-reported default, when available.
  std::optional<DeviceParameterValue> minValue;      ///< Slave-reported minimum, when available.
  std::optional<DeviceParameterValue> maxValue;      ///< Slave-reported maximum, when available.

  /// @brief Returns the packed @c (index, subindex) key used in the parameter map.
  uint32_t key() const { return makeParameterKey(index, subindex); }

  /// @brief Returns @c value as the requested type @p T.
  ///
  /// Type-exact: @p T must be the active variant alternative (e.g. @c uint32_t for an
  /// UNSIGNED32 parameter — booleans are stored as @c uint8_t, see
  /// @c defaultValueForDataType). Use @c numeric() when you only need a number and do
  /// not care about the exact width.
  ///
  /// @return The value, or an error string if @p T is not the active alternative.
  template <typename T>
  std::expected<T, std::string> getValue() const {
    if (const auto* p = std::get_if<T>(&value)) {
      return *p;
    }
    return std::unexpected(
        std::format("parameter 0x{:04X}:{:02X} holds a different type", index, subindex));
  }

  /// @brief Returns @c value coerced to a @c double, for any numeric parameter.
  ///
  /// The "don't worry about the type" accessor — handy because most parameters are
  /// numbers of varying width.
  ///
  /// @return The numeric value, or an error string if @c value is a string / raw bytes.
  std::expected<double, std::string> numeric() const;

  /// @brief Sets @c value, coercing @p v into the parameter's declared data type.
  ///
  /// Numeric inputs are @c static_cast into the alternative matching @c dataType, so
  /// @c setValue(5) works regardless of whether the parameter is UNSIGNED8 or
  /// UNSIGNED32 — unlike a bare variant assignment, which would store the wrong
  /// alternative. Does not touch @c syncState; the caller decides that.
  ///
  /// @return Void on success, or an error string when @p v cannot be coerced (e.g. a
  ///         string into a numeric parameter, or a number into a string parameter).
  std::expected<void, std::string> setValue(const DeviceParameterValue& v);

  /// @brief Strongly-typed @c setValue overload; wraps @p v and coerces as above.
  template <typename T>
  std::expected<void, std::string> setValue(const T& v) {
    return setValue(DeviceParameterValue{v});
  }

  /// @brief Whether the object is readable in any state (ETG.1000.6 ObjAccess read bits 0-2).
  ///
  /// Write-only objects (e.g. a command/trigger sub-object) report no read bits; an SDO upload of
  /// one aborts, so a bulk value refresh should skip them rather than log a spurious failure.
  bool isReadable() const { return (access & 0x07) != 0; }

  /// @brief Whether @p v lies within @c [minValue, maxValue].
  ///
  /// Bounds are compared via numeric coercion. Returns @c true when @p v is
  /// non-numeric or when a bound is unset/non-numeric (i.e. no constraint to violate).
  bool inRange(const DeviceParameterValue& v) const;

  /// @brief Returns @p v clamped to @c [minValue, maxValue].
  ///
  /// Clamps numerically and returns a value of the same alternative as @p v. Returns
  /// @p v unchanged when it is non-numeric or no numeric bounds apply.
  DeviceParameterValue clampToRange(const DeviceParameterValue& v) const;
};

/// @brief Decodes a raw SDO byte sequence according to an ETG.1020 data type code.
///
/// @param dataType  ETG.1020 data type code (e.g. @c 0x0007 = UNSIGNED32).
/// @param bytes     Bytes returned by an SDO upload.
/// @return The decoded value on success, or an error string if @p dataType is
///         unsupported or @p bytes is too short for the type.
std::expected<DeviceParameterValue, std::string> decodeSdoBytes(uint16_t dataType,
                                                                std::span<const uint8_t> bytes);

/// @brief Serialises a @c DeviceParameterValue to raw SDO bytes — the inverse of
///        @c decodeSdoBytes.
///
/// The active variant alternative must match @p dataType (numeric types expect the
/// scalar of the corresponding width; string types expect @c std::string). Strings are
/// written as their raw bytes with **no trailing NUL** so that
/// @c decodeSdoBytes(t, encodeSdoBytes(t, v)) round-trips exactly. Unknown / composite
/// types expect a @c std::vector<uint8_t> and are copied verbatim.
///
/// @param dataType  ETG.1020 data type code.
/// @param value     Value to serialise.
/// @return The encoded bytes on success, or an error string if @p value's alternative
///         does not match @p dataType.
std::expected<std::vector<uint8_t>, std::string> encodeSdoBytes(uint16_t dataType,
                                                                const DeviceParameterValue& value);

/// @brief Serialises a DeviceParameter to JSON.
///
/// Produces an object with keys `index`, `subindex`, `name`, `objectCode`,
/// `dataType`, `bitLength`, `access`, and `value`.
void to_json(nlohmann::json& j, const DeviceParameter& p);

}  // namespace mm::node
