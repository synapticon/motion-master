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

void ProgressReporter::start(std::string_view id) {
  const std::lock_guard lock(mutex_);
  if (auto* step = findStep(steps_, id)) {
    step->status = ProgressStatus::kRunning;
    step->value = nullptr;
    step->error.reset();
  }
}

void ProgressReporter::succeed(std::string_view id) { succeed(id, nlohmann::json(nullptr)); }

void ProgressReporter::succeed(std::string_view id, nlohmann::json value) {
  const std::lock_guard lock(mutex_);
  if (auto* step = findStep(steps_, id)) {
    step->status = ProgressStatus::kSucceeded;
    step->value = std::move(value);
    step->error.reset();
  }
}

void ProgressReporter::fail(std::string_view id, std::string error) {
  const std::lock_guard lock(mutex_);
  if (auto* step = findStep(steps_, id)) {
    step->status = ProgressStatus::kFailed;
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
      {"steps", std::move(steps)},
  };
}

}  // namespace mm::node
