#pragma once

// The full header, not json_fwd: ProgressStep holds a json by value (see its `value` member).
#include <cstdint>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mm::node {

/// @brief Status of one step within a procedure.
///
/// A step never reports cancellation: cancelling stops the *procedure*, leaving whichever step was
/// running as @c kRunning and the ones after it @c kIdle, which is a truthful record of how far it
/// got. The cancellation itself is reported by @c ProcedureStatus.
enum class ProgressStatus : uint8_t {
  kIdle,       ///< Not started (the initial state of every step in a template).
  kRunning,    ///< Started and not yet finished.
  kSucceeded,  ///< Finished; @c ProgressStep::value carries its measurement, if it has one.
  kFailed,     ///< Finished badly; @c ProgressStep::error says why.
};

/// @brief Human-readable name of a step status (for logging / JSON). Never returns @c nullptr.
constexpr std::string_view toString(ProgressStatus status) {
  switch (status) {
    case ProgressStatus::kIdle:
      return "idle";
    case ProgressStatus::kRunning:
      return "running";
    case ProgressStatus::kSucceeded:
      return "succeeded";
    case ProgressStatus::kFailed:
      return "failed";
  }
  return "unknown";
}

/// @brief Overall status of a procedure run — the single field a polling client checks.
///
/// The step statuses plus @c kCancelled, which exists because folding a user cancel into
/// @c kFailed would lose the difference between "the drive could not do it" and "I stopped it" —
/// exactly the distinction someone returning to a page needs. A polling loop is
/// @c while(status == kRunning).
enum class ProcedureStatus : uint8_t {
  kIdle,       ///< Never run on this device (or cleared by a rescan).
  kRunning,    ///< A run is in progress.
  kSucceeded,  ///< The last run completed successfully.
  kFailed,     ///< The last run failed; the failing step carries the reason.
  kCancelled,  ///< The last run was stopped by the user before it finished.
};

/// @brief Human-readable name of a procedure status (for logging / JSON). Never @c nullptr.
constexpr std::string_view toString(ProcedureStatus status) {
  switch (status) {
    case ProcedureStatus::kIdle:
      return "idle";
    case ProcedureStatus::kRunning:
      return "running";
    case ProcedureStatus::kSucceeded:
      return "succeeded";
    case ProcedureStatus::kFailed:
      return "failed";
    case ProcedureStatus::kCancelled:
      return "cancelled";
  }
  return "unknown";
}

/// @brief One step of a procedure — an entry in the fixed, ordered array a procedure publishes.
///
/// @c id identifies the step within its procedure and is stable across runs (the client keys its
/// labels off it).
///
/// @c value is a @c nlohmann::json because what a step produces is entirely up to its procedure: a
/// resistance is a number, a pole-pair count an integer, a raw OS command's reply a byte array, and
/// open phase detection reports eight per-terminal results at once. **Each procedure defines its
/// own value type** — a struct with a @c to_json beside the procedure — and
/// @c ProgressReporter::succeed converts it here, so the body stays typed and self-documenting
/// while the stored form stays uniform. That uniformity is not incidental: @c ProcedureManager
/// keeps every procedure's snapshot in one map and serves them through one endpoint, so a
/// @c ProgressStep<T> would buy type safety in the body and force type erasure one layer above it.
/// A null @c value means the step produced nothing.
///
/// What the value still does *not* carry is presentation — whether 0.123 renders as "123 mΩ" or 1
/// as "Inverted" is the client's decision, keyed off the step id.
struct ProgressStep {
  std::string id;                                 ///< Stable identifier, e.g. "phase-resistance".
  ProgressStatus status = ProgressStatus::kIdle;  ///< How far this step got.
  nlohmann::json value;                           ///< What the step produced; null if nothing.
  std::optional<std::string> error;               ///< Why the step failed.
};
void to_json(nlohmann::json& j, const ProgressStep& step);

/// @brief The complete state of a procedure on one device — everything a client polling
///        @c GET /api/devices/:pos/procedures/:name receives.
///
/// This is an **accumulating snapshot, not an event**: every finished step keeps its terminal
/// status and its measured value for as long as the snapshot is retained. That is what makes
/// polling lossless — a step that both starts and finishes between two polls is still visible as
/// @c kSucceeded with its value in the next one, so a client that never opens a WebSocket cannot
/// miss a result. Only the transient @c kRunning blip on a fast step can be skipped, and it
/// carries no data.
///
/// A device that has never run the procedure still yields a well-formed snapshot — @c kIdle,
/// @c runCount 0, no timestamps, every step @c kIdle from the template — so a client renders one
/// component with no empty-state special case.
struct ProcedureSnapshot {
  ProcedureStatus status = ProcedureStatus::kIdle;  ///< The one field a polling loop checks.

  /// @brief How many runs have been *accepted* on this device since the last rescan (a rejected
  ///        start does not count).
  ///
  /// Doubles as a generation counter, which is what makes polling safe without a run id: a run
  /// started elsewhere that begins and finishes between two polls would otherwise be invisible,
  /// and its result silently read as the run being watched. A changed @c runCount says "this is a
  /// different run"; @c (devicePosition, procedureName, runCount) identifies one uniquely.
  uint32_t runCount = 0;

  std::optional<int64_t> startedAt;   ///< Epoch ms the run began; absent when never run.
  std::optional<int64_t> finishedAt;  ///< Epoch ms the run ended; absent while running.
  std::vector<ProgressStep> steps;    ///< The ordered step array, always the full template.
};
void to_json(nlohmann::json& j, const ProcedureSnapshot& snapshot);

/// @brief The handle a procedure body uses to report where it has got to.
///
/// A body is a plain function over one of these — @c runXxx(SomanetDrive&, ProgressReporter&,
/// std::stop_token) — which is what keeps the orchestration transport-free and testable: a test
/// drives the body with its own reporter and asserts the resulting step array, with no manager, no
/// thread, and no HTTP anywhere near it.
///
/// Seeded from the procedure's step template (ids in order, all @c kIdle) and mutated in place as
/// the run progresses. Thread-safe: the body writes from its own thread while a poll reads
/// @c steps from an HTTP thread.
class ProgressReporter {
 public:
  /// @brief Seeds the reporter from a procedure's step template.
  explicit ProgressReporter(std::vector<ProgressStep> steps) : steps_(std::move(steps)) {}

  /// @brief Marks the step @p id as running, clearing any value or error left by a previous run.
  void start(std::string_view id);

  /// @brief Marks the step @p id as succeeded with no value — the step did something rather than
  ///        measured something.
  void succeed(std::string_view id);

  /// @brief Marks the step @p id as succeeded, recording what it produced.
  ///
  /// @p value is anything convertible to JSON: a number, a string, a byte vector, or — the usual
  /// case for a measurement — a procedure-specific struct with its own @c to_json declared beside
  /// the procedure. The conversion happens here so each body keeps its own typed vocabulary
  /// without every procedure needing a distinct step or snapshot type.
  template <typename T>
  void succeed(std::string_view id, const T& value) {
    succeed(id, nlohmann::json(value));
  }

  /// @brief Marks the step @p id as succeeded with an already-built JSON value.
  void succeed(std::string_view id, nlohmann::json value);

  /// @brief Marks the step @p id as failed, recording @p error as the reason.
  void fail(std::string_view id, std::string error);

  /// @brief A copy of the current step array — the snapshot's @c steps.
  std::vector<ProgressStep> steps() const;

 private:
  mutable std::mutex mutex_;
  std::vector<ProgressStep> steps_;
};

}  // namespace mm::node
