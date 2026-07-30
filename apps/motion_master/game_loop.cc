#include "game_loop.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <nlohmann/json.hpp>

#include "core/cyclic_timer.h"
#include "core/realtime.h"

namespace {

// Monotonic clock in ns — used for uptime (achievedHz) and per-cycle work timing.
uint64_t nowMonoNs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

// Wall-clock epoch µs — stamped into each health snapshot so the client's live-rate Δt is exact.
uint64_t nowEpochUs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count());
}

}  // namespace

GameLoop::GameLoop(std::chrono::microseconds period) : period_(period) {}

void GameLoop::addTask(CyclicTask* task) { tasks_.push_back(task); }

void GameLoop::run() {
  const mm::core::RtSetupResult rt = mm::core::setRealtimePriority();
  // Warn only where the step exists at all, so Windows (neither) stays quiet.
#ifndef _WIN32
  if (!rt.schedFifo) {
    spdlog::warn("GameLoop: failed to set SCHED_FIFO — running without RT scheduling");
  }
#endif
#ifdef __linux__
  if (!rt.memLocked) {
    spdlog::warn("GameLoop: failed to lock memory pages — page faults may cause jitter");
  }
#endif
  schedFifo_.store(rt.schedFifo, std::memory_order_relaxed);
  memLocked_.store(rt.memLocked, std::memory_order_relaxed);
  running_.store(true, std::memory_order_relaxed);
  startMonoNs_.store(nowMonoNs(), std::memory_order_relaxed);

  std::chrono::microseconds currentPeriod = period_.load(std::memory_order_relaxed);
  mm::core::CyclicTimer timer(currentPeriod);
  while (running_.load(std::memory_order_relaxed)) {
    const uint64_t skipped = timer.waitForNextCycle();

    // Apply a pending period change (setPeriod, from another thread). Re-anchor
    // the timer's deadline grid to now so the change takes effect next cycle
    // without a spurious skip burst. One relaxed load per cycle — negligible.
    const std::chrono::microseconds newPeriod = period_.load(std::memory_order_relaxed);
    if (newPeriod != currentPeriod) {
      currentPeriod = newPeriod;
      timer.setPeriod(newPeriod);
      // A period change starts a fresh health epoch. The point of changing the period is to
      // improve the loop's health, so the cumulative counters must not carry the pre-change
      // regime — otherwise the achieved rate and skip total would still show the old, worse
      // numbers and hide the very improvement the change made. Reset them here (single-writer:
      // this RT thread owns every counter below, so plain relaxed stores don't race the reads in
      // health()) and re-anchor the achievedHz baseline to now, in lockstep with the timer grid
      // re-anchor above. This cycle's fetch_add below seeds the new epoch as cycle 1.
      executedCycles_.store(0, std::memory_order_relaxed);
      skippedCycles_.store(0, std::memory_order_relaxed);
      maxExecNs_.store(0, std::memory_order_relaxed);
      sumExecNs_.store(0, std::memory_order_relaxed);
      startMonoNs_.store(nowMonoNs(), std::memory_order_relaxed);
    }
    // executed = loop iterations run; skipped = cycles the timer skipped to catch
    // up after an overrun or scheduling stall. Counted silently for diagnostics —
    // no logging on the RT path; skip-to-grid means we never burst stale frames.
    // fetch_add returns the pre-increment value, so add back to get the new
    // totals; their sum is the absolute grid index (elapsed).
    const uint64_t executed = executedCycles_.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint64_t totalSkipped =
        skippedCycles_.fetch_add(skipped, std::memory_order_relaxed) + skipped;
    const CycleContext ctx{.elapsed = executed + totalSkipped, .skipped = skipped};

    // Time the task work (not the sleep) so /api/game-loop can report budget use.
    // steady_clock::now() is a vDSO CLOCK_MONOTONIC read on Linux — cheap enough
    // for the RT path; the three stores below are relaxed and single-writer.
    const auto workStart = std::chrono::steady_clock::now();
    for (CyclicTask* task : tasks_) {
      task->execute(ctx);
    }
    const auto workEnd = std::chrono::steady_clock::now();
    const uint64_t execNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(workEnd - workStart).count());
    lastExecNs_.store(execNs, std::memory_order_relaxed);
    if (execNs > maxExecNs_.load(std::memory_order_relaxed)) {
      maxExecNs_.store(execNs, std::memory_order_relaxed);
    }
    sumExecNs_.fetch_add(execNs, std::memory_order_relaxed);
  }
}

// stop() is documented as safe to call from a signal handler, which holds only if the store to
// running_ is a lock-free atomic operation — a non-lock-free atomic would acquire an internal lock,
// which is forbidden in a signal handler. atomic<bool> is lock-free on every supported platform;
// assert it so the guarantee can never silently regress.
static_assert(std::atomic<bool>::is_always_lock_free,
              "GameLoop::stop() must be async-signal-safe: std::atomic<bool> must be lock-free");

void GameLoop::stop() { running_.store(false, std::memory_order_relaxed); }

void GameLoop::setPeriod(std::chrono::microseconds period) {
  period_.store(period, std::memory_order_relaxed);
}

uint64_t GameLoop::executedCycles() const {
  return executedCycles_.load(std::memory_order_relaxed);
}

uint64_t GameLoop::skippedCycles() const { return skippedCycles_.load(std::memory_order_relaxed); }

GameLoopHealth GameLoop::health() const {
  GameLoopHealth h;
  h.periodUs = static_cast<uint64_t>(period_.load(std::memory_order_relaxed).count());
  h.targetHz = h.periodUs > 0 ? 1'000'000.0 / static_cast<double>(h.periodUs) : 0.0;
  h.executedCycles = executedCycles_.load(std::memory_order_relaxed);
  h.skippedCycles = skippedCycles_.load(std::memory_order_relaxed);
  h.lastExecNs = lastExecNs_.load(std::memory_order_relaxed);
  h.maxExecNs = maxExecNs_.load(std::memory_order_relaxed);
  const uint64_t sum = sumExecNs_.load(std::memory_order_relaxed);
  h.avgExecNs = h.executedCycles > 0 ? sum / h.executedCycles : 0;
  const uint64_t start = startMonoNs_.load(std::memory_order_relaxed);
  if (start != 0) {
    const uint64_t now = nowMonoNs();
    const uint64_t upNs = now > start ? now - start : 0;
    h.achievedHz =
        upNs > 0 ? static_cast<double>(h.executedCycles) * 1e9 / static_cast<double>(upNs) : 0.0;
  }
  h.schedFifo = schedFifo_.load(std::memory_order_relaxed);
  h.memLocked = memLocked_.load(std::memory_order_relaxed);
  h.timestampUs = nowEpochUs();
  return h;
}

void to_json(nlohmann::json& j, const GameLoopHealth& h) {
  j = nlohmann::json{{"periodUs", h.periodUs},           {"targetHz", h.targetHz},
                     {"achievedHz", h.achievedHz},       {"executedCycles", h.executedCycles},
                     {"skippedCycles", h.skippedCycles}, {"lastExecNs", h.lastExecNs},
                     {"maxExecNs", h.maxExecNs},         {"avgExecNs", h.avgExecNs},
                     {"schedFifo", h.schedFifo},         {"memLocked", h.memLocked},
                     {"timestampUs", h.timestampUs}};
}
