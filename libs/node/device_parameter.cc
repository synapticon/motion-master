#include "node/device_parameter.h"

#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
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
    case ObjectDataType::VISIBLE_STRING:
    case ObjectDataType::UNICODE_STRING:
      return DeviceParameterValue{
          std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size())};
    default:
      // Unknown / composite type — return raw bytes so the caller can inspect.
      return DeviceParameterValue{std::vector<uint8_t>(bytes.begin(), bytes.end())};
  }
}

void to_json(nlohmann::json& j, const DeviceParameter& p) {
  j = nlohmann::json{
      {"index", p.index},         {"subindex", p.subindex},
      {"name", p.name},           {"objectCode", p.objectCode},
      {"dataType", p.dataType},   {"dataTypeName", mm::comm::objectDataTypeName(p.dataType)},
      {"bitLength", p.bitLength}, {"access", p.access},
  };
  std::visit([&j](const auto& v) { j["value"] = v; }, p.value);
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
