#include "core/user_cache.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/platform.h"

namespace fs = std::filesystem;
using mm::core::UserCache;
using namespace std::string_view_literals;  // NOLINT(build/namespaces) — "..\0"sv needs the length

namespace {

/// A cache rooted in a unique temporary directory, removed with the fixture. Named per test via
/// @c tag so a parallel ctest run never has two tests sharing a root.
class UserCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    root_ = fs::temp_directory_path() / ("mm-user-cache-test-" + std::string(info->name()));
    fs::remove_all(root_);
  }

  void TearDown() override { fs::remove_all(root_); }

  std::vector<uint8_t> bytes(std::string_view s) const { return {s.begin(), s.end()}; }

  fs::path root_;
};

TEST_F(UserCacheTest, WriteThenReadRoundTrips) {
  UserCache cache{root_};
  const auto data = bytes("<EtherCATInfo/>");
  ASSERT_TRUE(cache.write("esi/vendor/v5.6.6.xml", data)) << "write failed";

  auto read = cache.read("esi/vendor/v5.6.6.xml");
  ASSERT_TRUE(read) << read.error();
  EXPECT_EQ(*read, data);
  EXPECT_TRUE(fs::exists(root_ / "esi" / "vendor" / "v5.6.6.xml"));
}

TEST_F(UserCacheTest, WriteReplacesExistingFileAndLeavesNoTempBehind) {
  UserCache cache{root_};
  ASSERT_TRUE(cache.write("f.bin", bytes("old")));
  ASSERT_TRUE(cache.write("f.bin", bytes("new-and-longer")));

  auto read = cache.read("f.bin");
  ASSERT_TRUE(read) << read.error();
  EXPECT_EQ(*read, bytes("new-and-longer"));

  auto listed = cache.list();
  ASSERT_TRUE(listed) << listed.error();
  ASSERT_EQ(listed->size(), 1u);
  EXPECT_EQ((*listed)[0].path, "f.bin");
}

TEST_F(UserCacheTest, WriteAcceptsAnEmptyFile) {
  UserCache cache{root_};
  ASSERT_TRUE(cache.write("empty", {}));

  auto read = cache.read("empty");
  ASSERT_TRUE(read) << read.error();
  EXPECT_TRUE(read->empty());
}

TEST_F(UserCacheTest, ListIsFlatRecursiveAndSorted) {
  UserCache cache{root_};
  ASSERT_TRUE(cache.write("b.txt", bytes("bb")));
  ASSERT_TRUE(cache.write("a/deep/c.txt", bytes("ccc")));
  ASSERT_TRUE(cache.write("a/a.txt", bytes("a")));

  auto listed = cache.list();
  ASSERT_TRUE(listed) << listed.error();
  ASSERT_EQ(listed->size(), 3u);
  // Sorted by path, always with forward slashes — no directory rows of their own.
  EXPECT_EQ((*listed)[0].path, "a/a.txt");
  EXPECT_EQ((*listed)[1].path, "a/deep/c.txt");
  EXPECT_EQ((*listed)[2].path, "b.txt");
  EXPECT_EQ((*listed)[0].size, 1u);
  EXPECT_EQ((*listed)[1].size, 3u);
  EXPECT_EQ((*listed)[2].size, 2u);
  EXPECT_GT((*listed)[0].modifiedMs, 0);
}

TEST_F(UserCacheTest, ListOfAMissingRootIsEmptyNotAnError) {
  UserCache cache{root_};
  ASSERT_FALSE(fs::exists(root_));

  auto listed = cache.list();
  ASSERT_TRUE(listed) << listed.error();
  EXPECT_TRUE(listed->empty());
}

TEST_F(UserCacheTest, ReadOfAMissingFileFails) {
  UserCache cache{root_};
  EXPECT_FALSE(cache.read("nope.txt"));
}

TEST_F(UserCacheTest, ReadOfADirectoryFails) {
  UserCache cache{root_};
  ASSERT_TRUE(cache.write("dir/f.txt", bytes("x")));
  EXPECT_FALSE(cache.read("dir"));
}

TEST_F(UserCacheTest, RemoveDeletesAFileAndPrunesEmptiedDirectories) {
  UserCache cache{root_};
  ASSERT_TRUE(cache.write("a/deep/c.txt", bytes("ccc")));

  auto removed = cache.remove("a/deep/c.txt");
  ASSERT_TRUE(removed) << removed.error();
  EXPECT_TRUE(*removed);
  EXPECT_FALSE(fs::exists(root_ / "a" / "deep"));
  EXPECT_FALSE(fs::exists(root_ / "a"));
  EXPECT_TRUE(fs::exists(root_)) << "pruning must stop at the root";
}

TEST_F(UserCacheTest, RemoveKeepsDirectoriesThatStillHoldFiles) {
  UserCache cache{root_};
  ASSERT_TRUE(cache.write("a/one.txt", bytes("1")));
  ASSERT_TRUE(cache.write("a/two.txt", bytes("2")));

  ASSERT_TRUE(cache.remove("a/one.txt"));
  EXPECT_TRUE(fs::exists(root_ / "a" / "two.txt"));
}

TEST_F(UserCacheTest, RemoveDeletesADirectoryRecursively) {
  UserCache cache{root_};
  ASSERT_TRUE(cache.write("a/deep/c.txt", bytes("ccc")));
  ASSERT_TRUE(cache.write("a/b.txt", bytes("bb")));

  auto removed = cache.remove("a");
  ASSERT_TRUE(removed) << removed.error();
  EXPECT_TRUE(*removed);

  auto listed = cache.list();
  ASSERT_TRUE(listed) << listed.error();
  EXPECT_TRUE(listed->empty());
}

TEST_F(UserCacheTest, RemoveOfAMissingPathReportsFalseNotAnError) {
  UserCache cache{root_};
  auto removed = cache.remove("nope.txt");
  ASSERT_TRUE(removed) << removed.error();
  EXPECT_FALSE(*removed);
}

// The security-critical case: the HTTP API is unauthenticated, so path validation is all that keeps
// a caller inside the cache directory. Every spelling of "escape" must be refused, and refused
// before any filesystem call — resolve() is purely lexical.
//
// The three truncation cases are not hypothetical. Before the control-character and trailing
// dot/space guards, `resolve("..\0")` returned `<root>/..` — accepted by the ".." check as a
// three-byte component, then truncated back to ".." by the C string every OS path API takes. Since
// that path *is* a directory, `remove` would have reached `fs::remove_all` on the cache's parent
// (`~/.cache` on a default install). ".. " and "..." are the same escape via Win32's habit of
// stripping trailing dots and spaces before resolving a component.
TEST_F(UserCacheTest, ResolveRejectsEscapingAndMalformedPaths) {
  UserCache cache{root_};
  const std::string_view rejected[] = {
      ""sv,                     // empty
      ".."sv,                   // the root's parent
      "../outside.txt"sv,       // climb out
      "a/../../outside.txt"sv,  // climb out after descending
      "a/./b.txt"sv,            // "." component
      "/etc/passwd"sv,          // absolute (leading empty component)
      "\\windows\\host"sv,      // absolute, backslash spelling
      "a//b.txt"sv,             // doubled separator
      "a/"sv,                   // trailing separator
      "C:/Windows/host"sv,      // drive-qualified
      "f.xml:stream"sv,         // NTFS alternate data stream
      "..\0"sv,                 // NUL truncation back to ".."
      "a/..\0/b"sv,             // ... mid-path
      "safe\0.txt"sv,           // NUL anywhere at all
      ".. "sv,                  // Win32 strips the trailing space -> ".."
      "..."sv,                  // Win32 strips the trailing dots -> ".."
      "a."sv,                   // any trailing dot
      "a "sv,                   // any trailing space
      "a\r\nX-Injected: 1"sv,   // CR/LF, i.e. response-header splitting
      "a\tb"sv,                 // any other control character
      "CON"sv,                  // Windows device names, in any directory...
      "nul"sv,                  // ... case-insensitively ...
      "COM1"sv,                 //
      "AUX.txt"sv,              // ... and before the extension is considered
      "sub/LPT1"sv,             // ... at any depth
  };
  for (std::string_view path : rejected) {
    auto resolved = cache.resolve(path);
    EXPECT_FALSE(resolved) << "should have rejected: " << path;
    EXPECT_FALSE(cache.read(path)) << "read should have rejected: " << path;
    EXPECT_FALSE(cache.write(path, {})) << "write should have rejected: " << path;
    EXPECT_FALSE(cache.remove(path)) << "remove should have rejected: " << path;
  }
  // Nothing was created outside the (still absent) root by any of those attempts.
  EXPECT_FALSE(fs::exists(root_));
}

// The payoff of the case above, stated as the consequence rather than the mechanism: whatever a
// caller spells, nothing outside the cache root is ever touched.
TEST_F(UserCacheTest, RemoveCannotReachOutsideTheRoot) {
  const fs::path outside = root_.parent_path() / "mm-user-cache-test-victim";
  fs::create_directories(outside);
  fs::create_directories(root_);

  UserCache cache{root_};
  for (std::string_view path :
       {".."sv, "..\0"sv, ".. "sv, "..."sv, "../mm-user-cache-test-victim"sv}) {
    EXPECT_FALSE(cache.remove(path)) << "remove should have rejected: " << path;
    EXPECT_TRUE(fs::exists(outside)) << "removed something outside the root via: " << path;
    EXPECT_TRUE(fs::exists(root_)) << "removed the root itself via: " << path;
  }
  fs::remove_all(outside);
}

// A name that merely *contains* a device name, or is dotted/spaced in the middle, is an ordinary
// file — the guards must not be so blunt that they refuse legitimate names.
TEST_F(UserCacheTest, ResolveAcceptsNamesThatOnlyResembleRejectedOnes) {
  UserCache cache{root_};
  for (std::string_view path : {"CONFIG"sv, "console.log"sv, "my.CON"sv, "COM10"sv, "a..b"sv,
                                "v5.6.6.xml"sv, "a b/c d.txt"sv, ".hidden"sv}) {
    EXPECT_TRUE(cache.resolve(path)) << "should have accepted: " << path;
  }
}

TEST_F(UserCacheTest, ResolveAcceptsPlainNestedPathsAndNormalisesSeparators) {
  UserCache cache{root_};
  auto resolved = cache.resolve("esi/vendor/v5.6.6.xml");
  ASSERT_TRUE(resolved) << resolved.error();
  EXPECT_EQ(*resolved, (root_ / "esi" / "vendor" / "v5.6.6.xml").lexically_normal());

  // A backslash-separated path names the same file, so a Windows-style spelling round-trips.
  auto backslash = cache.resolve("esi\\vendor\\v5.6.6.xml");
  ASSERT_TRUE(backslash) << backslash.error();
  EXPECT_EQ(*backslash, *resolved);
}

// A sibling directory whose name merely starts with the root's must not be reachable — the reason
// the containment check compares path components rather than string prefixes.
TEST_F(UserCacheTest, ResolveIsNotFooledByASiblingWithAPrefixName) {
  UserCache cache{root_};
  EXPECT_FALSE(cache.resolve("../mm-user-cache-test-evil/f.txt"));
}

// A configured root is written by hand in a JSONC file, where a trailing separator is entirely
// ordinary. Before the constructor normalised it, `lexically_normal` kept the trailing separator as
// an empty final element, which matched no real path component — so the containment check rejected
// *every* path with "path escapes the cache directory", blaming the request instead of the config,
// and the store was silently unusable.
TEST_F(UserCacheTest, RootIsNormalisedSoATrailingSeparatorStillWorks) {
  UserCache cache{root_.string() + "/"};
  EXPECT_EQ(cache.root(), root_) << "the reported root should be the tidied one";

  ASSERT_TRUE(cache.write("a/f.txt", bytes("x")))
      << "a trailing-slash root must still accept paths";
  auto listed = cache.list();
  ASSERT_TRUE(listed) << listed.error();
  ASSERT_EQ(listed->size(), 1u);
  EXPECT_EQ((*listed)[0].path, "a/f.txt");

  // The prune walk must still recognise the root and stop there rather than climbing past it.
  ASSERT_TRUE(cache.remove("a/f.txt"));
  EXPECT_FALSE(fs::exists(root_ / "a"));
  EXPECT_TRUE(fs::exists(root_));
  EXPECT_TRUE(fs::exists(root_.parent_path()));
}

TEST_F(UserCacheTest, RootIsNormalisedSoInteriorDotsStillWork) {
  UserCache cache{root_ / "." / "sub" / ".."};
  EXPECT_EQ(cache.root(), root_);
  EXPECT_TRUE(cache.resolve("f.txt"));
}

TEST_F(UserCacheTest, RetainedPathIsRefusedByRemoveAndReportedInTheListing) {
  // The server's own log file: it is held open for the life of the process, so removing it fails on
  // Windows and silently unlinks-while-writing on Linux and macOS. Refusing is the only outcome
  // that is the same everywhere.
  UserCache cache{root_};
  ASSERT_TRUE(cache.write("logs/motion-master.log", bytes("line")));
  ASSERT_TRUE(cache.write("logs/motion-master.1.log", bytes("older")));
  cache.retain("logs/motion-master.log");

  EXPECT_TRUE(cache.isRetained("logs/motion-master.log"));
  EXPECT_FALSE(cache.isRetained("logs/motion-master.1.log"));

  auto removed = cache.remove("logs/motion-master.log");
  EXPECT_FALSE(removed.has_value());
  EXPECT_TRUE(fs::exists(root_ / "logs" / "motion-master.log")) << "the file must still be there";

  // A rotated sibling is closed and deletes normally — that is how a user reclaims the space.
  auto rotated = cache.remove("logs/motion-master.1.log");
  ASSERT_TRUE(rotated) << rotated.error();
  EXPECT_TRUE(*rotated);

  // The listing says which is which, so a client need not offer an action that would be refused.
  auto listed = cache.list();
  ASSERT_TRUE(listed) << listed.error();
  ASSERT_EQ(listed->size(), 1u);
  EXPECT_EQ((*listed)[0].path, "logs/motion-master.log");
  EXPECT_FALSE((*listed)[0].deletable);
}

TEST_F(UserCacheTest, UnretainedFilesAreDeletableByDefault) {
  UserCache cache{root_};
  ASSERT_TRUE(cache.write("notes.txt", bytes("hello")));
  auto listed = cache.list();
  ASSERT_TRUE(listed) << listed.error();
  ASSERT_EQ(listed->size(), 1u);
  EXPECT_TRUE((*listed)[0].deletable);
}

TEST(UserCacheDirTest, IsAbsoluteAndNamedForMotionMaster) {
  const auto dir = mm::core::userCacheDir();
  EXPECT_TRUE(dir.is_absolute()) << dir.string();
  EXPECT_EQ(dir.filename(), "motion-master");
}

}  // namespace
