#include "auto_tuning/process.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

// What these tests cover is the part that does not need the auto-tuning executable: the failures.
// A missing file, a child that dies at once, and a child that lives without serving are the three
// ways starting can go wrong, and each has to produce an error that says which one it was — the
// startup log line is all an operator gets.
//
// The success path needs the real executable, which is downloaded rather than built, so it is not
// available to a unit test. The HTTP integration tests exercise it.
namespace {

// A port nothing is expected to hold. A real auto-tuning listens on 63528, and using that here
// would let a test pass because somebody's server answered.
constexpr std::uint16_t kUnusedPort = 63897;

// Short, because these tests wait for it. Long enough that a loaded machine does not read a slow
// spawn as a timeout.
constexpr std::chrono::milliseconds kShortTimeout{500};

std::filesystem::path testDir() {
  const auto dir =
      std::filesystem::temp_directory_path() /
      ("mm-auto-tuning-test-" +
       std::to_string(::testing::UnitTest::GetInstance()->current_test_info()->line()));
  std::filesystem::create_directories(dir);
  return dir;
}

std::string readFile(const std::filesystem::path& path) {
  std::ifstream in(path);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

TEST(AutoTuningProcessTest, DefaultBinaryNameMatchesWhatTheInstallScriptsWrite) {
#ifdef _WIN32
  EXPECT_EQ(mm::auto_tuning::defaultBinaryName(), "auto-tuning.exe");
#else
  EXPECT_EQ(mm::auto_tuning::defaultBinaryName(), "auto-tuning");
#endif
}

TEST(AutoTuningProcessTest, BaseUrlNamesTheConfiguredPort) {
  mm::auto_tuning::ProcessOptions options;
  options.port = 1234;
  const mm::auto_tuning::Process process(options);

  EXPECT_EQ(process.baseUrl(), "http://127.0.0.1:1234");
}

TEST(AutoTuningProcessTest, StartWithoutABinaryIsAnError) {
  mm::auto_tuning::Process process({});

  const auto result = process.start();
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("no auto-tuning executable"), std::string::npos);
}

TEST(AutoTuningProcessTest, MissingBinaryReportsThePath) {
  mm::auto_tuning::ProcessOptions options;
  options.binary = testDir() / "not-installed";
  options.port = kUnusedPort;
  options.startTimeout = kShortTimeout;
  mm::auto_tuning::Process process(options);

  const auto result = process.start();
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("not-installed"), std::string::npos);
}

// The remaining two need a program to run, and the way to write one differs per platform. Nothing
// verifies the Windows implementation of the spawn: it is checked by running Motion Master there.
#ifndef _WIN32

TEST(AutoTuningProcessTest, ChildThatExitsAtOnceIsReportedWithItsLog) {
  const auto dir = testDir();
  const auto script = dir / "exits.sh";
  const auto logFile = dir / "auto-tuning.log";
  {
    std::ofstream out(script);
    out << "#!/bin/sh\necho started and gave up\nexit 3\n";
  }
  std::filesystem::permissions(script, std::filesystem::perms::owner_all);
  std::filesystem::remove(logFile);

  mm::auto_tuning::ProcessOptions options;
  options.binary = script;
  options.port = kUnusedPort;
  options.logFile = logFile;
  options.startTimeout = kShortTimeout;
  mm::auto_tuning::Process process(options);

  const auto result = process.start();
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("exited during startup"), std::string::npos);
  // The message has to point at the log, because the reason the child gave is only in there.
  EXPECT_NE(result.error().find(logFile.string()), std::string::npos);
  EXPECT_NE(readFile(logFile).find("started and gave up"), std::string::npos);
}

TEST(AutoTuningProcessTest, ChildThatNeverServesTimesOut) {
  const auto dir = testDir();
  const auto script = dir / "never-serves.sh";
  {
    std::ofstream out(script);
    // Alive, listening to nothing. The wait must end on its own timeout, and stop() must then take
    // the child down rather than leave it running. `exec` matters: a shell waiting on a foreground
    // child defers a termination signal until that child exits, so without it the test would wait
    // out the whole sleep.
    out << "#!/bin/sh\nexec sleep 30\n";
  }
  std::filesystem::permissions(script, std::filesystem::perms::owner_all);

  mm::auto_tuning::ProcessOptions options;
  options.binary = script;
  options.port = kUnusedPort;
  // Into a file, so the child's own output does not reach the pipe the test runner reads. A child
  // that outlives the test and holds that pipe open makes the test look slow.
  options.logFile = dir / "never-serves.log";
  options.startTimeout = kShortTimeout;
  mm::auto_tuning::Process process(options);

  const auto result = process.start();
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("did not answer"), std::string::npos);
  EXPECT_EQ(process.pid(), 0);
}

#endif

TEST(AutoTuningProcessTest, StopWithoutStartDoesNothing) {
  mm::auto_tuning::Process process({});

  process.stop();
  process.stop();
  EXPECT_EQ(process.pid(), 0);
}

}  // namespace
