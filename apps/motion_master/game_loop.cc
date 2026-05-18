#include "game_loop.h"

#include <spdlog/spdlog.h>

#include "core/cyclic_timer.h"

#ifndef _WIN32
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#endif

namespace {

void setRealtimePriority() {
#ifndef _WIN32
  struct sched_param param{};
  param.sched_priority = 80;
  if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
    spdlog::warn("GameLoop: failed to set SCHED_FIFO — running without RT scheduling");
  }
  // Prevents the kernel from faulting in pages mid-cycle, which would
  // introduce unbounded latency spikes.
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
    spdlog::warn("GameLoop: failed to lock memory pages — page faults may cause jitter");
  }
#endif
}

}  // namespace

GameLoop::GameLoop(std::chrono::microseconds period) : period_(period) {}

void GameLoop::addTask(ICyclicTask* task) {
  tasks_.push_back(task);
}

void GameLoop::run() {
  setRealtimePriority();
  running_.store(true, std::memory_order_relaxed);

  mm::core::CyclicTimer timer(period_);
  while (running_.load(std::memory_order_relaxed)) {
    timer.waitForNextCycle();
    tick_.fetch_add(1, std::memory_order_relaxed);
    for (ICyclicTask* task : tasks_) {
      task->execute();
    }
  }
}

void GameLoop::stop() {
  running_.store(false, std::memory_order_relaxed);
}

uint64_t GameLoop::tick() const {
  return tick_.load(std::memory_order_relaxed);
}
