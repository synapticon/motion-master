#include "capture_runner.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace {

namespace fs = std::filesystem;
using mm::CaptureError;
using mm::CaptureRunner;

#ifndef _WIN32

// Stands in for the capture program: it takes the same options, writes its file when SIGTERM
// arrives or its time is up, and exits 0. With --ifname fail it says why on stderr and exits 1, as
// the real program does for an adapter it cannot open.
constexpr std::string_view kFakeProgram = R"sh(#!/bin/sh
out=""; seconds=60; adapter=""
while [ $# -gt 0 ]; do
  case "$1" in
    --out) out="$2"; shift ;;
    --seconds) seconds="$2"; shift ;;
    --ifname) adapter="$2"; shift ;;
  esac
  shift
done
if [ "$adapter" = "fail" ]; then
  echo "no adapter named fail" >&2
  exit 1
fi
trap 'printf pcap > "$out"; exit 0' TERM
: > "$out"
end=$(( $(date +%s) + seconds ))
while [ "$(date +%s)" -lt "$end" ];
do sleep 0.05; done
printf pcap > "$out"
exit 0
)sh";

class CaptureRunnerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() /
            ("capture_runner_test_" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root_);
    program_ = root_ / "fake-capture";
    std::ofstream(program_) << kFakeProgram;
    fs::permissions(program_, fs::perms::owner_all);
  }
  void TearDown() override { fs::remove_all(root_); }

  fs::path root_;
  fs::path program_;
};

TEST_F(CaptureRunnerTest, AStoppedCaptureLeavesItsFileAndNoLog) {
  CaptureRunner runner{program_, root_ / "captures"};
  auto started = runner.start("eth0", 60);
  ASSERT_TRUE(started.has_value()) << started.error().message;
  auto stopped = runner.stop();
  ASSERT_TRUE(stopped.has_value()) << stopped.error().message;
  EXPECT_EQ(*stopped, *started);
  EXPECT_TRUE(fs::exists(*stopped));
  EXPECT_FALSE(fs::exists(fs::path(*stopped).replace_extension(".log")));
}

TEST_F(CaptureRunnerTest, OneCaptureRunsAtATime) {
  CaptureRunner runner{program_, root_ / "captures"};
  ASSERT_TRUE(runner.start("eth0", 60).has_value());

  auto second = runner.start("eth0", 60);
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error().kind, CaptureError::Kind::kBusy);
  auto blocking = runner.run("eth0", 1);
  ASSERT_FALSE(blocking.has_value());
  EXPECT_EQ(blocking.error().kind, CaptureError::Kind::kBusy);

  EXPECT_TRUE(runner.stop().has_value());
}

TEST_F(CaptureRunnerTest, AStopWithNoStartedCaptureIsRefused) {
  CaptureRunner runner{program_, root_ / "captures"};
  auto stopped = runner.stop();
  ASSERT_FALSE(stopped.has_value());
  EXPECT_EQ(stopped.error().kind, CaptureError::Kind::kNotRunning);
}

TEST_F(CaptureRunnerTest, ARefusedAdapterFailsTheStartWithWhatTheProgramSaid) {
  CaptureRunner runner{program_, root_ / "captures"};
  auto started = runner.start("fail", 60);
  ASSERT_FALSE(started.has_value());
  EXPECT_EQ(started.error().kind, CaptureError::Kind::kFailed);
  EXPECT_NE(started.error().message.find("no adapter named fail"), std::string::npos)
      << started.error().message;
  // Nothing is left running, so the next start is not refused.
  EXPECT_TRUE(runner.start("eth0", 60).has_value());
  EXPECT_TRUE(runner.stop().has_value());
}

TEST_F(CaptureRunnerTest, ACaptureThatReachedItsLimitIsDroppedByTheNextStart) {
  CaptureRunner runner{program_, root_ / "captures"};
  ASSERT_TRUE(runner.start("eth0", 1).has_value());
  std::this_thread::sleep_for(std::chrono::milliseconds{2500});
  auto next = runner.start("eth1", 60);
  ASSERT_TRUE(next.has_value()) << next.error().message;
  EXPECT_TRUE(runner.stop().has_value());
}

TEST_F(CaptureRunnerTest, ABlockingRunReturnsItsFile) {
  CaptureRunner runner{program_, root_ / "captures"};
  auto file = runner.run("eth0", 1);
  ASSERT_TRUE(file.has_value()) << file.error().message;
  EXPECT_TRUE(fs::exists(*file));
}

TEST_F(CaptureRunnerTest, AMissingProgramFails) {
  CaptureRunner runner{root_ / "missing", root_ / "captures"};
  auto started = runner.start("eth0", 60);
  ASSERT_FALSE(started.has_value());
  EXPECT_EQ(started.error().kind, CaptureError::Kind::kFailed);
}

#else

TEST(CaptureRunnerTest, AStartedCaptureIsNotSupportedOnWindows) {
  CaptureRunner runner{"motion-master-capture.exe", "captures"};
  auto started = runner.start("eth0", 60);
  ASSERT_FALSE(started.has_value());
  EXPECT_EQ(started.error().kind, CaptureError::Kind::kNotSupported);
}

#endif

}  // namespace
