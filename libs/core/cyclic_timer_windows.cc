#include "core/cyclic_timer.h"

#include <windows.h>

namespace mm::core {

// CREATE_WAITABLE_TIMER_HIGH_RESOLUTION gives ~0.5 ms resolution per timer,
// process-local.  The multimedia timer API (timeBeginPeriod) achieves similar
// resolution but as a system-wide change that affects all processes and
// increases power consumption — avoid it.
CyclicTimer::CyclicTimer(std::chrono::microseconds period) {
  handle_ = CreateWaitableTimerExW(nullptr, nullptr,
                                   CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                   TIMER_ALL_ACCESS);

  // Negative due time = relative first fire, in 100-nanosecond units.
  LARGE_INTEGER due{};
  due.QuadPart = -static_cast<LONGLONG>(period.count() * 10);

  // lPeriod is in milliseconds; acceptable for ≥1 ms cycles (Windows is a
  // development target, not a hard-RT target).
  SetWaitableTimer(static_cast<HANDLE>(handle_), &due,
                   static_cast<LONG>(period.count() / 1000),
                   nullptr, nullptr, FALSE);
}

CyclicTimer::~CyclicTimer() {
  CancelWaitableTimer(static_cast<HANDLE>(handle_));
  CloseHandle(static_cast<HANDLE>(handle_));
}

void CyclicTimer::waitForNextCycle() {
  WaitForSingleObject(static_cast<HANDLE>(handle_), INFINITE);
}

}  // namespace mm::core
