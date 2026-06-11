#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "node/device_parameter.h"

namespace mm::node {

/// @brief EtherCAT vendor ID of Synapticon (SOMANET drives).
///
/// For this vendor the CoE object dictionary is uniquely determined by @c (productCode,
/// @c revisionNumber): Synapticon bumps the revision whenever the dictionary changes, so an
/// on-disk cache keyed on identity alone can never serve definitions that do not match the device.
/// That guarantee does not hold for arbitrary third-party vendors, which is why the cache is
/// enabled by default only for this vendor (see @c ParameterCacheConfig::cacheAllVendors).
inline constexpr uint32_t kSynapticonVendorId = 0x000022D2;

/// @brief Policy and location for the on-disk parameter cache.
///
/// Mirrors the JSONC @c parameterCache block (mapped into this struct in @c main.cc).
struct ParameterCacheConfig {
  bool enabled = true;  ///< Master switch for the whole cache (false disables it entirely).
  bool cacheAllVendors =
      false;              ///< false: cache Synapticon (0x22D2) only; true: cache every vendor.
  std::string directory;  ///< "" = a standard per-user cache directory (see @c resolveDir).
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

}  // namespace mm::node
