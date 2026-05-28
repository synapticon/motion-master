#pragma once

#include <cstdint>
#include <expected>
#include <nlohmann/json_fwd.hpp>
#include <span>
#include <string>
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

  /// @brief Returns the packed @c (index, subindex) key used in the parameter map.
  uint32_t key() const { return makeParameterKey(index, subindex); }
};

/// @brief Decodes a raw SDO byte sequence according to an ETG.1020 data type code.
///
/// @param dataType  ETG.1020 data type code (e.g. @c 0x0007 = UNSIGNED32).
/// @param bytes     Bytes returned by an SDO upload.
/// @return The decoded value on success, or an error string if @p dataType is
///         unsupported or @p bytes is too short for the type.
std::expected<DeviceParameterValue, std::string> decodeSdoBytes(uint16_t dataType,
                                                                std::span<const uint8_t> bytes);

/// @brief Serialises a DeviceParameter to JSON.
///
/// Produces an object with keys `index`, `subindex`, `name`, `objectCode`,
/// `dataType`, `bitLength`, `access`, and `value`.
void to_json(nlohmann::json& j, const DeviceParameter& p);

}  // namespace mm::node
