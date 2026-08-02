#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mm::core {

/// @brief One file in the user cache, as reported by @c UserCache::list.
struct UserCacheFile {
  /// Path relative to the cache root, always with `/` separators (so a Windows listing reads the
  /// same as a Linux one and round-trips straight back into @c read / @c write / @c remove).
  std::string path;
  uint64_t size = 0;       ///< File size in bytes.
  int64_t modifiedMs = 0;  ///< Last-write time in milliseconds since the Unix epoch.
};

/// @brief Serialises a listing entry as it appears in `GET /api/user-cache`.
void to_json(nlohmann::json& j, const UserCacheFile& f);

/// @brief A flat, user-writable file store rooted at Motion Master's per-user cache directory.
///
/// This is deliberately a *dumb* store: it holds whatever the user puts in it under whatever
/// relative path they choose, and attaches no meaning to names or contents. It exists so data a
/// user supplies once can outlive a restart without a per-feature endpoint — the general problem
/// of "keep this file on the server", solved once, rather than a new upload/list/delete surface per
/// feature that needs one.
///
/// **Content-agnostic is a property of this class, not of the directory it serves.** Other
/// subsystems own subtrees under the same root and very much do parse their own files —
/// @c ParameterCache reads and writes `parameters/`, @c DeviceManager::dumpProcessData writes
/// `dumps/` — and more will follow. So @c list reports files this class never received through
/// @c write, and a caller must not assume every path it sees was put there by a user, nor that
/// deleting one is without consequence elsewhere.
///
/// **Every path is relative to the root and validated.** This matters because the HTTP API in front
/// of it is unauthenticated: @c resolve is the only thing standing between a caller and the rest of
/// the filesystem. It rejects absolute paths, drive letters, `.`/`..` and empty components, control
/// characters, components ending in a dot or space, and Windows device names, then confirms the
/// assembled path is still under the root. The last three are not decoration — each is a way to
/// spell `..` that an exact-match check waves through: `"..\0"` is truncated back to `..` by the C
/// string every OS path API takes, and Win32 strips trailing dots and spaces before resolving, so
/// `".. "` and `"..."` land on the parent too. The rules apply on every platform, so a
/// Windows-only escape cannot hide from a Linux test run.
///
/// One residual, by design: @c resolve is purely lexical and does not resolve symlinks, so a
/// symlink already inside the root would be followed by @c read. Nothing reachable through this
/// class can create one (there is no symlink operation), so placing it requires local filesystem
/// access — at which point the cache directory was never the boundary being defended.
///
/// All operations are non-throwing (@c std::error_code overloads of @c <filesystem>) and report
/// failure as @c std::expected. Thread-safe in the sense that every operation is a standalone
/// filesystem call on a @c const root; concurrent writes to the *same* path race as they would on
/// any filesystem, which is the caller's problem and not worth a lock here.
class UserCache {
 public:
  /// @brief Constructs a cache rooted at @p root (typically @c mm::core::userCacheDir()).
  /// The directory need not exist; it is created on the first successful @c write.
  explicit UserCache(std::filesystem::path root);

  /// @brief The cache root. Surfaced to users so they can find the files on disk themselves.
  const std::filesystem::path& root() const { return root_; }

  /// @brief Validates @p relPath and maps it to an absolute path under the root.
  ///
  /// The single gate every other operation goes through, exposed for testing and for callers that
  /// need the on-disk path. Purely lexical — it does not touch the filesystem, so it behaves the
  /// same for a path that does not exist yet (@c write) as for one that does.
  ///
  /// @param relPath Slash- or backslash-separated path relative to the root, e.g.
  /// `"configs/machine-a.json"`.
  /// @return The absolute path, or an error describing why the path was rejected.
  std::expected<std::filesystem::path, std::string> resolve(std::string_view relPath) const;

  /// @brief Lists every file under the root, recursively.
  ///
  /// Directories are not listed in their own right — only the files they contain, each with its
  /// full relative path — so a client renders one flat table and needs no tree walk. A root that
  /// does not exist yet is an empty listing, not an error (nothing has been uploaded).
  ///
  /// Entries are sorted by path so the listing is stable across calls and platforms (directory
  /// iteration order is not).
  std::expected<std::vector<UserCacheFile>, std::string> list() const;

  /// @brief Reads a file's full contents.
  /// @return The bytes, or an error when the path is invalid, absent, or unreadable.
  std::expected<std::vector<uint8_t>, std::string> read(std::string_view relPath) const;

  /// @brief Writes (creating or replacing) a file, creating parent directories as needed.
  ///
  /// The write is atomic from a reader's point of view: the bytes go to a temporary file beside the
  /// destination and are then renamed over it, so a concurrent @c read never observes a
  /// half-written file and a failed write never truncates the previous contents.
  std::expected<void, std::string> write(std::string_view relPath, std::span<const uint8_t> data);

  /// @brief Removes a file, or a directory and everything under it.
  ///
  /// After removing a file, any directories left empty between it and the root are pruned, so
  /// uploading and deleting a nested file leaves no husks behind in the listing's namespace.
  ///
  /// @return @c true when something was removed, @c false when the path did not exist (the caller
  ///         maps that to 404); an error only when the path is invalid or the removal failed.
  std::expected<bool, std::string> remove(std::string_view relPath);

 private:
  std::filesystem::path root_;
};

}  // namespace mm::core
