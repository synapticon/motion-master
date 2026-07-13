#pragma once

#include <cstdint>

/// @brief Per-cycle timing context passed to every CyclicTask::execute().
///
/// The game loop sleeps to a fixed deadline grid (CyclicTimer), so when a cycle
/// is missed — an overrun or a scheduling stall, routine on a non-RT OS — the
/// timer skips the backlog and re-syncs rather than firing missed cycles
/// back-to-back.  A task that generates a time-indexed target (e.g. trajectory
/// playback) needs to know that happened; a task that only acts on the freshest
/// state (e.g. ProcessDataTask) ignores this entirely.
///
/// The two views of the same event:
/// - `gridTick` — the absolute cycle index on the deadline grid: periods
///   elapsed since run() began.  It advances by 1 on a normal cycle and by
///   1 + skipped after a stall, so a target computed as a pure function of
///   `gridTick` (e.g. `cursor = gridTick - startTick`) stays on the real-time
///   schedule across skips with no per-cycle accumulation — the same
///   drift-free principle as the absolute-deadline timer itself.
/// - `skipped` — cycles skipped immediately before this one; 0 on the normal
///   path.  For anomaly policy (e.g. an opt-in strict mode that aborts a move
///   when the RT guarantee is badly violated), not for normal advancement.
struct CycleContext {
  uint64_t gridTick = 0;
  uint64_t skipped = 0;
};

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
  ///
  /// @param ctx  Per-cycle timing context (grid index + skipped count).  Tasks
  ///             that act only on the freshest state may ignore it.
  virtual void execute(const CycleContext& ctx) = 0;
};
