#pragma once

/// @brief Interface for work executed once per game loop cycle.
///
/// Implementations must be non-blocking and must not allocate heap memory or
/// perform I/O on the calling thread.  Any async work (e.g. pushing data to a
/// WebSocket) must be handed off to another thread via a lock-free channel.
///
/// Tasks are registered with GameLoop::addTask() before run() is called and
/// are owned by the caller (the composition root).  GameLoop holds non-owning
/// pointers, so a task must outlive every call to run().
class CyclicTask {
 public:
  /// @brief Virtual destructor.
  virtual ~CyclicTask() = default;

  /// @brief Called once per cycle, after process data has been exchanged and
  ///        device parameters updated.
  ///
  /// Must return before the next cycle deadline.  Blocking here stalls the
  /// entire RT loop.
  virtual void execute() = 0;
};
