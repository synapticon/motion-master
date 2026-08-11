#include "node/hardware_description.h"

#include <expected>
#include <format>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mm::node {

namespace {

/// Reads a string field, tolerating both absence and a non-string value.
///
/// A number where a string belongs is a real hazard in this file rather than a hypothetical one:
/// every field is specified as a UTF-8 string, but @c id, @c version and @c keyId all look like
/// numbers, and a producer that writes @c "id": 9501 would otherwise abort the parse of an
/// otherwise perfectly good file. Taking the number's own text keeps the descriptor buildable —
/// though a @c version written as 1 rather than "01" cannot be recovered, which is the file's
/// problem and not something to paper over here.
std::string stringField(const nlohmann::json& object, std::string_view key) {
  const auto at = object.find(key);
  if (at == object.end()) {
    return {};
  }
  if (at->is_string()) {
    return at->get<std::string>();
  }
  if (at->is_number_integer()) {
    return std::to_string(at->get<int64_t>());
  }
  return {};
}

std::vector<HardwareComponent> components(const nlohmann::json& object) {
  std::vector<HardwareComponent> parsed;
  const auto at = object.find("components");
  if (at == object.end() || !at->is_array()) {
    return parsed;
  }
  parsed.reserve(at->size());
  for (const nlohmann::json& entry : *at) {
    if (!entry.is_object()) {
      continue;
    }
    parsed.push_back(HardwareComponent{
        .name = stringField(entry, "name"),
        .version = stringField(entry, "version"),
        .serialNumber = stringField(entry, "serialNumber"),
    });
  }
  return parsed;
}

HardwareProduct product(const nlohmann::json& object) {
  return HardwareProduct{
      .name = stringField(object, "name"),
      .imageId = stringField(object, "imageId"),
      .id = stringField(object, "id"),
      .version = stringField(object, "version"),
      .keyId = stringField(object, "keyId"),
      .serialNumber = stringField(object, "serialNumber"),
      .macAddress = stringField(object, "macAddress"),
      .components = components(object),
  };
}

}  // namespace

std::string HardwareProduct::buildDescriptor() const { return std::format("{}-{}", id, version); }

std::expected<HardwareDescription, std::string> parseHardwareDescription(std::string_view content) {
  // allow_exceptions = false per the no-exceptions mandate; comments are not enabled, because this
  // is a machine-written file rather than a hand-edited config and the specification says JSON.
  const nlohmann::json root = nlohmann::json::parse(content, nullptr, false);
  if (root.is_discarded()) {
    return std::unexpected("the hardware description is not valid JSON");
  }
  if (!root.is_object()) {
    return std::unexpected("the hardware description is not a JSON object");
  }

  const auto device = root.find("device");
  if (device == root.end() || !device->is_object()) {
    return std::unexpected(
        "the hardware description carries no 'device' object — every device stores its own file, "
        "so "
        "a description without one is not a hardware description");
  }

  HardwareDescription description;
  description.fileVersion = stringField(root, "fileVersion");
  description.device = product(*device);
  if (description.device.id.empty() || description.device.version.empty()) {
    return std::unexpected(
        "the hardware description's 'device' carries no 'id' or no 'version' — both are required, "
        "and together they are the build descriptor firmware is matched against");
  }

  if (const auto assembly = root.find("assembly");
      assembly != root.end() && assembly->is_object()) {
    HardwareProduct parsed = product(*assembly);
    // An assembly with no id or version cannot contribute a descriptor, and dropping it is better
    // than carrying a "-" that would match nothing: firmware then matches against the device, which
    // §4.1 says is compatible anyway. An assembly is optional, so an unusable one is an absent one.
    if (!parsed.id.empty() && !parsed.version.empty()) {
      description.assembly = std::move(parsed);
    }
  }
  return description;
}

void to_json(nlohmann::json& j, const HardwareComponent& component) {
  j = nlohmann::json{
      {"name", component.name},
      {"version", component.version},
      {"serialNumber", component.serialNumber},
  };
}

void to_json(nlohmann::json& j, const HardwareProduct& product) {
  j = nlohmann::json{
      {"name", product.name},
      {"imageId", product.imageId},
      {"id", product.id},
      {"version", product.version},
      {"keyId", product.keyId},
      {"serialNumber", product.serialNumber},
      {"macAddress", product.macAddress},
      {"components", product.components},
      {"buildDescriptor", product.buildDescriptor()},
  };
}

void to_json(nlohmann::json& j, const HardwareDescription& description) {
  j = nlohmann::json{
      {"fileVersion", description.fileVersion},
      {"device", description.device},
  };
  if (description.assembly) {
    j["assembly"] = *description.assembly;
  }
}

}  // namespace mm::node
