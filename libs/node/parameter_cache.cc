#include "node/parameter_cache.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace mm::node {

namespace {

namespace fs = std::filesystem;

/// Encodes a typed parameter value as a native JSON scalar (number / string / byte array), so the
/// cache file stays human-readable. The active variant alternative maps directly: integers and
/// floats become JSON numbers (64-bit integers stay exact), strings become JSON strings, and raw
/// byte blobs become arrays of integers.
nlohmann::json valueToJson(const DeviceParameterValue& value) {
  return std::visit([](const auto& v) -> nlohmann::json { return v; }, value);
}

/// Reconstructs a typed parameter value from its JSON scalar, using @p prototype (a
/// type-appropriate default for the parameter's data type) to select the variant alternative — so
/// the round-trip preserves the exact type, including 64-bit integers. Throws (nlohmann) if @p j
/// does not hold the prototype's type; the caller treats that as a corrupt-file miss.
DeviceParameterValue valueFromJson(const DeviceParameterValue& prototype, const nlohmann::json& j) {
  return std::visit(
      [&](const auto& sample) -> DeviceParameterValue {
        using T = std::decay_t<decltype(sample)>;
        return DeviceParameterValue{j.get<T>()};
      },
      prototype);
}

/// Serialises one parameter's *definition* — never its live value or sync state.
nlohmann::json definitionToJson(const DeviceParameter& p) {
  nlohmann::json j{
      {"index", p.index},           {"subindex", p.subindex}, {"name", p.name},
      {"objectCode", p.objectCode}, {"dataType", p.dataType}, {"bitLength", p.bitLength},
      {"access", p.access},
  };
  if (p.unit) {
    j["unit"] = *p.unit;
  }
  if (p.defaultValue) {
    j["defaultValue"] = valueToJson(*p.defaultValue);
  }
  if (p.minValue) {
    j["minValue"] = valueToJson(*p.minValue);
  }
  if (p.maxValue) {
    j["maxValue"] = valueToJson(*p.maxValue);
  }
  return j;
}

/// Rebuilds a parameter from its cached definition. @c value is seeded to the type default and
/// @c syncState left Unknown — the device's live value is read by the caller, never from the file.
DeviceParameter definitionFromJson(const nlohmann::json& j) {
  DeviceParameter p;
  p.index = j.at("index").get<uint16_t>();
  p.subindex = j.at("subindex").get<uint8_t>();
  p.name = j.at("name").get<std::string>();
  p.objectCode = j.at("objectCode").get<uint16_t>();
  p.dataType = j.at("dataType").get<uint16_t>();
  p.bitLength = j.at("bitLength").get<uint16_t>();
  p.access = j.at("access").get<uint16_t>();
  p.value = defaultValueForDataType(p.dataType);
  p.syncState = SyncState::Unknown;
  if (auto it = j.find("unit"); it != j.end() && !it->is_null()) {
    p.unit = it->get<uint32_t>();
  }
  const DeviceParameterValue prototype = defaultValueForDataType(p.dataType);
  if (auto it = j.find("defaultValue"); it != j.end() && !it->is_null()) {
    p.defaultValue = valueFromJson(prototype, *it);
  }
  if (auto it = j.find("minValue"); it != j.end() && !it->is_null()) {
    p.minValue = valueFromJson(prototype, *it);
  }
  if (auto it = j.find("maxValue"); it != j.end() && !it->is_null()) {
    p.maxValue = valueFromJson(prototype, *it);
  }
  return p;
}

std::optional<std::string> envVar(const char* name) {
  const char* v = std::getenv(name);
  if (v == nullptr || *v == '\0') {
    return std::nullopt;
  }
  return std::string(v);
}

}  // namespace

ParameterCache::ParameterCache(ParameterCacheConfig config) : config_(std::move(config)) {}

void ParameterCache::configure(ParameterCacheConfig config) { config_ = std::move(config); }

bool ParameterCache::enabledForVendor(uint32_t vendorId) const {
  if (!config_.enabled) {
    return false;
  }
  return config_.cacheAllVendors || vendorId == kSynapticonVendorId;
}

fs::path ParameterCache::resolveDir() const {
  if (!config_.directory.empty()) {
    return fs::path(config_.directory);
  }
  // A standard per-user cache directory, so the cache survives restarts (the main win) rather than
  // living in temp. Falls back to the OS temp directory when the platform's home/cache env is
  // unset.
#if defined(_WIN32)
  if (auto p = envVar("LOCALAPPDATA")) {
    return fs::path(*p) / "motion-master" / "parameters";
  }
#elif defined(__APPLE__)
  if (auto p = envVar("HOME")) {
    return fs::path(*p) / "Library" / "Caches" / "motion-master" / "parameters";
  }
#else
  if (auto p = envVar("XDG_CACHE_HOME")) {
    return fs::path(*p) / "motion-master" / "parameters";
  }
  if (auto p = envVar("HOME")) {
    return fs::path(*p) / ".cache" / "motion-master" / "parameters";
  }
#endif
  std::error_code ec;
  return fs::temp_directory_path(ec) / "motion-master" / "parameters";
}

fs::path ParameterCache::pathFor(uint32_t vendorId, uint32_t productCode,
                                 uint32_t revisionNumber) const {
  return resolveDir() /
         std::format("parameters-{:08x}-{:08x}-{:08x}.json", vendorId, productCode, revisionNumber);
}

std::optional<std::vector<DeviceParameter>> ParameterCache::load(uint32_t vendorId,
                                                                 uint32_t productCode,
                                                                 uint32_t revisionNumber) const {
  if (!enabledForVendor(vendorId)) {
    return std::nullopt;
  }
  const fs::path path = pathFor(vendorId, productCode, revisionNumber);
  try {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      return std::nullopt;  // No cache file yet — a plain miss.
    }
    const nlohmann::json doc = nlohmann::json::parse(in);
    // Validate the file describes this exact identity and a format we understand; anything else is
    // treated as a miss so the caller re-enumerates and overwrites it.
    if (doc.value("formatVersion", -1) != kFormatVersion ||
        doc.value("vendorId", uint32_t{0}) != vendorId ||
        doc.value("productCode", uint32_t{0}) != productCode ||
        doc.value("revisionNumber", uint32_t{0}) != revisionNumber) {
      return std::nullopt;
    }
    const auto& entries = doc.at("parameters");
    std::vector<DeviceParameter> parameters;
    parameters.reserve(entries.size());
    std::transform(entries.begin(), entries.end(), std::back_inserter(parameters),
                   [](const nlohmann::json& entry) { return definitionFromJson(entry); });
    spdlog::info("Parameter cache: loaded {} definitions for {:#010x}/{:#010x}/{:#010x} from {}",
                 parameters.size(), vendorId, productCode, revisionNumber, path.string());
    return parameters;
  } catch (const std::exception& e) {
    // Corrupt / partial / unreadable file: log and miss so the device is re-enumerated.
    spdlog::warn("Parameter cache: ignoring unreadable file {}: {}", path.string(), e.what());
    return std::nullopt;
  }
}

void ParameterCache::store(uint32_t vendorId, uint32_t productCode, uint32_t revisionNumber,
                           const std::vector<DeviceParameter>& parameters) const {
  if (!enabledForVendor(vendorId)) {
    return;
  }
  const fs::path path = pathFor(vendorId, productCode, revisionNumber);
  try {
    fs::create_directories(path.parent_path());
    nlohmann::json doc{
        {"formatVersion", kFormatVersion},
        {"vendorId", vendorId},
        {"productCode", productCode},
        {"revisionNumber", revisionNumber},
        {"parameters", nlohmann::json::array()},
    };
    for (const auto& p : parameters) {
      doc["parameters"].push_back(definitionToJson(p));
    }
    // Atomic install: write a sibling temp file then rename over the target, so a reader never sees
    // a half-written file (mirrors the cert installer). dump(2) keeps the file human-readable.
    const fs::path tmp = path.string() + ".tmp";
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      out << doc.dump(2) << '\n';
    }
    fs::rename(tmp, path);
    spdlog::debug("Parameter cache: wrote {} definitions for {:#010x}/{:#010x}/{:#010x} to {}",
                  parameters.size(), vendorId, productCode, revisionNumber, path.string());
  } catch (const std::exception& e) {
    spdlog::warn("Parameter cache: failed to write {}: {}", path.string(), e.what());
  }
}

}  // namespace mm::node
