#include "auto_tuning/process.h"

#include <chrono>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "auto_tuning/process_platform.h"
#include "net/http_client.h"

namespace mm::auto_tuning {

namespace {

/// How often to ask a starting child whether it is ready. The child takes about a second to unpack
/// itself, so this costs about twenty requests against a socket that is not listening yet.
constexpr std::chrono::milliseconds kHealthPollInterval{50};

/// One health request must not sit on the timeout of the whole wait, so it gets its own short one.
constexpr std::chrono::seconds kHealthRequestTimeout{2};

/// How long the child gets to act on the exit request before it is signalled, and then again
/// before it is killed.
constexpr std::chrono::milliseconds kStopGrace{2000};

/// The exit request must not hold shutdown up. A child that does not answer it in this long is a
/// child for the signal to deal with.
constexpr std::chrono::seconds kExitRequestTimeout{1};

/// @brief Reads the version out of a health reply, or returns empty when it carries none.
///
/// A version is not required. An older auto-tuning build answers `{"status":"ok"}` and nothing
/// else, and a child that serves is useful whether or not it says which build it is.
std::string versionFromHealth(const std::string& body) {
  const auto doc = nlohmann::json::parse(body, nullptr, false);
  if (doc.is_discarded() || !doc.is_object()) {
    return {};
  }
  const auto version = doc.find("version");
  if (version == doc.end() || !version->is_string()) {
    return {};
  }
  return version->get<std::string>();
}

}  // namespace

std::filesystem::path defaultBinaryName() {
#ifdef _WIN32
  return "auto-tuning.exe";
#else
  return "auto-tuning";
#endif
}

Process::Process(ProcessOptions options) : options_(std::move(options)) {}

Process::~Process() { stop(); }

std::string Process::baseUrl() const { return "http://127.0.0.1:" + std::to_string(options_.port); }

std::expected<void, std::string> Process::start() {
  if (pid_ != 0) {
    return std::unexpected("auto-tuning is already running");
  }
  if (options_.binary.empty()) {
    return std::unexpected("no auto-tuning executable was named");
  }

  // --host is passed even though 127.0.0.1 is the child's own default, so the address Motion Master
  // then connects to is stated in one place rather than assumed to match.
  const std::vector<std::string> args{"--http", "--host", "127.0.0.1", "--port",
                                      std::to_string(options_.port)};
  auto child = detail::spawnChild(options_.binary, args, options_.logFile);
  if (!child) {
    return std::unexpected("cannot start " + options_.binary.string() + ": " + child.error());
  }
  pid_ = child->pid;
  handle_ = child->handle;
  group_ = child->group;

  const std::string healthUrl = baseUrl() + "/api/health";
  const auto deadline = std::chrono::steady_clock::now() + options_.startTimeout;
  while (std::chrono::steady_clock::now() < deadline) {
    // A child that died explains itself in its own log, and its exit is the real failure — so stop
    // waiting for a port that will never open and say where to look.
    if (!detail::childAlive({pid_, handle_, group_})) {
      forget();
      std::string message = options_.binary.string() + " exited during startup";
      if (!options_.logFile.empty()) {
        message += "; see " + options_.logFile.string();
      }
      return std::unexpected(message);
    }
    if (auto response = mm::net::httpGet(healthUrl, kHealthRequestTimeout);
        response && response->status == 200) {
      version_ = versionFromHealth(response->body);
      return {};
    }
    std::this_thread::sleep_for(kHealthPollInterval);
  }

  // A child that is alive but not answering is worse than one that failed to start, because it
  // holds the port. Take it down, so a later attempt starts from nothing.
  stop();
  return std::unexpected(options_.binary.string() + " did not answer on port " +
                         std::to_string(options_.port) + " within " +
                         std::to_string(options_.startTimeout.count()) + " ms");
}

Process::StopOutcome Process::stop() {
  if (pid_ == 0) {
    return StopOutcome::NotRunning;
  }

  // Ask over its own API first. The auto-tuning program answers a run named "exit" by shutting the
  // server down, which is the only orderly way to stop it on Windows: there is no signal it handles
  // there, and its output is block-buffered, so terminating it outright drops the tail of its log.
  // The reply is of no interest, and neither is a failure — the steps below do not depend on it.
  const auto exitRequest = mm::net::httpPost(baseUrl() + "/api/run", R"({"run":"exit"})",
                                             "application/json", kExitRequestTimeout);
  if (exitRequest) {
    const auto deadline = std::chrono::steady_clock::now() + kStopGrace;
    while (std::chrono::steady_clock::now() < deadline) {
      if (!detail::childAlive({pid_, handle_, group_})) {
        forget();
        return StopOutcome::Requested;
      }
      std::this_thread::sleep_for(kHealthPollInterval);
    }
  }

  const bool killed = detail::terminateChild({pid_, handle_, group_}, kStopGrace);
  forget();
  return killed ? StopOutcome::Killed : StopOutcome::Signalled;
}

void Process::forget() {
  pid_ = 0;
  handle_ = 0;
  group_ = 0;
  version_.clear();
}

}  // namespace mm::auto_tuning
