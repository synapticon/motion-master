#include "bus_health_reporter.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stop_token>

#include "node/device_manager.h"

BusHealthReporter::BusHealthReporter(mm::node::DeviceManager& deviceManager,
                                     std::chrono::milliseconds interval)
    : thread_([&deviceManager, interval](std::stop_token stop) {
        std::mutex mutex;
        std::condition_variable_any cv;
        std::unique_lock lock(mutex);
        uint64_t reported = 0;
        // The predicate is what distinguishes the two ways out: it returns true only when the stop
        // was requested, so a timeout runs a check and a stop leaves the loop — and shutdown does
        // not wait out the interval, which a plain sleep would make it do.
        while (!cv.wait_for(lock, stop, interval, [&stop] { return stop.stop_requested(); })) {
          const auto info = deviceManager.processImageInfo();
          if (info.shortWkcCycles == reported) {
            continue;  // nothing new since the last check — a healthy bus never speaks
          }
          reported = info.shortWkcCycles;
          // Reported as ages rather than absolute times, so the line is self-contained: it carries
          // its own reference point and a reader subtracts from it. An absolute time would have to
          // pick a zone, and printing UTC beside spdlog's local-time prefix reads as a
          // contradiction.
          const auto nowUs =
              static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count());
          const auto secondsBefore = [nowUs](uint64_t thenUs) {
            return thenUs == 0 || thenUs > nowUs ? 0.0 : static_cast<double>(nowUs - thenUs) / 1e6;
          };
          spdlog::warn(
              "Process data: {} cycle(s) answered with a short working counter since the bus came "
              "up; the first was {:.1f} s and the last {:.1f} s before this line",
              info.shortWkcCycles, secondsBefore(info.firstShortWkcUs),
              secondsBefore(info.lastShortWkcUs));
        }
      }) {}
