#include "node/device_parameter.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "comm/object_data_types.h"

namespace mm::node {

namespace {

using mm::comm::ObjectDataType;

template <typename T>
std::expected<DeviceParameterValue, std::string> decodeScalar(uint16_t dataType,
                                                              std::span<const uint8_t> bytes) {
  if (bytes.size() < sizeof(T)) {
    return std::unexpected(std::format("decodeSdoBytes: data type 0x{:04X} needs {} bytes, got {}",
                                       dataType, sizeof(T), bytes.size()));
  }
  T value{};
  std::memcpy(&value, bytes.data(), sizeof(T));
  return DeviceParameterValue{value};
}

// Serialises the scalar alternative @c T of @p value to little-endian bytes. Errors when
// @p value does not currently hold a @c T (caller should coerce via setValue first).
template <typename T>
std::expected<std::vector<uint8_t>, std::string> encodeScalar(uint16_t dataType,
                                                              const DeviceParameterValue& value) {
  const auto* p = std::get_if<T>(&value);
  if (!p) {
    return std::unexpected(std::format(
        "encodeSdoBytes: data type 0x{:04X} expects a {}-byte scalar but the value holds "
        "a different alternative",
        dataType, sizeof(T)));
  }
  std::vector<uint8_t> out(sizeof(T));
  std::memcpy(out.data(), p, sizeof(T));
  return out;
}

}  // namespace

DeviceParameterValue defaultValueForDataType(uint16_t dataType) {
  switch (static_cast<ObjectDataType>(dataType)) {
    case ObjectDataType::BOOLEAN:
    case ObjectDataType::UNSIGNED8:
    case ObjectDataType::BYTE:
      return uint8_t{0};
    case ObjectDataType::INTEGER8:
      return int8_t{0};
    case ObjectDataType::INTEGER16:
      return int16_t{0};
    case ObjectDataType::INTEGER32:
      return int32_t{0};
    case ObjectDataType::UNSIGNED16:
    case ObjectDataType::WORD:
      return uint16_t{0};
    case ObjectDataType::UNSIGNED32:
    case ObjectDataType::DWORD:
      return uint32_t{0};
    case ObjectDataType::REAL32:
      return float{0.0F};
    case ObjectDataType::VISIBLE_STRING:
    case ObjectDataType::UNICODE_STRING:
      return std::string{};
    case ObjectDataType::REAL64:
      return double{0.0};
    case ObjectDataType::INTEGER64:
      return int64_t{0};
    case ObjectDataType::UNSIGNED64:
      return uint64_t{0};
    default:
      // BIT*, BITARR*, GUID, OCTET_STRING, RECORD, composite/user types, and
      // anything not in the table fall through to raw bytes.
      return std::vector<uint8_t>{};
  }
}

bool isScalarDataType(uint16_t dataType) {
  switch (static_cast<ObjectDataType>(dataType)) {
    case ObjectDataType::BOOLEAN:
    case ObjectDataType::UNSIGNED8:
    case ObjectDataType::BYTE:
    case ObjectDataType::INTEGER8:
    case ObjectDataType::INTEGER16:
    case ObjectDataType::INTEGER32:
    case ObjectDataType::UNSIGNED16:
    case ObjectDataType::WORD:
    case ObjectDataType::UNSIGNED32:
    case ObjectDataType::DWORD:
    case ObjectDataType::REAL32:
    case ObjectDataType::REAL64:
    case ObjectDataType::INTEGER64:
    case ObjectDataType::UNSIGNED64:
      return true;
    default:
      // Strings, and the BIT*/GUID/OCTET_STRING/composite types defaultValueForDataType maps to
      // raw bytes. Kept as one switch beside that table so the two cannot drift: every case
      // returning a scalar alternative there must return true here.
      return false;
  }
}

uint64_t packLeBits(std::span<const uint8_t> bytes) {
  uint64_t out = 0;
  const size_t n = std::min<size_t>(bytes.size(), sizeof(uint64_t));
  for (size_t i = 0; i < n; ++i) {
    out |= static_cast<uint64_t>(bytes[i]) << (8U * i);
  }
  return out;
}

void unpackLeBits(uint64_t bits, std::span<uint8_t> out) {
  for (size_t i = 0; i < out.size(); ++i) {
    out[i] = i < sizeof(uint64_t) ? static_cast<uint8_t>(bits >> (8U * i)) : uint8_t{0};
  }
}

std::expected<DeviceParameterValue, std::string> decodeSdoBytes(uint16_t dataType,
                                                                std::span<const uint8_t> bytes) {
  switch (static_cast<ObjectDataType>(dataType)) {
    case ObjectDataType::BOOLEAN:
    case ObjectDataType::UNSIGNED8:
    case ObjectDataType::BYTE:
      return decodeScalar<uint8_t>(dataType, bytes);
    case ObjectDataType::INTEGER8:
      return decodeScalar<int8_t>(dataType, bytes);
    case ObjectDataType::INTEGER16:
      return decodeScalar<int16_t>(dataType, bytes);
    case ObjectDataType::INTEGER32:
      return decodeScalar<int32_t>(dataType, bytes);
    case ObjectDataType::UNSIGNED16:
    case ObjectDataType::WORD:
      return decodeScalar<uint16_t>(dataType, bytes);
    case ObjectDataType::UNSIGNED32:
    case ObjectDataType::DWORD:
      return decodeScalar<uint32_t>(dataType, bytes);
    case ObjectDataType::REAL32:
      return decodeScalar<float>(dataType, bytes);
    case ObjectDataType::REAL64:
      return decodeScalar<double>(dataType, bytes);
    case ObjectDataType::INTEGER64:
      return decodeScalar<int64_t>(dataType, bytes);
    case ObjectDataType::UNSIGNED64:
      return decodeScalar<uint64_t>(dataType, bytes);
    case ObjectDataType::VISIBLE_STRING: {
      // ETG.1000.6: VISIBLE_STRING is a fixed-width field padded with trailing
      // spaces (and may carry a terminating NUL). Devices return the full padded
      // field on upload — neither is significant, so strip them for display.
      size_t len = bytes.size();
      while (len > 0 && (bytes[len - 1] == 0x00 || bytes[len - 1] == 0x20)) {
        --len;
      }
      return DeviceParameterValue{std::string(reinterpret_cast<const char*>(bytes.data()), len)};
    }
    case ObjectDataType::UNICODE_STRING:
      return DeviceParameterValue{
          std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size())};
    default:
      // Unknown / composite type — return raw bytes so the caller can inspect.
      return DeviceParameterValue{std::vector<uint8_t>(bytes.begin(), bytes.end())};
  }
}

std::expected<std::vector<uint8_t>, std::string> encodeSdoBytes(uint16_t dataType,
                                                                const DeviceParameterValue& value) {
  switch (static_cast<ObjectDataType>(dataType)) {
    case ObjectDataType::BOOLEAN:
    case ObjectDataType::UNSIGNED8:
    case ObjectDataType::BYTE:
      return encodeScalar<uint8_t>(dataType, value);
    case ObjectDataType::INTEGER8:
      return encodeScalar<int8_t>(dataType, value);
    case ObjectDataType::INTEGER16:
      return encodeScalar<int16_t>(dataType, value);
    case ObjectDataType::INTEGER32:
      return encodeScalar<int32_t>(dataType, value);
    case ObjectDataType::UNSIGNED16:
    case ObjectDataType::WORD:
      return encodeScalar<uint16_t>(dataType, value);
    case ObjectDataType::UNSIGNED32:
    case ObjectDataType::DWORD:
      return encodeScalar<uint32_t>(dataType, value);
    case ObjectDataType::REAL32:
      return encodeScalar<float>(dataType, value);
    case ObjectDataType::REAL64:
      return encodeScalar<double>(dataType, value);
    case ObjectDataType::INTEGER64:
      return encodeScalar<int64_t>(dataType, value);
    case ObjectDataType::UNSIGNED64:
      return encodeScalar<uint64_t>(dataType, value);
    case ObjectDataType::VISIBLE_STRING:
    case ObjectDataType::UNICODE_STRING: {
      const auto* s = std::get_if<std::string>(&value);
      if (!s) {
        return std::unexpected(
            std::format("encodeSdoBytes: data type 0x{:04X} expects a string value", dataType));
      }
      // No trailing NUL: keep symmetric with decodeSdoBytes, which takes the whole span.
      return std::vector<uint8_t>(s->begin(), s->end());
    }
    default: {
      // Unknown / composite type — expect raw bytes and copy verbatim.
      const auto* raw = std::get_if<std::vector<uint8_t>>(&value);
      if (!raw) {
        return std::unexpected(
            std::format("encodeSdoBytes: data type 0x{:04X} expects raw bytes", dataType));
      }
      return *raw;
    }
  }
}

std::optional<double> numericValue(const DeviceParameterValue& value) {
  return std::visit(
      [](const auto& v) -> std::optional<double> {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_arithmetic_v<T>) {
          return static_cast<double>(v);
        } else {
          return std::nullopt;
        }
      },
      value);
}

std::string_view syncStateName(SyncState state) {
  switch (state) {
    case SyncState::Unknown:
      return "unknown";
    case SyncState::Synced:
      return "synced";
    case SyncState::Pending:
      return "pending";
  }
  return "unknown";
}

std::string_view parameterOriginName(ParameterOrigin origin) {
  switch (origin) {
    case ParameterOrigin::ObjectDictionary:
      return "objectDictionary";
    case ParameterOrigin::Sii:
      return "sii";
  }
  return "objectDictionary";
}

DeviceParameterValue DeviceParameter::currentValue() const {
  // Reconstructed rather than stored: dataType is immutable, so the same mapping
  // defaultValueForDataType uses to pick the alternative turns the stored wire bytes back into it.
  // A decode failure can only mean the stored bytes are too short for the type (nothing has been
  // read yet, or a slave answered short), for which the type's zero is the honest answer.
  if (isScalarDataType(dataType)) {
    // Always the full eight bytes: decodeSdoBytes takes the leading sizeof(T) of them, so one
    // buffer serves every scalar width and the zero-extended tail is never read.
    std::array<uint8_t, sizeof(uint64_t)> buf{};
    unpackLeBits(loadBits(), buf);
    auto decoded = decodeSdoBytes(dataType, buf);
    return decoded ? std::move(*decoded) : defaultValueForDataType(dataType);
  }
  auto decoded = decodeSdoBytes(dataType, rawValue);
  return decoded ? std::move(*decoded) : defaultValueForDataType(dataType);
}

void DeviceParameter::setRawValue(std::span<const uint8_t> bytes) {
  if (isScalarDataType(dataType)) {
    storeBits(packLeBits(bytes));
    return;
  }
  rawValue.assign(bytes.begin(), bytes.end());
}

std::expected<double, std::string> DeviceParameter::numeric() const {
  auto n = numericValue(currentValue());
  if (n) {
    return *n;
  }
  return std::unexpected(std::format("parameter 0x{:04X}:{:02X} is not numeric", index, subindex));
}

std::expected<void, std::string> DeviceParameter::setValue(const DeviceParameterValue& v) {
  // Two steps, because storage is wire bytes and the caller may pass any numeric width: coerce the
  // incoming value into the alternative the declared data type dictates, then encode that to bytes.
  // Coercion is what makes setValue(5) work whether the object is UNSIGNED8 or UNSIGNED32 — a bare
  // assignment would encode the wrong width.
  DeviceParameterValue target = defaultValueForDataType(dataType);
  auto coerced = std::visit(
      [this](const auto& proto,
             const auto& incoming) -> std::expected<DeviceParameterValue, std::string> {
        using Target = std::decay_t<decltype(proto)>;
        using In = std::decay_t<decltype(incoming)>;
        if constexpr (std::is_arithmetic_v<Target>) {
          if constexpr (std::is_arithmetic_v<In>) {
            return DeviceParameterValue{static_cast<Target>(incoming)};
          } else {
            return std::unexpected(std::format(
                "parameter 0x{:04X}:{:02X} is numeric; cannot assign a non-numeric value", index,
                subindex));
          }
        } else if constexpr (std::is_same_v<Target, std::string>) {
          if constexpr (std::is_same_v<In, std::string>) {
            return DeviceParameterValue{incoming};
          } else {
            return std::unexpected(std::format(
                "parameter 0x{:04X}:{:02X} is a string; cannot assign a non-string value", index,
                subindex));
          }
        } else {  // std::vector<uint8_t>
          if constexpr (std::is_same_v<In, std::vector<uint8_t>>) {
            return DeviceParameterValue{incoming};
          } else {
            return std::unexpected(
                std::format("parameter 0x{:04X}:{:02X} is raw bytes; assign a std::vector<uint8_t>",
                            index, subindex));
          }
        }
      },
      target, v);
  if (!coerced) {
    return std::unexpected(coerced.error());
  }
  auto bytes = encodeSdoBytes(dataType, *coerced);
  if (!bytes) {
    return std::unexpected(bytes.error());
  }
  setRawValue(*bytes);
  return {};
}

bool DeviceParameter::inRange(const DeviceParameterValue& v) const {
  auto n = numericValue(v);
  if (!n) {
    return true;  // non-numeric values have no range to violate
  }
  if (minValue) {
    if (auto lo = numericValue(*minValue); lo && *n < *lo) {
      return false;
    }
  }
  if (maxValue) {
    if (auto hi = numericValue(*maxValue); hi && *n > *hi) {
      return false;
    }
  }
  return true;
}

DeviceParameterValue DeviceParameter::clampToRange(const DeviceParameterValue& v) const {
  auto n = numericValue(v);
  if (!n) {
    return v;
  }
  double r = *n;
  if (minValue) {
    if (auto lo = numericValue(*minValue); lo && r < *lo) {
      r = *lo;
    }
  }
  if (maxValue) {
    if (auto hi = numericValue(*maxValue); hi && r > *hi) {
      r = *hi;
    }
  }
  if (r == *n) {
    return v;  // unchanged — preserve the exact alternative and value
  }
  // Re-cast the clamped number back into v's own alternative.
  return std::visit(
      [r](const auto& x) -> DeviceParameterValue {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_arithmetic_v<T>) {
          return static_cast<T>(r);
        } else {
          return x;  // unreachable: numericValue(v) succeeded above
        }
      },
      v);
}

void to_json(nlohmann::json& j, const DeviceParameter& p) {
  j = nlohmann::json{
      {"index", p.index},
      {"subindex", p.subindex},
      {"name", p.name},
      {"objectCode", p.objectCode},
      {"dataType", p.dataType},
      {"dataTypeName", mm::comm::objectDataTypeName(p.dataType)},
      {"bitLength", p.bitLength},
      {"access", p.access},
      {"origin", parameterOriginName(p.origin)},
      {"syncState", syncStateName(p.syncState)},
  };
  std::visit([&j](const auto& v) { j["value"] = v; }, p.currentValue());
  if (p.unit) {
    j["unit"] = *p.unit;
  }
  auto emitValue = [&j](const char* key, const std::optional<DeviceParameterValue>& v) {
    if (v) {
      std::visit([&j, key](const auto& x) { j[key] = x; }, *v);
    }
  };
  emitValue("defaultValue", p.defaultValue);
  emitValue("minValue", p.minValue);
  emitValue("maxValue", p.maxValue);
}

}  // namespace mm::node
