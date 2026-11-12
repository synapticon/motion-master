#pragma once

#include <atomic>
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

/// @brief Called once the adapter is open and before the first frame. An error ends the capture
///        before it starts.
using ReadyFn = std::function<std::expected<void, std::string>()>;

/// @brief Captures the EtherCAT frames that leave or enter @p adapter for @p duration, or until
///        @p stop is set.
///
/// @p ready runs once the adapter is open, so a caller can create its output then: a file that
/// exists means the capture runs.
///
/// Every frame is handed to @p sink. A frame the master sends and the same frame on its way back
/// are two calls, and the frame tells which it is. Returns when the time is up or @p stop is set,
/// or an error when the adapter cannot be opened, which on Linux is a missing CAP_NET_RAW or a
/// wrong name. @p stop is read at least every 100 ms, so a signal handler can set it and the
/// capture ends within that time with every frame it read.
std::expected<void, std::string> captureFrames(const std::string& adapter,
                                               std::chrono::milliseconds duration,
                                               const FrameSink& sink, const std::atomic<bool>& stop,
                                               const ReadyFn& ready);

}  // namespace mm::capture
