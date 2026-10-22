#include "core/child_process.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#ifndef _WIN32

namespace mm::core {
namespace {

std::filesystem::path testDir() {
  const auto dir =
      std::filesystem::temp_directory_path() /
      ("mm-child-process-test-" +
       std::to_string(::testing::UnitTest::GetInstance()->current_test_info()->line()));
  std::filesystem::create_directories(dir);
  return dir;
}

std::filesystem::path script(const std::filesystem::path& dir, const std::string& body) {
  const auto path = dir / "child.sh";
  {
    std::ofstream out(path);
    out << "#!/bin/sh\n" << body;
  }
  std::filesystem::permissions(path, std::filesystem::perms::owner_all);
  return path;
}

TEST(ChildProcessTest, WaitReturnsTheExitCodeOfAChildThatExits) {
  const auto dir = testDir();
  const auto child = spawnChild(script(dir, "exit 3\n"), {}, dir / "child.log");
  ASSERT_TRUE(child.has_value());
  const auto code = waitChild(*child, std::chrono::seconds{5});
  ASSERT_TRUE(code.has_value());
  EXPECT_EQ(*code, 3);
}

TEST(ChildProcessTest, WaitGivesUpOnAChildThatRunsOn) {
  const auto dir = testDir();
  const auto child = spawnChild(script(dir, "exec sleep 30\n"), {}, dir / "child.log");
  ASSERT_TRUE(child.has_value());
  const auto code = waitChild(*child, std::chrono::milliseconds{200});
  EXPECT_FALSE(code.has_value());
  // The child is still there, so the caller stops it; a kill is what sleep needs.
  terminateChild(*child, std::chrono::milliseconds{200});
}

}  // namespace
}  // namespace mm::core

#endif
