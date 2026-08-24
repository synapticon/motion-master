#include "auto_tuning_setup.h"

#include <spdlog/spdlog.h>

#include <string>
#include <utility>

#include "core/platform.h"

namespace mm {

auto_tuning::ProcessOptions buildAutoTuningOptions(const AutoTuningConfig& config,
                                                   const std::filesystem::path& logFile) {
  auto_tuning::ProcessOptions options;
  options.binary = config.binaryPath.empty() ? mm::core::exeDir() / auto_tuning::defaultBinaryName()
                                             : std::filesystem::path{config.binaryPath};
  options.port = config.port;
  if (!logFile.empty()) {
    options.logFile = logFile.parent_path() / "auto-tuning.log";
  }
  return options;
}

AutoTuningStartup startAutoTuning(auto_tuning::Process& process, bool enabled) {
  const auto& options = process.options();
  AutoTuningStartup startup;
  startup.status.enabled = enabled;
  startup.status.binaryPath = options.binary.string();
  startup.status.installed = std::filesystem::exists(options.binary);
  startup.status.port = options.port;

  // The install scripts download the executable, and a machine that could not reach the release, or
  // that was installed from a tarball nobody ran setup.sh on, simply has no auto-tuning. So every
  // failure here is a warning.
  if (!enabled) {
    startup.status.error = "disabled by the configuration";
    spdlog::info("Auto-tuning is disabled by the configuration");
  } else if (!startup.status.installed) {
    startup.status.error = "not installed at " + startup.status.binaryPath;
    spdlog::warn("Auto-tuning is not installed at {} — the auto-tuning endpoints will fail",
                 startup.status.binaryPath);
  } else if (auto started = process.start(); !started) {
    startup.status.error = started.error();
    spdlog::warn("Auto-tuning did not start: {}", started.error());
  } else {
    startup.status.started = true;
    startup.status.version = process.version();
    startup.client.emplace(process.baseUrl());
    spdlog::info("Auto-tuning {} started on port {} (pid {})",
                 process.version().empty() ? "of an unknown version" : process.version(),
                 options.port, process.pid());
  }
  return startup;
}

void stopAutoTuning(auto_tuning::Process& process) {
  const auto pid = process.pid();
  switch (process.stop()) {
    case auto_tuning::Process::StopOutcome::NotRunning:
      break;
    case auto_tuning::Process::StopOutcome::Requested:
      spdlog::info("Auto-tuning stopped (pid {})", pid);
      break;
    case auto_tuning::Process::StopOutcome::Signalled:
      spdlog::info("Auto-tuning stopped after a termination signal (pid {})", pid);
      break;
    case auto_tuning::Process::StopOutcome::Killed:
      // It ignored both the exit request and the signal, so it was wedged inside a routine. Worth a
      // warning: this is the case where something could be left holding the port against the next
      // start.
      spdlog::warn("Auto-tuning did not exit and was killed (pid {})", pid);
      break;
  }
}

}  // namespace mm
