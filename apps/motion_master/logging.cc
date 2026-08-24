#include "logging.h"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace mm {

LogSinks installLogSinks() {
  // Replacing the default logger drops its built-in console sink, so re-add it
  // explicitly alongside the ring sink that backs GET /api/log.
  LogSinks sinks{.ring = std::make_shared<RingLogSinkMt>(),
                 .console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>()};
  spdlog::set_default_logger(
      std::make_shared<spdlog::logger>("", spdlog::sinks_init_list{sinks.console, sinks.ring}));
  return sinks;
}

std::filesystem::path applyLoggingConfig(const LogSinks& sinks, const LoggingConfig& config,
                                         const std::filesystem::path& userCacheRoot) {
  // The console and the ring share the configured level; the file keeps its own, so the terminal
  // can stay readable while the file holds the detail a support request needs. The logger gates
  // before any sink does, so it has to run at whichever of the two is more verbose — otherwise the
  // console setting would silently starve the file.
  const auto consoleLevel = spdlog::level::from_str(config.level);
  sinks.console->set_level(consoleLevel);
  sinks.ring->set_level(consoleLevel);
  auto loggerLevel = consoleLevel;
  std::filesystem::path logFile;
  if (config.file.enabled) {
    const auto& fileConfig = config.file;
    const std::filesystem::path logDir = fileConfig.directory.empty()
                                             ? userCacheRoot / "logs"
                                             : std::filesystem::path{fileConfig.directory};
    logFile = logDir / "motion-master.log";
    // spdlog signals a sink it cannot open by throwing, which is the one place this codebase has to
    // catch rather than return — and it must not be fatal: a read-only or unwritable directory is a
    // reason to run without a log file, not a reason not to run. The console and GET /api/log are
    // unaffected either way.
    try {
      auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
          logFile.string(), static_cast<std::size_t>(fileConfig.maxSizeMb) * 1024 * 1024,
          fileConfig.maxFiles);
      const auto fileLevel = spdlog::level::from_str(fileConfig.level);
      fileSink->set_level(fileLevel);
      spdlog::default_logger()->sinks().push_back(std::move(fileSink));
      loggerLevel = std::min(loggerLevel, fileLevel);
    } catch (const spdlog::spdlog_ex& e) {
      logFile.clear();
      spdlog::warn("Log file disabled — could not open {}: {}", (logDir).string(), e.what());
    }
  }
  spdlog::set_level(loggerLevel);
  // Every line, not just the alarming ones. spdlog hands each line to fwrite immediately but never
  // calls fflush of its own accord, so without this they sit in the C stdio buffer (~4 KB, roughly
  // 44 lines) until it fills. A clean shutdown flushes that; a segfault or a SIGKILL does not — and
  // the log file exists precisely to outlive the crash it is describing, so a tail lost to the
  // buffer is the one part that must not be missing.
  //
  // Flushing only on warn+ was the first attempt, on the assumption that a crash is preceded by a
  // warning that checkpoints everything buffered before it. That assumption does not hold here:
  // this codebase's crashes were memory-corruption segfaults inside SOEM (the re-map FMMU
  // overrun, the BOOT -> SAFE-OP mailbox read), which log nothing at all before dying — so the
  // policy dropped exactly the trail into the fault it was meant to preserve.
  //
  // Measured, in this three-sink layout: ~610 ns per line against ~240. The ratio is large and the
  // number is not — the heaviest thing logged is a full parameter read at a few thousand lines, so
  // roughly a millisecond across an operation that spends seconds in SDO round-trips. Nothing logs
  // on the RT path (checked: exchangeProcessData and the cyclic tasks make no logging calls), so no
  // deadline is exposed to it either.
  //
  // "Flushed" means handed to the OS, not on the platter — this survives a process crash, not a
  // power cut. Guarding against that needs an fsync per line, which is milliseconds each.
  spdlog::default_logger()->flush_on(spdlog::level::trace);
  return logFile;
}

}  // namespace mm
