#pragma once

#include <chrono>
#include <thread>

namespace mm::node {
class DeviceManager;
}

/// @brief Logs what the RT loop saw and cannot say itself: cycles the bus did not fully answer.
///
/// A cycle whose working counter comes back below the expected value is one where some device did
/// not process the frame. The RT loop is the only thread that sees every cycle, and it cannot log —
/// so it only counts, into relaxed atomics that @c DeviceManager::processImageInfo serves.
/// Something off the RT thread has to read them, and nothing already running can: every background
/// thread in this process sleeps until it has work (the monitoring sampler waits on a condition
/// variable when no monitoring is registered), so this needs a timer of its own.
///
/// **Why it lives in the app and not in @c DeviceManager.** Writing warnings to a log is a
/// *policy*, and @c DeviceManager is meant to be embeddable without one — an embedder who does not
/// want this thread simply does not construct this. It is also the pattern already in place: every
/// background thread here belongs to a layer built outward from @c DeviceManager — the monitoring
/// manager and the procedure manager both own theirs — while @c DeviceManager owns none itself.
///
/// **Expected to be short-lived.** The planned @c NotificationBus is exactly this shape — one
/// off-RT thread polling a registry of `{change token, render}` sources — and bus health is its
/// first source. When it lands, this becomes one registration and the class goes away. See
/// NEXTGEN.md.
///
/// Silent unless the count has grown since the last check, so a healthy bus never speaks and one
/// fault is reported once. Construct after the @c DeviceManager it borrows, so it is destroyed
/// first: the destructor requests the stop and joins, and the interval is not waited out.
class BusHealthReporter {
 public:
  /// @param deviceManager  Borrowed; must outlive this reporter.
  /// @param interval       How often to look. A diagnostic cadence, not a sample rate — the count
  ///                       is cumulative, so a longer interval delays a report without losing one.
  explicit BusHealthReporter(mm::node::DeviceManager& deviceManager,
                             std::chrono::milliseconds interval = std::chrono::seconds{10});

 private:
  // Last on purpose: the members above are fully constructed before the thread that reads them
  // starts, and the thread is joined first on destruction.
  std::jthread thread_;
};
