#include <windows.h>

#include <format>
#include <string>

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
// CREATE_WAITABLE_TIMER_HIGH_RESOLUTION gives ~0.5 ms resolution per timer and
// needs no change of the timer resolution. A loop on timeBeginPeriod instead
// depends on the resolution Windows grants the process, and since Windows 11
// Windows does not guarantee it to a process that cannot be seen; the loop then
// falls to the default tick of 15.625 ms. (Since Windows 10 2004
// timeBeginPeriod no longer changes the global resolution.)
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

bool CyclicTimer::valid() const { return handle_ != nullptr; }

std::expected<void, std::string> checkCyclicTimer() {
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
  if (timer == nullptr) {
    return std::unexpected(std::format(
        "the RT loop needs a high-resolution waitable timer, which Windows 10, version 1803 or "
        "newer provides; CreateWaitableTimerExW failed with error {}",
        GetLastError()));
  }
  CloseHandle(timer);
  return {};
}

CyclicTimer::~CyclicTimer() {
  if (handle_ == nullptr) {
    return;
  }
  CancelWaitableTimer(static_cast<HANDLE>(handle_));
  CloseHandle(static_cast<HANDLE>(handle_));
}

void CyclicTimer::setPeriod(std::chrono::microseconds period) {
  // period (µs) → QPC ticks: ticks = µs * freq / 1e6.
  periodTicks_ = period.count() * qpcFreq_ / 1'000'000;
  // Re-anchor the grid at the current QPC reading, matching the constructor.
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  next_ = now.QuadPart;
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
