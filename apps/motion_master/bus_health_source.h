#pragma once

#include <string_view>

#include "node/notification_bus.h"

namespace mm::node {
class DeviceManager;
}

/// @file
/// @brief The notification source for cycles the bus did not fully answer.

namespace mm {

/// @brief The @c data.event this source sends.
///
/// Named here rather than written into the render, because an event name is protocol: a client
/// switches on it, and it must survive a rewrite of the message around it.
inline constexpr std::string_view kShortWorkingCounterEvent = "short-working-counter";

/// @brief Reports cycles that answered with a short working counter, to the log and to clients.
///
/// A cycle whose working counter comes back below the expected value is one where some device did
/// not process the frame. The real-time loop is the only thread that sees every cycle, and it
/// cannot log or publish — so it only counts, and this reads that count.
///
/// **Why it lives in the app and not in @c DeviceManager.** Writing a warning to a log is a policy,
/// and @c DeviceManager is meant to be embeddable without one. It is also the pattern already in
/// place: every background thread in this process belongs to a layer built outward from
/// @c DeviceManager, which owns none itself.
///
/// It both warns and returns a payload. The warning is what reaches a support log from a machine
/// nobody was watching; the payload is what reaches a client that is. Whether a source does one or
/// both is a per-source decision, not a rule.
///
/// Silent unless the count has grown since it was last read, so a healthy bus never speaks and one
/// fault is reported once.
///
/// @param deviceManager  Borrowed. Must outlive the bus this source is registered with.
/// @param interval       How often the count is read, which is also the most often it can be
///                       reported. A second, because someone watching a bus fault should see it
///                       while they are still looking at what caused it — and because at 1 kHz a
///                       faulting bus bumps the counter a thousand times a second, so reporting at
///                       the rate of the fault is useless. Nothing is lost to the gap: the count is
///                       cumulative and carries the time of the first and last cycle in it.
node::NotificationBus::Source busHealthSource(
    node::DeviceManager& deviceManager,
    std::chrono::milliseconds interval = std::chrono::seconds{1});

}  // namespace mm
