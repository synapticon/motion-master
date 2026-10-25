#include "node/esi_cache.h"

#include <spdlog/spdlog.h>

#include <format>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#include "core/platform.h"  // userCacheDir
#include "etg/esi.h"
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

}  // namespace mm::node
