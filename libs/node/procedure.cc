#include "node/procedure.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace mm::node {

namespace {

// Finds a step by id. A miss is a programming error — a body naming a step its own template does
// not declare — so it is logged rather than silently ignored, and never fails the run: a
// mis-reported step is a reporting bug, not a reason to abandon a measurement on real hardware.
ProgressStep* findStep(std::vector<ProgressStep>& steps, std::string_view id) {
  auto it = std::ranges::find(steps, id, &ProgressStep::id);
  if (it == steps.end()) {
    spdlog::error("Procedure reported step '{}', which is not in its template", id);
    return nullptr;
  }
  return &*it;
}

}  // namespace

// Every status change is logged, which is what lets a long procedure's progress be read off the
// same terminal as the operations it is performing. A firmware install spends ten seconds inside a
// single step, so "which step is this output from" is otherwise a guess — and if a client ever
// disagrees with this timeline, the disagreement is in the client rather than in the run.
void ProgressReporter::start(std::string_view id) {
  const std::lock_guard lock(mutex_);
  if (auto* step = findStep(steps_, id)) {
    step->status = ProgressStatus::kRunning;
    step->value = nullptr;
    step->error.reset();
    spdlog::debug("Procedure step '{}': running", id);
  }
}

void ProgressReporter::succeed(std::string_view id) { succeed(id, nlohmann::json(nullptr)); }

void ProgressReporter::succeed(std::string_view id, nlohmann::json value) {
  const std::lock_guard lock(mutex_);
  if (auto* step = findStep(steps_, id)) {
    step->status = ProgressStatus::kSucceeded;
    step->value = std::move(value);
    step->error.reset();
    spdlog::debug("Procedure step '{}': succeeded", id);
  }
}

void ProgressReporter::fail(std::string_view id, std::string error) {
  const std::lock_guard lock(mutex_);
  if (auto* step = findStep(steps_, id)) {
    step->status = ProgressStatus::kFailed;
    spdlog::debug("Procedure step '{}': failed — {}", id, error);
    step->error = std::move(error);
  }
}

std::vector<ProgressStep> ProgressReporter::steps() const {
  const std::lock_guard lock(mutex_);
  return steps_;
}

void to_json(nlohmann::json& j, const ProgressStep& step) {
  j = nlohmann::json{
      {"id", step.id},
      {"status", toString(step.status)},
  };
  // Both are omitted rather than emitted as null: a step that produced nothing and a step that did
  // not fail should not clutter every snapshot with empty fields.
  if (!step.value.is_null()) {
    j["value"] = step.value;
  }
  if (step.error) {
    j["error"] = *step.error;
  }
}

std::vector<ProgressStep> stepsFrom(std::initializer_list<std::string_view> ids) {
  std::vector<ProgressStep> steps;
  steps.reserve(ids.size());
  for (auto id : ids) {
    ProgressStep step;
    step.id = std::string(id);
    steps.push_back(std::move(step));
  }
  return steps;
}

ProcedureSnapshot idleSnapshot(std::vector<ProgressStep> steps) {
  ProcedureSnapshot snapshot;
  snapshot.steps = std::move(steps);
  return snapshot;
}

void to_json(nlohmann::json& j, const ProcedureSnapshot& snapshot) {
  j = nlohmann::json{
      {"status", toString(snapshot.status)},
      {"runCount", snapshot.runCount},
      {"steps", snapshot.steps},
  };
  if (snapshot.startedAt) {
    j["startedAt"] = *snapshot.startedAt;
  }
  if (snapshot.finishedAt) {
    j["finishedAt"] = *snapshot.finishedAt;
  }
  if (snapshot.error) {
    j["error"] = *snapshot.error;
  }
}

void to_json(nlohmann::json& j, const ParameterOption& option) {
  j = nlohmann::json{{"value", option.value}, {"title", option.title}};
}

void to_json(nlohmann::json& j, const ProcedureParameter& parameter) {
  j = nlohmann::json{
      {"name", parameter.name},
      {"title", parameter.title},
      {"description", parameter.description},
      {"type", toString(parameter.type)},
      // Derived from defaultValue rather than stored, so the two can never disagree — but emitted,
      // so a client renders a required field without having to know the rule.
      {"required", parameter.required()},
  };
  // The type-specific fields are omitted where they do not apply, which is the difference that
  // matters here: an absent `options` says "not a choice", where an empty array would say "a choice
  // with nothing to choose".
  if (!parameter.defaultValue.is_null()) {
    j["defaultValue"] = parameter.defaultValue;
  }
  if (parameter.minValue) {
    j["minValue"] = *parameter.minValue;
  }
  if (parameter.maxValue) {
    j["maxValue"] = *parameter.maxValue;
  }
  if (parameter.length) {
    j["length"] = *parameter.length;
  }
  if (!parameter.options.empty()) {
    j["options"] = parameter.options;
  }
}

ProcedureParameter integerParameter(std::string name, std::string title, std::string description,
                                    nlohmann::json defaultValue, int64_t minValue,
                                    int64_t maxValue) {
  ProcedureParameter parameter;
  parameter.name = std::move(name);
  parameter.title = std::move(title);
  parameter.description = std::move(description);
  parameter.type = ParameterType::kInteger;
  parameter.defaultValue = std::move(defaultValue);
  parameter.minValue = minValue;
  parameter.maxValue = maxValue;
  return parameter;
}

ProcedureParameter booleanParameter(std::string name, std::string title, std::string description,
                                    nlohmann::json defaultValue) {
  ProcedureParameter parameter;
  parameter.name = std::move(name);
  parameter.title = std::move(title);
  parameter.description = std::move(description);
  parameter.type = ParameterType::kBoolean;
  parameter.defaultValue = std::move(defaultValue);
  return parameter;
}

ProcedureParameter enumParameter(std::string name, std::string title, std::string description,
                                 nlohmann::json defaultValue,
                                 std::vector<ParameterOption> options) {
  ProcedureParameter parameter;
  parameter.name = std::move(name);
  parameter.title = std::move(title);
  parameter.description = std::move(description);
  parameter.type = ParameterType::kEnum;
  parameter.defaultValue = std::move(defaultValue);
  parameter.options = std::move(options);
  return parameter;
}

ProcedureParameter byteArrayParameter(std::string name, std::string title, std::string description,
                                      size_t length) {
  ProcedureParameter parameter;
  parameter.name = std::move(name);
  parameter.title = std::move(title);
  parameter.description = std::move(description);
  parameter.type = ParameterType::kByteArray;
  parameter.length = length;
  return parameter;
}

ProcedureParameter stringParameter(std::string name, std::string title, std::string description,
                                   nlohmann::json defaultValue) {
  ProcedureParameter parameter;
  parameter.name = std::move(name);
  parameter.title = std::move(title);
  parameter.description = std::move(description);
  parameter.type = ParameterType::kString;
  parameter.defaultValue = std::move(defaultValue);
  return parameter;
}

ProcedureParameter stringArrayParameter(std::string name, std::string title,
                                        std::string description, nlohmann::json defaultValue) {
  ProcedureParameter parameter;
  parameter.name = std::move(name);
  parameter.title = std::move(title);
  parameter.description = std::move(description);
  parameter.type = ParameterType::kStringArray;
  parameter.defaultValue = std::move(defaultValue);
  return parameter;
}

ProcedureParameter fileParameter(std::string name, std::string title, std::string description,
                                 nlohmann::json defaultValue) {
  ProcedureParameter parameter;
  parameter.name = std::move(name);
  parameter.title = std::move(title);
  parameter.description = std::move(description);
  parameter.type = ParameterType::kFile;
  parameter.defaultValue = std::move(defaultValue);
  return parameter;
}

void to_json(nlohmann::json& j, const ProcedureDescriptor& descriptor) {
  // The template is emitted as bare step ids, not as ProgressSteps: every entry would carry
  // status "idle" and no value, which says nothing a client can use. Live per-step status belongs
  // to the snapshot; the descriptor only declares which steps exist, and in what order.
  auto steps = nlohmann::json::array();
  for (const auto& step : descriptor.steps) {
    steps.push_back(step.id);
  }
  // Every field is emitted unconditionally, empty or not — a client renders one control per
  // procedure, and branching on absent-versus-empty would be a distinction without a difference.
  j = nlohmann::json{
      {"name", descriptor.name},
      {"title", descriptor.title},
      {"description", descriptor.description},
      {"caveats", descriptor.caveats},
      {"movesMotor", descriptor.movesMotor},
      {"requiresEnabled", descriptor.requiresEnabled},
      {"parameters", descriptor.parameters},
      {"steps", std::move(steps)},
  };
}

}  // namespace mm::node
