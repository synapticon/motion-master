#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "node/device_parameter.h"
#include "node/synapticon.h"  // kSynapticonVendorId

namespace mm::node {

// The cache is enabled by default only for @c kSynapticonVendorId: for this vendor the CoE object
// dictionary is uniquely determined by @c (productCode, revisionNumber) — Synapticon bumps the
// revision whenever the dictionary changes, so an on-disk cache keyed on identity alone can never
// serve definitions that do not match the device. That guarantee does not hold for arbitrary
// third-party vendors (see @c ParameterCacheConfig::cacheAllVendors).

/// @brief Policy and location for the on-disk parameter cache.
///
/// Mirrors the JSONC @c parameterCache block (mapped into this struct in @c main.cc).
struct ParameterCacheConfig {
  bool cacheAllVendors =
      false;              ///< false: cache Synapticon (0x22D2) only; true: cache every vendor.
  std::string directory;  ///< "" = a standard per-user cache directory (see @c resolveDir).
  bool enabled = true;    ///< Master switch for the whole cache (false disables it entirely).
};

/// @brief On-disk cache of CoE parameter *definitions*, keyed by device identity.
///
/// Enumerating a drive's object dictionary over the SDO-Information service is hundreds of mailbox
/// round-trips and takes seconds; the result — the set of @c DeviceParameter definitions (index,
/// subindex, name, data type, bit length, access, unit, and the static default/min/max bounds) — is
/// identical for every device of the same @c (vendorId, productCode, revisionNumber) and, for a
/// vendor with stable revision discipline, stays valid across power cycles. This cache persists
/// those definitions to a human-readable JSON file per identity so a later scan of the same
/// hardware skips the round-trips.
///
/// Only the **definition** is cached — never the live @c value or @c syncState. On load each
/// parameter's @c value is reset to the type-appropriate default and read live from the device, so
/// a stale cache can at most cost a re-enumeration, never show a stale value as if current. The
/// cached file is a definition/schema snapshot, intended to be readable and (in future) enriched
/// with ESI-derived metadata such as descriptions and enum option labels.
///
/// All operations are best-effort and never throw: a missing, unreadable, corrupt, or
/// wrong-format-version file is a miss (the caller falls back to a live enumeration); a write
/// failure is logged, not propagated. Owned by @c DeviceManager (it outlives the @c Device set,
/// which is rebuilt on every scan) and handed to each @c Device by pointer.
class ParameterCache {
 public:
  /// @brief Bumped when the on-disk field set / meaning changes in a way that invalidates older
  ///        files. Adding a new optional field does not require a bump (missing keys default and
  ///        unknown keys are ignored); changing a field's meaning does.
  static constexpr int kFormatVersion = 1;

  ParameterCache() = default;
  explicit ParameterCache(ParameterCacheConfig config);

  /// @brief Replaces the active policy/location. Called by @c DeviceManager::init.
  void configure(ParameterCacheConfig config);

  /// @brief Whether the cache is active for @p vendorId under the current policy.
  ///
  /// True when the master switch is on and either @c cacheAllVendors is set or @p vendorId is
  /// Synapticon's. The single gate consulted by both @c load and @c store.
  bool enabledForVendor(uint32_t vendorId) const;

  /// @brief Loads the cached parameter definitions for an identity.
  ///
  /// Each returned @c DeviceParameter has its definition fields populated, its @c value set to the
  /// type-appropriate default, and its @c syncState left @c Unknown (the caller reads live values).
  ///
  /// @return The cached definitions on a hit, or @c std::nullopt when the cache is disabled for the
  ///         vendor, no file exists, or the file is unreadable / corrupt / for a different identity
  ///         or format version (all treated as a miss).
  std::optional<std::vector<DeviceParameter>> load(uint32_t vendorId, uint32_t productCode,
                                                   uint32_t revisionNumber) const;

  /// @brief Persists parameter definitions for an identity (atomic temp-file + rename).
  ///
  /// The live @c value and @c syncState of each parameter are intentionally not written. No-op when
  /// the cache is disabled for the vendor. Best-effort: directory-creation or write failures are
  /// logged at warn level and otherwise ignored.
  void store(uint32_t vendorId, uint32_t productCode, uint32_t revisionNumber,
             const std::vector<DeviceParameter>& parameters) const;

  /// @brief Summary of one cache file on disk, for the management UI.
  struct CacheEntry {
    std::string id;                ///< Opaque "<vendor>-<product>-<revision>" key (see @c makeId).
    uint32_t vendorId = 0;         ///< Vendor ID from the file header.
    uint32_t productCode = 0;      ///< Product code from the file header.
    uint32_t revisionNumber = 0;   ///< Revision number from the file header.
    uint32_t parameterCount = 0;   ///< Number of cached parameter definitions in the file.
    std::uintmax_t sizeBytes = 0;  ///< File size on disk, in bytes.
  };

  /// @brief Builds the opaque id a client uses to address one cache file, from an identity triple.
  ///        The single place the id encoding lives; @c list reports it and @c readRaw / @c remove
  ///        parse it back, so neither the HTTP layer nor the UI ever formats the key itself.
  static std::string makeId(uint32_t vendorId, uint32_t productCode, uint32_t revisionNumber);

  /// @brief Lists every valid cache file in the cache directory.
  ///
  /// Unlike @c load / @c store, listing/management ignores the enabled/vendor *policy* — you must
  /// be able to inspect and clean up files even with caching disabled or after turning it off.
  /// Files that do not parse or carry a different @c formatVersion are skipped. Returns an empty
  /// list when the directory does not exist.
  std::vector<CacheEntry> list() const;

  /// @brief Reads the raw JSON bytes of one cache file, addressed by its @c id (for download).
  ///
  /// Parses @p id and reads the file; policy-independent. The HTTP layer passes the path parameter
  /// straight through — the id encoding and its validation live here, not in the server.
  ///
  /// @return The file bytes, or an error string if @p id is malformed or the file does not exist.
  std::expected<std::vector<uint8_t>, std::string> readRaw(std::string_view id) const;

  /// @brief Deletes one cache file addressed by its @c id. Policy-independent.
  /// @return Void on success, or an error string if @p id is malformed or the file does not exist.
  std::expected<void, std::string> remove(std::string_view id) const;

 private:
  /// @brief The directory cache files live in: @c config_.directory if set, else a per-user cache
  ///        directory (@c $XDG_CACHE_HOME / @c ~/.cache on Linux, @c %LOCALAPPDATA% on Windows,
  ///        @c ~/Library/Caches on macOS), falling back to the OS temp directory.
  std::filesystem::path resolveDir() const;

  /// @brief Absolute path of the cache file for one identity
  ///        (@c parameters-<vendor>-<product>-<rev>.json).
  std::filesystem::path pathFor(uint32_t vendorId, uint32_t productCode,
                                uint32_t revisionNumber) const;

  ParameterCacheConfig config_;
};

/// @brief Serialises a CacheEntry to JSON (keys: vendorId, productCode, revisionNumber,
///        parameterCount, sizeBytes). Participates in nlohmann ADL.
void to_json(nlohmann::json& j, const ParameterCache::CacheEntry& e);

}  // namespace mm::node
