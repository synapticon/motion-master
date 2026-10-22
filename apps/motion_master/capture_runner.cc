#include "capture_runner.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <ctime>
#include <fstream>
#include <string>
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

std::expected<std::filesystem::path, std::string> runCapture(const std::filesystem::path& binary,
                                                             const std::string& adapter,
                                                             int seconds,
                                                             const std::filesystem::path& dir) {
  if (!std::filesystem::exists(binary)) {
    return std::unexpected("the capture program is not installed at " + binary.string());
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    return std::unexpected("cannot create " + dir.string() + ": " + ec.message());
  }
  const std::time_t now = std::time(nullptr);
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
  const std::string stem = std::string("capture-") + stamp + "-" + safeForFileName(adapter);
  const std::filesystem::path file = dir / (stem + ".pcap");
  const std::filesystem::path log = dir / (stem + ".log");

  const std::vector<std::string> args = {
      "--ifname", adapter, "--seconds", std::to_string(seconds), "--out", file.string()};
  spdlog::info("Capture: {} on {} for {} s into {}", binary.filename().string(), adapter, seconds,
               file.string());
  auto child = mm::core::spawnChild(binary, args, log);
  if (!child) {
    return std::unexpected("cannot start the capture program: " + child.error());
  }
  const auto exit = mm::core::waitChild(*child, std::chrono::seconds{seconds + 10});
  if (!exit) {
    mm::core::terminateChild(*child, std::chrono::seconds{2});
    return std::unexpected("the capture program did not finish in time and was stopped");
  }
  if (*exit != 0) {
    const std::string why = lastLineOf(log);
    return std::unexpected("the capture program failed" + (why.empty() ? "" : ": " + why));
  }
  std::filesystem::remove(log, ec);
  return file;
}

}  // namespace mm
