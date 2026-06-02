#pragma once

#include <spdlog/sinks/base_sink.h>

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace mm {

/// @brief Thread-safe spdlog sink that retains the most recent log entries in memory.
///
/// Stores up to @p capacity formatted log lines in a ring buffer.  Once full the
/// oldest entry is discarded to make room for each new one.  @c entries() returns
/// a snapshot of all buffered lines in chronological order.
///
/// Intended use: install alongside the console sink so the HTTP API can serve the
/// in-process log without touching the filesystem.
///
/// @tparam Mutex  Mutex type forwarded to @c spdlog::sinks::base_sink (use @c
///                std::mutex for multi-threaded sinks).
template <typename Mutex>
class RingLogSink : public spdlog::sinks::base_sink<Mutex> {
 public:
  /// @brief Constructs the sink with the given ring-buffer capacity.
  /// @param capacity  Maximum number of log lines to retain.  Defaults to 10 000.
  explicit RingLogSink(std::size_t capacity = 100000) : capacity_(capacity) {}

  /// @brief Returns a snapshot of all currently buffered log lines.
  ///
  /// Lines are in chronological order (oldest first).  Trailing newlines are
  /// stripped so each element is a plain text string suitable for JSON embedding.
  ///
  /// Not @c const because @c base_sink::mutex_ is not @c mutable.
  /// @return Vector of formatted log lines.
  std::vector<std::string> entries() {
    std::lock_guard<Mutex> lock(this->mutex_);
    return {buffer_.begin(), buffer_.end()};
  }

 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    spdlog::memory_buf_t formatted;
    this->formatter_->format(msg, formatted);
    std::string entry{formatted.data(), formatted.size()};
    if (!entry.empty() && entry.back() == '\n') {
      entry.pop_back();
    }
    if (buffer_.size() >= capacity_) {
      buffer_.pop_front();
    }
    buffer_.push_back(std::move(entry));
  }

  void flush_() override {}

 private:
  std::size_t capacity_;
  std::deque<std::string> buffer_;
};

/// @brief Convenience alias: @c RingLogSink with a @c std::mutex (multi-threaded).
using RingLogSinkMt = RingLogSink<std::mutex>;

}  // namespace mm
