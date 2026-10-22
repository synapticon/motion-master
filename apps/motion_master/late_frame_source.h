#pragma once

#include <chrono>
#include <string_view>

#include "node/notification_bus.h"

namespace mm::node {
class DeviceManager;
}

/// @file
/// @brief The notification source for control-plane frames that come back late.

namespace mm {

/// @brief The @c data.event this source sends.
inline constexpr std::string_view kLateFrameEvent = "late-frame";

/// @brief Reports the first time the control-plane frames of this host come back later than a
/// wait the transport uses, to the log and to clients.
///
/// The transport counts how late every control-plane frame comes back, in the bands of its three
/// waits: 2 ms, 20 ms and 100 ms. This source reads those counts and speaks once when the latest
/// return first passes 20 ms and once when it first passes 100 ms, with the counts so far. A
/// support log from a machine nobody watched then carries the number that sets the waits. The
/// bands are read again from zero after @c DeviceManager::resetFrameStats or a new init.
///
/// It lives in the app for the reason @c busHealthSource does: a warning in a log is a policy.
///
/// The counts live in the transport and are read under the bus lock, so the revision is a tick of
/// @p interval rather than a counter: the source reads once per interval and decides then. A
/// transport that sends no frames (SPoE) answers nothing, so this is silent over it.
///
/// @param deviceManager  Borrowed. Must outlive the bus this source is registered with.
/// @param interval       How often the counts are read. A second, like the bus health.
node::NotificationBus::Source lateFrameSource(
    node::DeviceManager& deviceManager,
    std::chrono::milliseconds interval = std::chrono::seconds{1});

}  // namespace mm
