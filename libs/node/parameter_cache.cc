#include "node/parameter_cache.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "core/platform.h"  // userCacheDir — the shared root this cache lives under

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
  p.syncState = SyncState::Unknown;  // the cache holds definitions only, never values
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

/// Parses an id of the form "<vendor>-<product>-<revision>" (three hex fields, the inverse of
/// @c makeId) into its identity triple. Returns nullopt on any malformed or extra/missing field.
std::optional<std::array<uint32_t, 3>> parseId(std::string_view id) {
  std::array<uint32_t, 3> parts{0, 0, 0};
  size_t field = 0;
  size_t start = 0;
  for (size_t end = 0; end <= id.size(); ++end) {
    if (end != id.size() && id[end] != '-') {
      continue;
    }
    if (field >= parts.size()) {
      return std::nullopt;  // too many fields
    }
    const std::string_view token = id.substr(start, end - start);
    auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), parts[field], 16);
    if (ec != std::errc() || ptr != token.data() + token.size()) {
      return std::nullopt;
    }
    ++field;
    start = end + 1;
  }
  if (field != parts.size()) {
    return std::nullopt;  // too few fields
  }
  return parts;
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
  // A subdirectory of the standard per-user cache directory, so the cache survives restarts (the
  // main win) rather than living in temp. Sharing that root with the user cache is deliberate: the
  // `/api/user-cache` browser then lists these files too, which is the honest picture of what
  // Motion Master has written to the machine.
  return mm::core::userCacheDir() / "parameters";
}

std::string ParameterCache::makeId(uint32_t vendorId, uint32_t productCode,
                                   uint32_t revisionNumber) {
  return std::format("{:08x}-{:08x}-{:08x}", vendorId, productCode, revisionNumber);
}

fs::path ParameterCache::pathFor(uint32_t vendorId, uint32_t productCode,
                                 uint32_t revisionNumber) const {
  return resolveDir() /
         std::format("parameters-{}.json", makeId(vendorId, productCode, revisionNumber));
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

std::vector<ParameterCache::CacheEntry> ParameterCache::list() const {
  std::vector<CacheEntry> entries;
  std::error_code ec;
  const fs::path dir = resolveDir();
  if (!fs::is_directory(dir, ec)) {
    return entries;  // No cache directory yet — nothing cached.
  }
  for (const auto& dirEntry : fs::directory_iterator(dir, ec)) {
    if (!dirEntry.is_regular_file() || dirEntry.path().extension() != ".json") {
      continue;
    }
    try {
      std::ifstream in(dirEntry.path(), std::ios::binary);
      if (!in) {
        continue;
      }
      const nlohmann::json doc = nlohmann::json::parse(in);
      if (doc.value("formatVersion", -1) != kFormatVersion) {
        continue;  // A different (incompatible) format — not ours to report.
      }
      const uint32_t vendorId = doc.value("vendorId", uint32_t{0});
      const uint32_t productCode = doc.value("productCode", uint32_t{0});
      const uint32_t revisionNumber = doc.value("revisionNumber", uint32_t{0});
      entries.push_back(CacheEntry{
          .id = makeId(vendorId, productCode, revisionNumber),
          .vendorId = vendorId,
          .productCode = productCode,
          .revisionNumber = revisionNumber,
          .parameterCount =
              static_cast<uint32_t>(doc.value("parameters", nlohmann::json::array()).size()),
          .sizeBytes = dirEntry.file_size(ec),
      });
    } catch (const std::exception& e) {
      spdlog::warn("Parameter cache: skipping unreadable file {}: {}", dirEntry.path().string(),
                   e.what());
    }
  }
  return entries;
}

std::expected<std::vector<uint8_t>, std::string> ParameterCache::readRaw(
    std::string_view id) const {
  const auto parts = parseId(id);
  if (!parts) {
    return std::unexpected(std::format("invalid cache id '{}'", id));
  }
  const fs::path path = pathFor((*parts)[0], (*parts)[1], (*parts)[2]);
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::unexpected(std::format("no cache file '{}'", id));
  }
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::expected<void, std::string> ParameterCache::remove(std::string_view id) const {
  const auto parts = parseId(id);
  if (!parts) {
    return std::unexpected(std::format("invalid cache id '{}'", id));
  }
  const fs::path path = pathFor((*parts)[0], (*parts)[1], (*parts)[2]);
  std::error_code ec;
  if (!fs::remove(path, ec)) {
    return std::unexpected(ec ? ec.message() : std::format("no cache file '{}'", id));
  }
  spdlog::info("Parameter cache: removed {}", path.string());
  return {};
}

void to_json(nlohmann::json& j, const ParameterCache::CacheEntry& e) {
  j = nlohmann::json{
      {"id", e.id},
      {"vendorId", e.vendorId},
      {"productCode", e.productCode},
      {"revisionNumber", e.revisionNumber},
      {"parameterCount", e.parameterCount},
      {"sizeBytes", e.sizeBytes},
  };
}

}  // namespace mm::node
