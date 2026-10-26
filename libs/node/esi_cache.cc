#include "node/esi_cache.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/platform.h"  // userCacheDir
#include "etg/esi.h"
#include "etg/esi_unit.h"
#include "node/firmware_version.h"

namespace mm::node {

namespace fs = std::filesystem;

EsiCache::EsiCache(EsiCacheConfig config) : config_(std::move(config)) {}

void EsiCache::configure(EsiCacheConfig config) {
  std::lock_guard<std::mutex> lock(mutex_);
  config_ = std::move(config);
  files_.clear();
}

std::string EsiCache::fileName(uint32_t vendorId, std::string_view firmwareVersion) {
  return std::format("esi-{:08x}-{}.xml", vendorId, sanitizeFirmwareVersion(firmwareVersion));
}

bool EsiCache::enabled() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return config_.enabled;
}

fs::path EsiCache::resolveDir() const {
  if (!config_.directory.empty()) {
    return fs::path(config_.directory);
  }
  return mm::core::userCacheDir() / "esis";
}

std::shared_ptr<const mm::etg::EsiEntryTable> EsiCache::find(
    uint32_t vendorId, uint32_t productCode, std::string_view firmwareVersion) const {
  if (firmwareVersion.empty()) {
    return nullptr;
  }
  const std::string name = fileName(vendorId, firmwareVersion);

  std::lock_guard<std::mutex> lock(mutex_);
  if (!config_.enabled) {
    return nullptr;
  }
  const fs::path path = resolveDir() / name;
  std::error_code ec;
  const auto modified = fs::last_write_time(path, ec);
  const auto size = ec ? 0 : fs::file_size(path, ec);
  if (ec) {
    spdlog::debug("No ESI for firmware {}: {} is not readable: {}", firmwareVersion, path.string(),
                  ec.message());
    files_.erase(name);
    return nullptr;
  }

  auto it = files_.find(name);
  if (it == files_.end() || it->second.modified != modified || it->second.size != size) {
    it = files_.insert_or_assign(name, load(path, modified, size)).first;
  }
  const auto& tables = it->second.tables;
  if (const auto table = tables.find(productCode); table != tables.end()) {
    return table->second;
  }
  if (!tables.empty()) {
    spdlog::info("The ESI {} does not describe product code 0x{:08X}", path.string(), productCode);
  }
  return nullptr;
}

std::expected<fs::path, std::string> EsiCache::store(std::string_view xml,
                                                     std::string_view firmwareVersion) const {
  if (firmwareVersion.empty()) {
    return std::unexpected("the firmware version is empty");
  }
  fs::path dir;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_.enabled) {
      return std::unexpected("the ESI cache is disabled");
    }
    dir = resolveDir();
  }
  const auto file = mm::etg::parseEsi(xml);
  if (!file) {
    return std::unexpected(std::format("the ESI does not parse: {}", file.error()));
  }
  const fs::path path = dir / fileName(file->vendor.id, firmwareVersion);
  const fs::path temporary = fs::path(path).concat(".tmp");
  std::error_code ec;
  fs::create_directories(dir, ec);
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (ec || !out || !out.write(xml.data(), static_cast<std::streamsize>(xml.size()))) {
      return std::unexpected(std::format("could not write '{}'", temporary.string()));
    }
  }
  fs::rename(temporary, path, ec);
  if (ec) {
    fs::remove(temporary, ec);
    return std::unexpected(std::format("could not write '{}'", path.string()));
  }
  return path;
}

EsiCache::LoadedFile EsiCache::load(const fs::path& path, fs::file_time_type modified,
                                    std::uintmax_t size) {
  LoadedFile loaded{.modified = modified, .size = size, .tables = {}};
  const auto file = mm::etg::parseEsiFile(path);
  if (!file) {
    spdlog::warn("The ESI {} is not used, because it does not parse: {}", path.string(),
                 file.error());
    return loaded;
  }
  for (const auto& device : file->devices) {
    if (!device.productCode || loaded.tables.contains(*device.productCode)) {
      continue;
    }
    // One file can describe a product at several revisions. The revision the master reads does not
    // say which one applies, so the newest description is taken, as findEsiDeviceByProductCode does
    // with no revision given.
    const auto* newest = mm::etg::findEsiDeviceByProductCode(*file, *device.productCode);
    auto table = mm::etg::buildDeviceEntries(*file, *newest);
    if (!table) {
      spdlog::warn("The ESI {} gives no dictionary for product code 0x{:08X}: {}", path.string(),
                   *device.productCode, table.error());
      continue;
    }
    spdlog::debug("Read the ESI {} for product code 0x{:08X}: {} entries, {} warnings",
                  path.string(), *device.productCode, table->entries.size(),
                  table->warnings.size());
    loaded.tables.emplace(*device.productCode,
                          std::make_shared<const mm::etg::EsiEntryTable>(std::move(*table)));
  }
  return loaded;
}

namespace {

// A bound of the ESI as a value of the parameter's own type, or nothing when the bytes do not fit
// it. The ESI pads a short value to its declared width with the sign rules of libs/etg.
std::optional<DeviceParameterValue> decodeBound(uint16_t dataType,
                                                const std::optional<std::vector<uint8_t>>& bytes) {
  if (!bytes) {
    return std::nullopt;
  }
  auto decoded = decodeSdoBytes(dataType, *bytes);
  if (!decoded) {
    return std::nullopt;
  }
  return std::move(*decoded);
}

}  // namespace

std::vector<DeviceParameter> joinEsi(std::vector<DeviceParameter> parameters,
                                     const mm::etg::EsiEntryTable* esi, std::string_view esiFile) {
  std::unordered_map<uint32_t, const mm::etg::EsiEntry*> entries;
  if (esi != nullptr) {
    entries.reserve(esi->entries.size());
    std::transform(esi->entries.begin(), esi->entries.end(), std::inserter(entries, entries.end()),
                   [](const mm::etg::EsiEntry& entry) { return std::pair{entry.key(), &entry}; });
  }
  for (auto& p : parameters) {
    if (const auto it = entries.find(p.key()); it != entries.end()) {
      const mm::etg::EsiEntry& entry = *it->second;
      if (!p.unit && entry.unit) {
        p.unit = entry.unit;
        p.unitSymbol = entry.unitSymbol;
      }
      if (!p.defaultValue) {
        p.defaultValue = decodeBound(p.dataType, entry.defaultData);
      }
      if (!p.minValue) {
        p.minValue = decodeBound(p.dataType, entry.minData);
      }
      if (!p.maxValue) {
        p.maxValue = decodeBound(p.dataType, entry.maxData);
      }
      p.description = entry.description;
      p.options.clear();
      for (const auto& option : entry.options) {
        p.options.push_back({.value = option.value, .label = option.label});
      }
      p.esiFile = esiFile;
    }
    if (p.unit && p.unitSymbol.empty()) {
      p.unitSymbol = mm::etg::esiUnitSymbol(*p.unit);
    }
  }
  return parameters;
}

}  // namespace mm::node
