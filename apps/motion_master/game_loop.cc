#include "game_loop.h"

#include <spdlog/spdlog.h>

#include "core/cyclic_timer.h"

#ifndef _WIN32
#include <pthread.h>
#include <sched.h>
#endif
#ifdef __linux__
#include <sys/mman.h>
#endif

namespace {

void setRealtimePriority() {
#ifndef _WIN32
  struct sched_param param = {};
  param.sched_priority = 80;
  if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
    spdlog::warn("GameLoop: failed to set SCHED_FIFO — running without RT scheduling");
  }
#endif
#ifdef __linux__
  // Prevents the kernel from faulting in pages mid-cycle, which would
  // introduce unbounded latency spikes.  macOS has no mlockall().
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
    spdlog::warn("GameLoop: failed to lock memory pages — page faults may cause jitter");
  }
#endif
}

}  // namespace

GameLoop::GameLoop(std::chrono::microseconds period) : period_(period) {}

void GameLoop::addTask(CyclicTask* task) { tasks_.push_back(task); }

void GameLoop::run() {
  setRealtimePriority();
  running_.store(true, std::memory_order_relaxed);

  mm::core::CyclicTimer timer(period_);
  while (running_.load(std::memory_order_relaxed)) {
    const uint64_t skipped = timer.waitForNextCycle();
    // executed = loop iterations run; skipped = cycles the timer skipped to
    // catch up after an overrun or scheduling stall. Both are counted silently
    // for diagnostics; the RT path itself does no logging (skip-to-grid means
    // we never burst stale frames). Their sum is the absolute grid index.
    // This thread is the only writer, so load/compute/store needs no atomic RMW.
    const uint64_t executed = executedCycles_.load(std::memory_order_relaxed) + 1;
    const uint64_t totalSkipped = skippedCycles_.load(std::memory_order_relaxed) + skipped;
    executedCycles_.store(executed, std::memory_order_relaxed);
    skippedCycles_.store(totalSkipped, std::memory_order_relaxed);
    const CycleContext ctx{.elapsed = executed + totalSkipped, .skipped = skipped};
    for (CyclicTask* task : tasks_) {
      task->execute(ctx);
    }
  }
}

// stop() is documented as safe to call from a signal handler, which holds only if the store to
// running_ is a lock-free atomic operation — a non-lock-free atomic would acquire an internal lock,
// which is forbidden in a signal handler. atomic<bool> is lock-free on every supported platform;
// assert it so the guarantee can never silently regress.
static_assert(std::atomic<bool>::is_always_lock_free,
              "GameLoop::stop() must be async-signal-safe: std::atomic<bool> must be lock-free");

void GameLoop::stop() { running_.store(false, std::memory_order_relaxed); }

uint64_t GameLoop::executedCycles() const {
  return executedCycles_.load(std::memory_order_relaxed);
}

uint64_t GameLoop::skippedCycles() const { return skippedCycles_.load(std::memory_order_relaxed); }
