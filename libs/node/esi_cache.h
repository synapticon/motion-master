#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "etg/esi_entry.h"

namespace mm::node {

// An ESI is found by its file name, never by its content. The file names no firmware version, and
// the revision it declares does not change with it: the SOMANET ESIs of v5.6.6 and v5.6.10 differ
// in bounds and descriptions and both declare revision 0x11000003. A Jasper drive with an older
// bootloader also reports revision 1 whatever application is installed. So the name carries the
// vendor ID and the firmware version, and the version must equal the device's 0x100A exactly,
// build metadata included. A development build gets no ESI until a person adds one under its name.

/// @brief Location of the ESI cache. Mirrors how @c ParameterCacheConfig names its directory.
struct EsiCacheConfig {
  std::string directory;  ///< "" = the @c esis subdirectory of the per-user cache directory.
};

/// @brief Finds the ESI of a device in a directory of ESI files, and keeps it parsed in memory.
///
/// A file is named @c esi-<vendor>-<version>.xml (see @c fileName). The name holds no product code,
/// because one ESI describes several products; @c find picks the device inside the file by product
/// code and ignores the revision.
///
/// Each file is parsed once and flattened once per device it describes, and the parsed document is
/// then dropped. Every device of one product on one firmware shares one table. A file is read again
/// when its modification time or size changes, so a file replaced through the user cache is used at
/// the next lookup.
///
/// Best-effort and never throws: a missing file, a file that does not parse, or a product the file
/// does not describe is a miss. Thread-safe; a lookup that parses a file holds the lock for the
/// parse.
class EsiCache {
 public:
  EsiCache() = default;
  explicit EsiCache(EsiCacheConfig config);

  /// @brief Replaces the location. Drops every table held, so the next lookup reads from the new
  ///        directory.
  void configure(EsiCacheConfig config);

  /// @brief The file name of the ESI for one vendor and firmware version:
  ///        @c esi-<vendor>-<version>.xml, the vendor as 8 lowercase hex digits and the version
  ///        reduced by @c sanitizeFirmwareVersion.
  static std::string fileName(uint32_t vendorId, std::string_view firmwareVersion);

  /// @brief The flattened dictionary of one device from its ESI.
  ///
  /// @param firmwareVersion  The device's 0x100A, trimmed. Empty is a miss.
  /// @return The table, shared with every other caller for the same file and product, or
  ///         @c nullptr on a miss.
  std::shared_ptr<const mm::etg::EsiEntryTable> find(uint32_t vendorId, uint32_t productCode,
                                                     std::string_view firmwareVersion) const;

 private:
  /// @brief One file as it was read: the stamp it was read at, and a table per product code. A file
  ///        that does not parse is kept with no tables, so it is not parsed and reported again
  ///        until it changes.
  struct LoadedFile {
    std::filesystem::file_time_type modified;
    std::uintmax_t size = 0;
    std::map<uint32_t, std::shared_ptr<const mm::etg::EsiEntryTable>> tables;
  };

  std::filesystem::path resolveDir() const;

  /// @brief Parses @p path and flattens every product it describes. Called with @c mutex_ held.
  static LoadedFile load(const std::filesystem::path& path,
                         std::filesystem::file_time_type modified, std::uintmax_t size);

  mutable std::mutex mutex_;  ///< Guards @c config_ and @c files_.
  EsiCacheConfig config_;
  mutable std::unordered_map<std::string, LoadedFile> files_;  ///< Keyed on the file name.
};

}  // namespace mm::node
