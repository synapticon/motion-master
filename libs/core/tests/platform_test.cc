#include "core/platform.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

// exeDir() is resolved three different ways (/proc/self/exe, _NSGetExecutablePath,
// GetModuleFileNameW) and is what the config auto-discovery and the default cert paths are built
// on, so a platform whose branch returns something relative or non-existent breaks startup rather
// than failing here. Assert the two properties every branch owes its callers.
TEST(PlatformTest, ExeDirIsAbsoluteAndExists) {
  const auto dir = mm::core::exeDir();
  EXPECT_TRUE(dir.is_absolute()) << dir.string();
  EXPECT_TRUE(std::filesystem::exists(dir)) << dir.string();
}

TEST(PlatformTest, ExeDirIsStableAcrossCalls) { EXPECT_EQ(mm::core::exeDir(), mm::core::exeDir()); }

// acquireSingleInstanceLock() is deliberately NOT tested here. It contends for a machine-global
// resource — an flock on <temp_dir>/motion-master.lock, or a named mutex on Windows — so any test
// of it fails whenever a real Motion Master happens to be running on the same machine, which is the
// normal state on a developer's box and can happen on a shared CI runner. It would pass in
// isolation and fail exactly when someone is using the application, which is worse than no test.
// Its behaviour is covered where it matters: the second instance is refused at startup, visibly.
