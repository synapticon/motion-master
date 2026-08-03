#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "node/device.h"
#include "node/device_manager.h"
#include "node/procedure.h"

namespace mm::node {

/// @brief A procedure's work: everything one run does, as a plain callable.
///
/// It receives the device already borrowed under the bus lock (so the reference is valid for the
/// call's whole duration), the reporter to record progress on, and the stop token to check between
/// steps. It takes a @c Device& rather than a profile view so that @c ProcedureManager stays as
/// profile-ignorant as @c DeviceManager — a body that needs a @c SomanetDrive binds one itself, and
/// the manager never names a profile type.
///
/// Returning an error fails the run. A body should normally fail the *step* that went wrong (via
/// the reporter) as well, so the snapshot says where it stopped; the returned string is what a
/// caller sees when the failure happened outside any step.
using ProcedureBody =
    std::function<std::expected<void, std::string>(Device&, ProgressReporter&, std::stop_token)>;

/// @brief Why a procedure operation could not be performed. A caller must branch on this — an HTTP
///        handler maps each kind to a different status — which is what earns a structured error
///        here rather than the usual @c std::string (see CLAUDE.md's no-exceptions mandate, and
///        @c libs/comm/foe_error.h for the shape). It keeps a string face so forwarding callers are
///        unaffected.
///
/// Two layers raise these, and the split is worth knowing: @c ProcedureManager produces only
/// @c kBusy and @c kUnknownDevice, because those are the only two things it can judge. Whether a
/// procedure *exists* and whether a request *is valid* are the catalogue's business
/// (@c procedure_catalogue.h) — the manager is told a body, never a name it could validate. One
/// error type across both keeps a handler's mapping in one place instead of translating between
/// two.
struct ProcedureError {
  enum class Kind : uint8_t {
    kBusy,              ///< Another procedure is already running on that device. → 409
    kUnknownDevice,     ///< No device holds that bus position. → 404
    kUnknownProcedure,  ///< No procedure by that name, or not one this device supports. → 404
    kInvalidRequest,    ///< The procedure exists but the request for it does not validate. → 400
  };

  Kind kind = Kind::kBusy;
  std::string message;

  /// @brief Exception-shaped accessor for the message, for call sites that prefer @c .what().
  const std::string& what() const { return message; }
};

/// @brief Streams the message, so `ASSERT_TRUE(r) << r.error()` and spdlog `{}` work unchanged.
inline std::ostream& operator<<(std::ostream& os, const ProcedureError& e) {
  return os << e.message;
}

/// @brief Runs off-RT command-and-wait procedures and remembers how each one went.
///
/// The @c MonitoringManager analogue for procedures: it owns the cancellable @c std::jthread each
/// run executes on, the per-device exclusion that stops two runs colliding, and the snapshot a
/// client polls. What it does *not* own is any knowledge of what a procedure does — bodies are
/// supplied at @c start, so adding a procedure never touches this class.
///
/// **Exclusion is per device, and it is the part a mutex cannot provide.** The driver's
/// control-plane lock serialises individual *transactions* (one SDO, one mailbox round-trip), but a
/// procedure is a multi-second span of many transactions interleaved with sleeps. "This device is
/// busy detecting its offset" is a span-level fact, so @c start rejects a second run on a device
/// that already has one, and the claim is released when the thread exits — success, failure or
/// cancellation alike.
///
/// **Progress is polled, never pushed.** This class holds no publish callback and names no
/// WebSocket. Each snapshot is an accumulating full-array state in which finished steps keep their
/// terminal status and value, which is what makes polling lossless: a step that starts and finishes
/// between two polls is still visible as succeeded. See @c ProcedureSnapshot.
///
/// **A snapshot outlives its run** — retained per (device, procedure) until the next run of the
/// same procedure replaces it — so a client that reconnects, or a user returning to a page, sees
/// how the last run went rather than an empty state.
///
/// **Retained snapshots are dropped when the device set is rebuilt.** Positions may remap across a
/// @c scan / @c reset, and a retained measurement rendered against a *different* physical drive is
/// worse than no measurement at all. The rebuild is noticed by watching
/// @c DeviceManager::topologyGeneration rather than by being told, which is what keeps
/// @c DeviceManager unaware that procedures exist. This is safe to do lazily because a rescan
/// **cannot overlap a running procedure**: the body runs inside @c withDevice, which holds the bus
/// lock shared for its whole duration, and @c scan needs it exclusively.
///
/// Thread-safe; @c start, @c snapshot and @c cancel may be called from any non-RT thread.
class ProcedureManager {
 public:
  /// @brief Binds the manager to the device set its procedures will run against.
  /// @param deviceManager  Must outlive this manager.
  explicit ProcedureManager(DeviceManager& deviceManager) : deviceManager_(deviceManager) {}

  /// @brief Requests cancellation of every running procedure and waits for them to finish.
  ~ProcedureManager();

  ProcedureManager(const ProcedureManager&) = delete;
  ProcedureManager& operator=(const ProcedureManager&) = delete;

  /// @brief Starts @p name on @p devicePosition, returning as soon as the run is under way.
  ///
  /// The device is checked to exist before anything is spawned, so an unknown position fails here
  /// rather than surfacing later as a failed snapshot. On success the run is immediately visible
  /// via @c snapshot with status @c kRunning and a bumped @c runCount.
  ///
  /// @param devicePosition  1-based bus position the procedure runs against.
  /// @param name            Procedure identifier, e.g. "os-command"; scopes the retained snapshot.
  /// @param steps           The procedure's step template, in order, all idle.
  /// @param body            The work to run (see @c ProcedureBody).
  /// @return Void once started, or why it could not be (see @c ProcedureError).
  std::expected<void, ProcedureError> start(uint16_t devicePosition, std::string name,
                                            std::vector<ProgressStep> steps, ProcedureBody body);

  /// @brief The current or last-known state of @p name on @p devicePosition.
  ///
  /// @return The snapshot, or @c std::nullopt if that procedure has never run on that device since
  ///         the last rebuild of the device set. (A caller that knows the procedure's step template
  ///         can render an all-idle snapshot itself; the manager does not invent one, because it is
  ///         only told a template when a run starts.)
  std::optional<ProcedureSnapshot> snapshot(uint16_t devicePosition, std::string_view name) const;

  /// @brief Asks the running @p name on @p devicePosition to stop.
  ///
  /// Cooperative: it requests the run's stop token and returns immediately. The body decides how
  /// promptly it notices — typically at its next step boundary or poll — after which the run
  /// finishes as @c kCancelled.
  ///
  /// @return True if a run was in flight and has been asked to stop; false if there was nothing to
  ///         cancel.
  bool cancel(uint16_t devicePosition, std::string_view name);

 private:
  /// One run of one procedure on one device: its progress, its outcome, and the thread doing it.
  /// Written by the running thread and read by pollers, so everything the thread touches after
  /// start-up is atomic — which is what lets the thread finish without taking the manager's mutex,
  /// and so lets the manager be destroyed without deadlocking against a run that is completing.
  struct Run {
    std::shared_ptr<ProgressReporter> reporter;  ///< Set once at start; never reassigned.
    uint32_t runCount = 0;                       ///< Set once at start, under the manager's mutex.
    int64_t startedAt = 0;                       ///< Set once at start, under the manager's mutex.
    std::atomic<ProcedureStatus> status{ProcedureStatus::kRunning};
    std::atomic<int64_t> finishedAt{0};  ///< Epoch ms; 0 while running.
    std::atomic<bool> running{true};     ///< Cleared last, once the outcome is recorded.
    std::jthread thread;

    /// @brief Records why the run failed when no step captured it. Called once, by the running
    ///        thread, on its way out.
    void setError(std::string reason) {
      const std::lock_guard lock(errorMutex_);
      error_ = std::move(reason);
    }

    /// @brief The out-of-step failure reason, if there was one.
    std::optional<std::string> error() const {
      const std::lock_guard lock(errorMutex_);
      return error_;
    }

   private:
    /// The one field here that cannot be an atomic. libc++ does not implement
    /// @c std::atomic<std::shared_ptr<T>> at all — it is unimplemented rather than merely gated
    /// behind -fexperimental-library, so no flag makes the macOS build accept it — and a string is
    /// not trivially copyable, so no plain atomic will take one either.
    ///
    /// A mutex of its own preserves the property that actually matters here, which is *not* being
    /// lock-free: the finishing thread must never need **the manager's** mutex, so that the
    /// destructor can collect the running threads under that lock and join them after releasing it.
    /// This mutex is per-run, uncontended, and held for one assignment.
    mutable std::mutex errorMutex_;
    std::optional<std::string> error_;
  };

  using Key = std::pair<uint16_t, std::string>;

  /// Drops every retained snapshot if the device set has been rebuilt since the last check.
  /// The manager's mutex must be held. Const, and the state it clears is mutable, because this is
  /// lazy invalidation of results that a rescan already made meaningless — a poll has to honour it
  /// just as a start does, or a client reading after a rescan would be handed another drive's
  /// measurement.
  void discardIfRescanned() const;

  DeviceManager& deviceManager_;
  mutable std::mutex mutex_;                          ///< Guards runs_ and topologyGeneration_.
  mutable uint64_t topologyGeneration_ = 0;           ///< Last observed device-set generation.
  mutable std::map<Key, std::shared_ptr<Run>> runs_;  ///< Latest run per (device, procedure).
};

}  // namespace mm::node
