#include "capture_runner.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <ctime>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/child_process.h"

namespace mm {

namespace {

/// A name a file system takes on every platform: letters, digits, dots and dashes.
std::string safeForFileName(const std::string& text) {
  std::string out;
  for (const char c : text) {
    const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '.' || c == '-';
    out.push_back(keep ? c : '_');
  }
  return out;
}

std::string lastLineOf(const std::filesystem::path& file) {
  std::ifstream in(file);
  std::string line;
  std::string last;
  while (std::getline(in, line)) {
    if (!line.empty()) {
      last = line;
    }
  }
  return last;
}

}  // namespace

std::filesystem::path captureBinaryName() {
#ifdef _WIN32
  return "motion-master-capture.exe";
#else
  return "motion-master-capture";
#endif
}

namespace {

CaptureError failed(std::string message) {
  return CaptureError{CaptureError::Kind::kFailed, std::move(message)};
}

/// Whether the program wrote its file, and if not, why, from the last line of its log.
std::expected<std::filesystem::path, CaptureError> result(const std::filesystem::path& file,
                                                          const std::filesystem::path& log,
                                                          bool exitedCleanly) {
  std::error_code ec;
  if (!exitedCleanly || !std::filesystem::exists(file, ec)) {
    const std::string why = lastLineOf(log);
    return std::unexpected(failed("the capture program failed" + (why.empty() ? "" : ": " + why)));
  }
  std::filesystem::remove(log, ec);
  return file;
}

}  // namespace

CaptureRunner::CaptureRunner(std::filesystem::path binary, std::filesystem::path dir)
    : binary_(std::move(binary)), dir_(std::move(dir)) {}

CaptureRunner::~CaptureRunner() {
  const std::scoped_lock lock(mutex_);
  if (started_) {
    mm::core::terminateChild(started_->child, std::chrono::seconds{5});
  }
}

std::expected<CaptureRunner::Started, CaptureError> CaptureRunner::prepare(
    const std::string& adapter) const {
  if (!std::filesystem::exists(binary_)) {
    return std::unexpected(failed("the capture program is not installed at " + binary_.string()));
  }
  std::error_code ec;
  std::filesystem::create_directories(dir_, ec);
  if (ec) {
    return std::unexpected(failed("cannot create " + dir_.string() + ": " + ec.message()));
  }
  const std::time_t now = std::time(nullptr);
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
  const std::string stem = std::string("capture-") + stamp + "-" + safeForFileName(adapter);
  return Started{{}, dir_ / (stem + ".pcap"), dir_ / (stem + ".log")};
}

std::expected<std::filesystem::path, CaptureError> CaptureRunner::run(const std::string& adapter,
                                                                      int seconds) {
  Started capture;
  {
    const std::scoped_lock lock(mutex_);
    if (busy_ || (started_ && mm::core::childAlive(started_->child))) {
      return std::unexpected(
          CaptureError{CaptureError::Kind::kBusy, "a capture is running; wait for it to finish"});
    }
    started_.reset();  // a started capture that ended by itself
    auto prepared = prepare(adapter);
    if (!prepared) {
      return std::unexpected(prepared.error());
    }
    capture = std::move(*prepared);
    busy_ = true;
  }
  // Cleared on every way out, so a capture that fails never refuses the next.
  struct ClearOnExit {
    CaptureRunner& runner;
    ~ClearOnExit() {
      const std::scoped_lock lock(runner.mutex_);
      runner.busy_ = false;
    }
  } clearOnExit{*this};

  const std::vector<std::string> args = {
      "--ifname", adapter, "--seconds", std::to_string(seconds), "--out", capture.file.string()};
  spdlog::info("Capture: {} on {} for {} s into {}", binary_.filename().string(), adapter, seconds,
               capture.file.string());
  auto child = mm::core::spawnChild(binary_, args, capture.log);
  if (!child) {
    return std::unexpected(failed("cannot start the capture program: " + child.error()));
  }
  const auto exit = mm::core::waitChild(*child, std::chrono::seconds{seconds + 10});
  if (!exit) {
    mm::core::terminateChild(*child, std::chrono::seconds{2});
    return std::unexpected(failed("the capture program did not finish in time and was stopped"));
  }
  return result(capture.file, capture.log, *exit == 0);
}

std::expected<std::filesystem::path, CaptureError> CaptureRunner::start(const std::string& adapter,
                                                                        int maxSeconds) {
#ifdef _WIN32
  (void)adapter;
  (void)maxSeconds;
  return std::unexpected(CaptureError{
      CaptureError::Kind::kNotSupported,
      "a capture with no fixed length needs a signal to end it, and Windows has none for a "
      "program without a console; use POST /api/captures with a number of seconds"});
#else
  const std::scoped_lock lock(mutex_);
  if (busy_ || (started_ && mm::core::childAlive(started_->child))) {
    return std::unexpected(
        CaptureError{CaptureError::Kind::kBusy, "a capture is running; stop it first"});
  }
  if (started_) {
    spdlog::info("Capture: {} ended at its time limit and was never stopped",
                 started_->file.string());
    started_.reset();
  }
  auto capture = prepare(adapter);
  if (!capture) {
    return std::unexpected(capture.error());
  }
  const std::vector<std::string> args = {"--ifname",  adapter,
                                         "--seconds", std::to_string(maxSeconds),
                                         "--out",     capture->file.string()};
  spdlog::info("Capture: {} on {} until stopped, at most {} s, into {}",
               binary_.filename().string(), adapter, maxSeconds, capture->file.string());
  auto child = mm::core::spawnChild(binary_, args, capture->log);
  if (!child) {
    return std::unexpected(failed("cannot start the capture program: " + child.error()));
  }
  // The program creates its file once the adapter is open and it handles SIGTERM, so a stop
  // after this point always gets a complete file. One that exits first refused the adapter.
  constexpr auto kStartTimeout = std::chrono::seconds{5};
  const auto deadline = std::chrono::steady_clock::now() + kStartTimeout;
  std::error_code ec;
  while (!std::filesystem::exists(capture->file, ec)) {
    if (!mm::core::childAlive(*child)) {
      const std::string why = lastLineOf(capture->log);
      return std::unexpected(
          failed("the capture program did not start" + (why.empty() ? "" : ": " + why)));
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      mm::core::terminateChild(*child, std::chrono::seconds{2});
      return std::unexpected(failed("the capture program did not start within 5 s"));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  capture->child = *child;
  started_ = std::move(*capture);
  return started_->file;
#endif
}

std::expected<std::filesystem::path, CaptureError> CaptureRunner::stop() {
  Started capture;
  {
    const std::scoped_lock lock(mutex_);
    if (!started_ || busy_) {
      return std::unexpected(
          CaptureError{CaptureError::Kind::kNotRunning, "no capture was started"});
    }
    capture = std::move(*started_);
    started_.reset();
    busy_ = true;  // a start during the stop is refused, not raced
  }
  struct ClearOnExit {
    CaptureRunner& runner;
    ~ClearOnExit() {
      const std::scoped_lock lock(runner.mutex_);
      runner.busy_ = false;
    }
  } clearOnExit{*this};
  // SIGTERM ends the program within 100 ms, and it writes its file and summary first. The kill
  // after the grace time is for a program that hangs; its file is then incomplete, and the
  // capture is reported as failed.
  const bool killed = mm::core::terminateChild(capture.child, std::chrono::seconds{5});
  spdlog::info("Capture: stopped {}", capture.file.string());
  return result(capture.file, capture.log, !killed);
}

}  // namespace mm
