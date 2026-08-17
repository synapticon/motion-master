#include "core/user_cache.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace mm::core {

namespace {

/// Milliseconds since the Unix epoch for a filesystem timestamp.
///
/// `file_time_type`'s epoch is unspecified, and the standard conversion (`clock_cast` /
/// `file_clock::to_sys`) is not uniformly available across the three toolchains this ships on. The
/// portable expression of the same idea is to rebase through both clocks' "now": the difference
/// between the file's time and the file clock's now is a plain duration, which added to the system
/// clock's now yields a wall-clock instant. Sampling the two clocks a few nanoseconds apart skews
/// the result by that much — irrelevant for a "last modified" column.
int64_t toEpochMs(fs::file_time_type t) {
  const auto sys = std::chrono::system_clock::now() + (t - fs::file_time_type::clock::now());
  return std::chrono::duration_cast<std::chrono::milliseconds>(sys.time_since_epoch()).count();
}

/// Windows device names, which the OS resolves to a device rather than a file **in any directory**
/// — `CON`, `LPT1`, and `AUX.txt` all bypass the filesystem wherever they appear. Opening one can
/// block indefinitely (a serial port that never answers), which on an unauthenticated endpoint is a
/// free way to wedge a request thread. Matched against the component's stem, case-insensitively,
/// because Windows applies the rule before the extension: `CON.txt` *is* `CON`.
constexpr std::string_view kWindowsDeviceNames[] = {
    "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
    "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};

bool isControlCharacter(char c) {
  const auto byte = static_cast<unsigned char>(c);
  return byte < 0x20 || byte == 0x7F;
}

bool isWindowsDeviceName(std::string_view component) {
  const std::string_view stem = component.substr(0, component.find('.'));
  return std::ranges::any_of(kWindowsDeviceNames, [stem](std::string_view device) {
    return std::ranges::equal(stem, device, [](unsigned char a, unsigned char b) {
      return std::toupper(a) == std::toupper(b);
    });
  });
}

/// Splits a relative path on either separator, rejecting any component that could escape the root
/// or name something other than a plain file/directory. Both separators are accepted because the
/// path arrives from an HTTP URL (always `/`) or a hand-written config value (possibly `\` on
/// Windows), and normalising here keeps every downstream comparison single-form.
///
/// The checks are deliberately identical on every platform. A Windows-only rule applied only on
/// Windows would mean a path accepted by a Linux server and rejected by a Windows one — and, worse,
/// a Windows-specific escape that no Linux test could ever catch.
std::expected<std::vector<std::string>, std::string> splitRelative(std::string_view relPath) {
  if (relPath.empty()) {
    return std::unexpected("path is empty");
  }
  std::vector<std::string> parts;
  size_t start = 0;
  for (size_t end = 0; end <= relPath.size(); ++end) {
    if (end != relPath.size() && relPath[end] != '/' && relPath[end] != '\\') {
      continue;
    }
    const std::string_view part = relPath.substr(start, end - start);
    start = end + 1;
    if (part.empty()) {
      // Also catches a leading separator, i.e. an absolute path.
      return std::unexpected(
          "path has an empty component (leading, trailing or doubled separator)");
    }
    // Control characters, NUL above all. Every OS path API ultimately takes a C string, so a
    // component of "..\0" passes the ".." comparison below as a *three*-byte string and is then
    // truncated back to ".." at the syscall — the cache's parent directory, reached past every
    // lexical check. (Verified: before this guard, `resolve("..\0")` returned <root>/.. and
    // `remove` would have `remove_all`'d it.) Rejecting the whole control range also keeps CR/LF
    // out of any path that might later be echoed into a response header.
    if (std::ranges::any_of(part, isControlCharacter)) {
      return std::unexpected("path component contains a control character");
    }
    if (part == "." || part == "..") {
      return std::unexpected("path component \".\" or \"..\" is not allowed");
    }
    // Windows strips trailing dots and spaces from a component before resolving it, so ".. " and
    // "..." reach the filesystem as ".." — the same escape as above wearing a different hat, and
    // invisible to the exact-match check. A name ending in a dot or a space is pathological
    // anyway; refusing it outright is cheaper than modelling Win32's normalisation.
    if (part.ends_with('.') || part.ends_with(' ')) {
      return std::unexpected("path component may not end with '.' or a space");
    }
    if (isWindowsDeviceName(part)) {
      return std::unexpected("path component is a reserved device name");
    }
    if (part.find(':') != std::string_view::npos) {
      // A drive-relative path ("C:file") or an NTFS alternate data stream ("f.xml:hidden"); on
      // Windows either would resolve somewhere other than where the components say.
      return std::unexpected("path component may not contain ':'");
    }
    parts.emplace_back(part);
  }
  return parts;
}

/// Puts a configured root into the one spelling every comparison here assumes.
///
/// The root arrives from a config file, where a trailing separator (`"/var/lib/mm/"`) is an
/// entirely ordinary thing to write — but `lexically_normal` preserves it, leaving a path whose
/// final element is empty. That element matches no real component, so the containment check in
/// @c resolve would reject *every* path (blaming the request, not the config) and the prune walk in
/// @c remove would never recognise the root and could climb past it. Normalising once, here, fixes
/// both: @c filename() is empty exactly for a trailing separator, and @c parent_path() drops it.
fs::path normalizeRoot(fs::path root) {
  root = root.lexically_normal();
  if (!root.has_filename() && root.has_parent_path()) {
    root = root.parent_path();
  }
  return root;
}

}  // namespace

void to_json(nlohmann::json& j, const UserCacheFile& f) {
  j = nlohmann::json{
      {"path", f.path}, {"size", f.size}, {"modifiedMs", f.modifiedMs}, {"deletable", f.deletable}};
}

UserCache::UserCache(fs::path root) : root_(normalizeRoot(std::move(root))) {}

std::expected<fs::path, std::string> UserCache::resolve(std::string_view relPath) const {
  auto parts = splitRelative(relPath);
  if (!parts) {
    return std::unexpected(parts.error());
  }
  fs::path p = root_;
  for (const auto& part : *parts) {
    p /= part;
  }
  // The component checks above already make an escape impossible; this confirms it on the assembled
  // path rather than trusting that reasoning, since a miss here is a read/write outside the cache.
  // Compared component-wise, not as strings: a string prefix test would accept a sibling directory
  // whose name merely starts with the root's ("motion-master-elsewhere"). root_ is already
  // normalised by the constructor, so only the assembled path needs it here.
  fs::path normalized = p.lexically_normal();
  const auto rootEnd =
      std::mismatch(root_.begin(), root_.end(), normalized.begin(), normalized.end()).first;
  if (rootEnd != root_.end()) {
    return std::unexpected("path escapes the cache directory");
  }
  return normalized;
}

std::expected<std::vector<UserCacheFile>, std::string> UserCache::list() const {
  std::vector<UserCacheFile> files;
  std::error_code ec;
  if (!fs::exists(root_, ec)) {
    return files;  // Nothing uploaded yet — an empty cache, not a failure.
  }
  fs::recursive_directory_iterator it{root_, fs::directory_options::skip_permission_denied, ec};
  if (ec) {
    return std::unexpected("failed to list " + root_.string() + ": " + ec.message());
  }
  // Stepped with the error_code overload, never a range-for. The throwing `operator++` is the one
  // place this class could raise, and it runs on the HTTP event-loop thread, where an escaping
  // exception takes down the whole server — not just the request. `skip_permission_denied` handles
  // the common cause; this covers the rest (a directory unlinked mid-walk, an I/O error).
  const fs::recursive_directory_iterator end;
  while (it != end) {
    std::error_code entryEc;
    const fs::directory_entry& entry = *it;
    if (entry.is_regular_file(entryEc) && !entryEc) {
      UserCacheFile file;
      file.path = entry.path().lexically_relative(root_).generic_string();
      file.size = entry.file_size(entryEc);
      if (entryEc) {
        file.size = 0;
      }
      const auto written = entry.last_write_time(entryEc);
      file.modifiedMs = entryEc ? 0 : toEpochMs(written);
      file.deletable = !isRetained(file.path);
      files.push_back(std::move(file));
    }
    // Anything not a readable regular file simply contributes no row: directories carry no entry of
    // their own, and an entry that cannot be stat'd is skipped rather than failing the listing.
    it.increment(ec);
    if (ec) {
      return std::unexpected("failed while listing " + root_.string() + ": " + ec.message());
    }
  }
  std::ranges::sort(files, {}, &UserCacheFile::path);
  return files;
}

std::expected<std::vector<uint8_t>, std::string> UserCache::read(std::string_view relPath) const {
  auto path = resolve(relPath);
  if (!path) {
    return std::unexpected(path.error());
  }
  std::error_code ec;
  if (!fs::is_regular_file(*path, ec)) {
    return std::unexpected("no such file: " + std::string(relPath));
  }
  const auto size = fs::file_size(*path, ec);
  if (ec) {
    return std::unexpected("failed to stat " + std::string(relPath) + ": " + ec.message());
  }
  std::ifstream in{*path, std::ios::binary};
  if (!in) {
    return std::unexpected("failed to open " + std::string(relPath));
  }
  std::vector<uint8_t> data(static_cast<size_t>(size));
  if (size > 0) {
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    if (in.gcount() != static_cast<std::streamsize>(size)) {
      return std::unexpected("short read on " + std::string(relPath));
    }
  }
  return data;
}

std::expected<void, std::string> UserCache::write(std::string_view relPath,
                                                  std::span<const uint8_t> data) {
  auto path = resolve(relPath);
  if (!path) {
    return std::unexpected(path.error());
  }
  std::error_code ec;
  fs::create_directories(path->parent_path(), ec);
  if (ec) {
    return std::unexpected("failed to create " + path->parent_path().string() + ": " +
                           ec.message());
  }
  // Write beside the destination, then rename over it: a reader sees either the old file or the
  // new one, never a partial write, and a failure part-way through leaves the previous contents.
  fs::path tmp = *path;
  tmp += ".mm-tmp";
  {
    std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
    if (!out) {
      return std::unexpected("failed to open " + tmp.string() + " for writing");
    }
    if (!data.empty()) {
      out.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
    }
    out.close();
    if (!out) {
      fs::remove(tmp, ec);
      return std::unexpected("failed to write " + std::string(relPath));
    }
  }
  fs::rename(tmp, *path, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return std::unexpected("failed to replace " + std::string(relPath) + ": " + ec.message());
  }
  return {};
}

void UserCache::retain(std::string relPath) { retained_.insert(std::move(relPath)); }

bool UserCache::isRetained(std::string_view relPath) const { return retained_.contains(relPath); }

std::expected<bool, std::string> UserCache::remove(std::string_view relPath) {
  auto path = resolve(relPath);
  if (!path) {
    return std::unexpected(path.error());
  }
  // Refused rather than attempted: this process holds the file open, and removing it would fail on
  // Windows and silently unlink-while-writing on Linux and macOS. See retain().
  if (isRetained(relPath)) {
    return std::unexpected(std::string(relPath) +
                           " is in use by Motion Master and cannot be deleted while it is running");
  }
  std::error_code ec;
  if (!fs::exists(*path, ec)) {
    return false;
  }
  if (fs::is_directory(*path, ec)) {
    fs::remove_all(*path, ec);
  } else {
    fs::remove(*path, ec);
  }
  if (ec) {
    return std::unexpected("failed to remove " + std::string(relPath) + ": " + ec.message());
  }
  // Prune directories the removal emptied, so a deleted "configs/site/a.json" does not leave
  // "configs/site" behind. fs::remove on a non-empty directory fails, which ends the walk.
  for (fs::path dir = path->parent_path(); dir != root_ && dir != dir.parent_path();
       dir = dir.parent_path()) {
    std::error_code pruneEc;
    if (!fs::remove(dir, pruneEc) || pruneEc) {
      break;
    }
  }
  return true;
}

}  // namespace mm::core
