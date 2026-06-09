#pragma once

namespace mm::comm {

/// @brief When true on the calling thread, a driver demotes its per-transfer SDO debug logs to
///        @c trace. Set it via @c ScopedQuietSdoLog around background polling so a periodic poller
///        does not flood the log, while direct (user-initiated) reads keep their @c debug trace.
///
/// Thread-local (the comment, not the name, carries that): the flag is read on the same thread
/// that issues the transfer — SDO reads run synchronously on the caller's thread — so a scope set
/// by the caller reliably covers the driver's logging. Inline so the one instance is shared across
/// translation units.
inline thread_local bool sdoLogQuiet = false;

/// @brief RAII guard that suppresses per-transfer SDO debug logging for its scope (and restores the
///        previous state on exit, so it nests correctly).
class ScopedQuietSdoLog {
 public:
  ScopedQuietSdoLog() : previous_(sdoLogQuiet) { sdoLogQuiet = true; }
  ~ScopedQuietSdoLog() { sdoLogQuiet = previous_; }

  ScopedQuietSdoLog(const ScopedQuietSdoLog&) = delete;
  ScopedQuietSdoLog& operator=(const ScopedQuietSdoLog&) = delete;

 private:
  bool previous_;
};

}  // namespace mm::comm
