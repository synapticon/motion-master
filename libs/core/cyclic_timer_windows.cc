#include <windows.h>

#include "core/cyclic_timer.h"

namespace mm::core {

// Windows is a development target, not a hard-RT target. This mirrors the
// Linux/macOS absolute-deadline model rather than using a periodic timer: the
// deadline grid lives on the QueryPerformanceCounter (monotonic) timeline, and
// each cycle arms a one-shot HIGH_RESOLUTION timer with a *relative* due time
// computed from the current QPC reading. Relative due times avoid the wall-clock
// (FILETIME) dependency an absolute SetWaitableTimer due time would carry, so a
// system-clock change can't disturb the cadence.
//
// CREATE_WAITABLE_TIMER_HIGH_RESOLUTION gives ~0.5 ms resolution per timer,
// process-local. The multimedia timer API (timeBeginPeriod) achieves similar
// resolution but as a system-wide change that affects all processes and
// increases power consumption — avoid it.
CyclicTimer::CyclicTimer(std::chrono::microseconds period) {
  handle_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                   TIMER_ALL_ACCESS);

  LARGE_INTEGER freq{};
  QueryPerformanceFrequency(&freq);
  qpcFreq_ = freq.QuadPart;

  // period (µs) → QPC ticks: ticks = µs * freq / 1e6.
  periodTicks_ = period.count() * qpcFreq_ / 1'000'000;

  // Anchor the grid at construction, matching the Linux/macOS baseline.
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  next_ = now.QuadPart;
}

CyclicTimer::~CyclicTimer() {
  CancelWaitableTimer(static_cast<HANDLE>(handle_));
  CloseHandle(static_cast<HANDLE>(handle_));
}

uint64_t CyclicTimer::waitForNextCycle() {
  next_ += periodTicks_;

  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);

  // Skip-to-grid: if the deadline is already behind us — an overrun or a
  // multi-cycle stall — fast-forward to the next future grid point instead of
  // firing the missed cycles back-to-back. Strict `<` so an exactly-due
  // deadline is met, not skipped. The original grid phase is preserved.
  uint64_t skipped = 0;
  while (next_ < now.QuadPart) {
    next_ += periodTicks_;
    ++skipped;
  }

  // Relative due time in 100-ns units (negative = relative). next_ >= now here,
  // so the delta is non-negative. ticks → 100-ns units: delta * 1e7 / freq.
  const int64_t deltaTicks = next_ - now.QuadPart;
  LARGE_INTEGER due{};
  due.QuadPart = -(deltaTicks * 10'000'000 / qpcFreq_);
  SetWaitableTimer(static_cast<HANDLE>(handle_), &due, 0, nullptr, nullptr, FALSE);
  WaitForSingleObject(static_cast<HANDLE>(handle_), INFINITE);
  return skipped;
}

}  // namespace mm::core
