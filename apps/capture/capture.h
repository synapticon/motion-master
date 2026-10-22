#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string>

/// @file
/// @brief The platform step of the capture: every EtherCAT frame on an adapter, with the time the
/// kernel gave it, for a bounded time.

namespace mm::capture {

/// @brief Called for every frame, with nanoseconds since the Unix epoch and the frame from the
/// Ethernet header on. Called on the capturing thread, in time order.
using FrameSink = std::function<void(int64_t timestampNs, std::span<const uint8_t> bytes)>;

/// @brief Captures the EtherCAT frames that leave or enter @p adapter for @p duration.
///
/// Every frame is handed to @p sink. A frame the master sends and the same frame on its way back
/// are two calls, and the frame tells which it is. Returns when the time is up, or an error when
/// the adapter cannot be opened, which on Linux is a missing CAP_NET_RAW or a wrong name.
std::expected<void, std::string> captureFrames(const std::string& adapter,
                                               std::chrono::milliseconds duration,
                                               const FrameSink& sink);

}  // namespace mm::capture
