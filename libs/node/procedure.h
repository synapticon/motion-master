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

  /// @brief Why the run failed, when no step captured it.
  ///
  /// A body normally fails the step that went wrong, so the reason is visible where it happened
  /// and this stays empty. It is for the failure that belongs to no step — the device turning out
  /// not to be the right kind, say — which would otherwise leave a run marked failed with nothing
  /// anywhere saying why.
  std::optional<std::string> error;
};
void to_json(nlohmann::json& j, const ProcedureSnapshot& snapshot);

/// @brief A well-formed snapshot for a procedure that has never run — idle, no timestamps, and
///        every step idle from @p steps.
///
/// @c ProcedureManager cannot produce this — it is only told a step template when a run starts — so
/// it is the job of whoever holds the template (the catalogue). With it, "never run" and "ran" have
/// the same shape on the wire and a client renders one component with no empty-state branch, which
/// is the whole point of the accumulating-snapshot model.
ProcedureSnapshot idleSnapshot(std::vector<ProgressStep> steps);

/// @brief A procedure's step template: the given ids, in order, all idle.
///
/// Every procedure declares its steps this way, which is why it lives here rather than beside any
/// one family of them.
std::vector<ProgressStep> stepsFrom(std::initializer_list<std::string_view> ids);

/// @brief What kind of value a procedure parameter takes — the client's cue for which control to
///        render, and the only vocabulary a generic form has to understand.
///
/// Deliberately a short closed list rather than a JSON Schema fragment: a client renders a control
/// per kind, and every kind here is one a procedure actually asks for. A new kind is added when a
/// procedure needs it, which keeps the renderer as small as the set of things procedures take.
enum class ParameterType : uint8_t {
  kInteger,      ///< A whole number, bounded by @c minValue / @c maxValue.
  kBoolean,      ///< A checkbox.
  kEnum,         ///< One of @c options — a value the client picks rather than types.
  kByteArray,    ///< Exactly @c length byte values (the raw OS command's request bytes).
  kString,       ///< Free text — a filename, a label.
  kStringArray,  ///< A list of free-text values the user edits as a list.
  kFile,         ///< A file's contents, base64-encoded; a client renders a file picker.
};

/// @brief Human-readable name of a parameter type (for JSON). Never returns @c nullptr.
constexpr std::string_view toString(ParameterType type) {
  switch (type) {
    case ParameterType::kInteger:
      return "integer";
    case ParameterType::kBoolean:
      return "boolean";
    case ParameterType::kEnum:
      return "enum";
    case ParameterType::kByteArray:
      return "byteArray";
    case ParameterType::kString:
      return "string";
    case ParameterType::kStringArray:
      return "stringArray";
    case ParameterType::kFile:
      return "file";
  }
  return "unknown";
}

/// @brief One choice of a @c kEnum parameter: the value that goes on the wire, and its label.
struct ParameterOption {
  nlohmann::json value;  ///< What the request body carries when this option is chosen.
  std::string title;     ///< What the user picks, e.g. "Encoder 1".
};
void to_json(nlohmann::json& j, const ParameterOption& option);

/// @brief One named parameter a procedure accepts — enough for a client to render a field for it
///        and for a user to know what to put in.
///
/// **Descriptive, not authoritative.** The server validates every request in the procedure's own
/// parse function whatever a client sends; this is what lets a client build a sensible form and
/// catch a mistake before a request goes out. The two live next to each other (a
/// @c parseXxxRequest beside the @c xxxParameters that describes it) precisely so they cannot
/// drift.
///
/// **A parameter with a default is optional; one without is required.** That is the whole rule —
/// there is no separate required flag to contradict the default, and a body that omits an optional
/// parameter gets exactly what the descriptor advertised.
///
/// The three type-specific fields apply only where their comments say so, and are absent from the
/// JSON otherwise.
///
/// @c name identifies and @c title displays, exactly as on the @c ProcedureDescriptor this is
/// nested in — the two arrive in the same response, so one rule covers both. The value fields are
/// named as @c DeviceParameter names its own (@c defaultValue, @c minValue, @c maxValue); there is
/// no current @c value, because a parameter description says what a request *may* carry, never what
/// one did.
struct ProcedureParameter {
  std::string name;         ///< The key in the request body, e.g. "registerAddress".
  std::string title;        ///< Short label, e.g. "Register address".
  std::string description;  ///< What it does and what a sensible value is.
  ParameterType type = ParameterType::kInteger;

  /// What the procedure uses when the request omits this parameter; null makes it required.
  nlohmann::json defaultValue;

  std::optional<int64_t> minValue;       ///< @c kInteger only — smallest accepted value.
  std::optional<int64_t> maxValue;       ///< @c kInteger only — largest accepted value.
  std::optional<size_t> length;          ///< @c kByteArray only — the exact number of bytes.
  std::vector<ParameterOption> options;  ///< @c kEnum only — the values that may be chosen.

  /// @brief Whether a request must carry this parameter — true exactly when it has no default.
  bool required() const { return defaultValue.is_null(); }
};
void to_json(nlohmann::json& j, const ProcedureParameter& parameter);

/// @brief A whole-number parameter accepting @p minValue to @p maxValue inclusive.
///
/// One factory per type, rather than a braced aggregate at each call site, because the fields are
/// type-specific: only an integer has bounds, only an enum has options, only a byte array has a
/// length. A factory makes the combination that applies the only one that can be written.
///
/// @param defaultValue  What an omitting request gets; pass @c nullptr to make it required.
ProcedureParameter integerParameter(std::string name, std::string title, std::string description,
                                    nlohmann::json defaultValue, int64_t minValue,
                                    int64_t maxValue);

/// @brief A true/false parameter. @p defaultValue as in @c integerParameter.
ProcedureParameter booleanParameter(std::string name, std::string title, std::string description,
                                    nlohmann::json defaultValue);

/// @brief A parameter whose value is one of @p options. @p defaultValue as in @c integerParameter,
///        and it should be one of the option values.
ProcedureParameter enumParameter(std::string name, std::string title, std::string description,
                                 nlohmann::json defaultValue, std::vector<ParameterOption> options);

/// @brief A parameter taking exactly @p length byte values.
///
/// Always required: a default set of command bytes would be a command nobody asked to run.
ProcedureParameter byteArrayParameter(std::string name, std::string title, std::string description,
                                      size_t length);

/// @brief A free-text parameter. @p defaultValue as in @c integerParameter.
ProcedureParameter stringParameter(std::string name, std::string title, std::string description,
                                   nlohmann::json defaultValue);

/// @brief A parameter taking a list of strings, which the client renders as an editable list.
///
/// @param defaultValue  A JSON array used when the request omits the parameter; pass @c nullptr to
///                      make it required. An **empty array is a meaningful default** and not the
///                      same as no default — "skip nothing" is a real answer — so a caller that
///                      wants the parameter optional-with-nothing-selected passes
///                      @c nlohmann::json::array().
ProcedureParameter stringArrayParameter(std::string name, std::string title,
                                        std::string description, nlohmann::json defaultValue);

/// @brief A parameter carrying a whole file, base64-encoded in a JSON string.
///
/// The transport is base64 because JSON has no binary type and a procedure's parameters travel in
/// its request body. Decode with @c mm::core::base64Decode.
///
/// @param defaultValue  As in @c integerParameter; a file parameter is normally required, but one
///                      that has an alternative source (a package already on the server, named by
///                      another parameter) is optional.
ProcedureParameter fileParameter(std::string name, std::string title, std::string description,
                                 nlohmann::json defaultValue);

/// @brief What a procedure *is*, independent of any run — the half of the catalogue a client needs
///        to render a control for it.
///
/// The text lives here, on the server, rather than in each client: the house rule that every action
/// control carries a description *and its caveats* means it has to exist somewhere, and duplicating
/// it per client is how it goes stale. A client renders whatever the catalogue reports, so it stays
/// in step with the server's procedure set without hard-coding an entry per procedure.
///
/// What it deliberately does *not* carry is presentation. Formatting a step's value is the client's
/// business (see @c ProgressStep) — whether 0.123 renders as "123 mΩ" is a decision the server has
/// no business making.
struct ProcedureDescriptor {
  std::string name;   ///< Identifier: the URL segment, and the key its snapshot is retained under.
  std::string title;  ///< Short human-readable name, e.g. "OS command".
  std::string description;           ///< What the procedure does, in a sentence or two.
  std::vector<std::string> caveats;  ///< What a user must know before running it; may be empty.
  bool movesMotor = false;           ///< True if running it can move the shaft.
  bool requiresEnabled = false;      ///< True if the drive must be enabled first.

  /// What the request body may carry, in the order a client should present it; empty for a
  /// procedure that takes none (most of them — a procedure's timings are properties of the command
  /// it issues, not a caller's choice).
  std::vector<ProcedureParameter> parameters;

  /// The ordered step template, all idle — both what a run is seeded with and what an idle snapshot
  /// is rendered from.
  std::vector<ProgressStep> steps;
};
void to_json(nlohmann::json& j, const ProcedureDescriptor& descriptor);

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
