#include <time.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "core/cyclic_timer.h"
#include "core/realtime.h"

namespace {

// Applies the same RT setup the production GameLoop uses, so the measurement
// characterises the configuration that actually ships.
void setRealtimePriority() {
  const mm::core::RtSetupResult rt = mm::core::setRealtimePriority();
  if (!rt.schedFifo) {
    std::cerr << "warning: SCHED_FIFO failed — run as root or grant CAP_SYS_NICE for valid results\n";
  }
#ifdef __linux__
  if (!rt.memLocked) {
    std::cerr << "warning: mlockall failed — page faults may inflate jitter\n";
  }
#endif
}

// Spin-waits for duration_ns nanoseconds to simulate task workload without yielding the CPU.
void busyWait(int64_t duration_ns) {
  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);
  int64_t end_ns = start.tv_sec * 1'000'000'000LL + start.tv_nsec + duration_ns;
  struct timespec now;
  do {
    clock_gettime(CLOCK_MONOTONIC, &now);
  } while (now.tv_sec * 1'000'000'000LL + now.tv_nsec < end_ns);
}

int64_t percentileNs(const std::vector<int64_t>& sorted, double p) {
  size_t idx = static_cast<size_t>(p / 100.0 * static_cast<double>(sorted.size() - 1));
  return sorted[idx];
}

void printStats(const std::vector<int64_t>& samples, int period_us) {
  std::vector<int64_t> sorted = samples;
  std::sort(sorted.begin(), sorted.end());

  double sum = 0.0;
  for (int64_t v : samples) {
    sum += static_cast<double>(v);
  }
  double mean = sum / static_cast<double>(samples.size());

  double variance = 0.0;
  for (int64_t v : samples) {
    double d = static_cast<double>(v) - mean;
    variance += d * d;
  }
  double stddev = std::sqrt(variance / static_cast<double>(samples.size()));

  int64_t overruns = 0;
  int64_t period_ns = static_cast<int64_t>(period_us) * 1000;
  for (int64_t v : samples) {
    if (v > period_ns) {
      overruns++;
    }
  }

  std::cout << "Jitter (cycle-to-cycle deviation from " << period_us << " µs period):\n"
            << "  Min:    " << sorted.front() << " ns\n"
            << "  Max:    " << sorted.back() << " ns\n"
            << "  Mean:   " << static_cast<int64_t>(mean) << " ns\n"
            << "  StdDev: " << static_cast<int64_t>(stddev) << " ns\n"
            << "  P50:    " << percentileNs(sorted, 50.0) << " ns\n"
            << "  P95:    " << percentileNs(sorted, 95.0) << " ns\n"
            << "  P99:    " << percentileNs(sorted, 99.0) << " ns\n"
            << "  P99.9:  " << percentileNs(sorted, 99.9) << " ns\n\n"
            << "Overruns (|jitter| > one period): " << overruns << " / " << samples.size() << "\n";
}

}  // namespace

int main(int argc, char* argv[]) {
  int duration_s = 30;
  int period_us = 1000;
  int workload_us = 0;
  std::string output = "jitter.csv";

  for (int i = 1; i < argc; i++) {
    std::string_view arg(argv[i]);
    if (arg == "--help") {
      std::cout << "Usage: jitter_bench [options]\n\n"
                << "Options:\n"
                << "  --duration <s>     Run duration in seconds        (default: 30)\n"
                << "  --period <µs>      Cycle period in microseconds   (default: 1000)\n"
                << "  --workload <µs>    Per-cycle busy-wait to simulate task load  (default: 0)\n"
                << "  --output <file>    CSV output path                (default: jitter.csv)\n\n"
                << "Run as root or with CAP_SYS_NICE + CAP_IPC_LOCK for valid RT results.\n"
                << "Plot results with: python3 plot_jitter.py jitter.csv\n";
      return 0;
    } else if (arg == "--duration" && i + 1 < argc) {
      duration_s = std::atoi(argv[++i]);
    } else if (arg == "--period" && i + 1 < argc) {
      period_us = std::atoi(argv[++i]);
    } else if (arg == "--workload" && i + 1 < argc) {
      workload_us = std::atoi(argv[++i]);
    } else if (arg == "--output" && i + 1 < argc) {
      output = argv[++i];
    }
  }

  uint64_t num_cycles =
      static_cast<uint64_t>(duration_s) * 1'000'000ULL / static_cast<uint64_t>(period_us);

  std::cout << "Jitter Bench\n"
            << "  Period:   " << period_us << " µs\n"
            << "  Duration: " << duration_s << " s  (" << num_cycles << " cycles)\n"
            << "  Workload: " << workload_us << " µs/cycle\n"
            << "  Output:   " << output << "\n\n"
            << "Running..." << std::endl;

  std::vector<int64_t> timestamps;
  timestamps.reserve(num_cycles);

  setRealtimePriority();

  mm::core::CyclicTimer timer{std::chrono::microseconds(period_us)};
  for (uint64_t i = 0; i < num_cycles; i++) {
    timer.waitForNextCycle();

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    timestamps.push_back(now.tv_sec * 1'000'000'000LL + now.tv_nsec);

    if (workload_us > 0) {
      busyWait(static_cast<int64_t>(workload_us) * 1000);
    }
  }

  // Compute cycle-to-cycle jitter: how much each actual interval deviated from the target period.
  int64_t period_ns = static_cast<int64_t>(period_us) * 1000;
  std::vector<int64_t> jitter_ns;
  jitter_ns.reserve(timestamps.size() - 1);
  for (size_t i = 1; i < timestamps.size(); i++) {
    jitter_ns.push_back((timestamps[i] - timestamps[i - 1]) - period_ns);
  }

  {
    std::ofstream f(output);
    f << "cycle,elapsed_ms,jitter_ns\n";
    for (size_t i = 0; i < jitter_ns.size(); i++) {
      double elapsed_ms = static_cast<double>(timestamps[i + 1] - timestamps[0]) / 1e6;
      f << (i + 1) << "," << elapsed_ms << "," << jitter_ns[i] << "\n";
    }
  }

  std::cout << "Done. " << jitter_ns.size() << " samples collected.\n\n";
  printStats(jitter_ns, period_us);
  std::cout << "\nPlot: python3 plot_jitter.py " << output << "\n";

  return 0;
}
