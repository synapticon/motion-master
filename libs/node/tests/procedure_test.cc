#include "node/procedure.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

using mm::node::ProcedureSnapshot;
using mm::node::ProcedureStatus;
using mm::node::ProgressReporter;
using mm::node::ProgressStatus;
using mm::node::ProgressStep;

// One idle step — the shape a procedure's template is built from.
ProgressStep idleStep(std::string id) {
  ProgressStep step;
  step.id = std::move(id);
  return step;
}

// A procedure's step template: ids in order, everything idle.
std::vector<ProgressStep> makeTemplate() { return {idleStep("first"), idleStep("second")}; }

// A procedure-specific value type — the shape a real measurement step reports. Declared with its
// own to_json exactly as a procedure would declare it beside its body.
struct PhaseResult {
  double resistance = 0.0;
  bool inverted = false;
};

void to_json(nlohmann::json& j, const PhaseResult& r) {
  j = nlohmann::json{{"resistance", r.resistance}, {"inverted", r.inverted}};
}

TEST(ProgressReporter, StartsIdleFromItsTemplate) {
  ProgressReporter reporter(makeTemplate());
  const auto steps = reporter.steps();
  ASSERT_EQ(steps.size(), 2u);
  EXPECT_EQ(steps[0].id, "first");
  EXPECT_EQ(steps[0].status, ProgressStatus::kIdle);
  EXPECT_TRUE(steps[0].value.is_null());
  EXPECT_FALSE(steps[0].error.has_value());
}

TEST(ProgressReporter, RecordsATransitionPerStepIndependently) {
  ProgressReporter reporter(makeTemplate());
  reporter.start("first");

  auto steps = reporter.steps();
  EXPECT_EQ(steps[0].status, ProgressStatus::kRunning);
  EXPECT_EQ(steps[1].status, ProgressStatus::kIdle) << "a later step must stay untouched";

  reporter.succeed("first", 1.5);
  reporter.start("second");
  steps = reporter.steps();
  EXPECT_EQ(steps[0].status, ProgressStatus::kSucceeded);
  EXPECT_EQ(steps[0].value, 1.5);
  EXPECT_EQ(steps[1].status, ProgressStatus::kRunning);
}

TEST(ProgressReporter, CarriesAnyJsonConvertibleValue) {
  ProgressReporter reporter(
      {idleStep("number"), idleStep("bytes"), idleStep("text"), idleStep("record")});
  reporter.succeed("number", 42);
  reporter.succeed("bytes", std::vector<uint8_t>{1, 2, 3});
  reporter.succeed("text", std::string("inverted"));
  reporter.succeed("record", PhaseResult{.resistance = 0.123, .inverted = true});

  const auto steps = reporter.steps();
  EXPECT_EQ(steps[0].value, 42);
  EXPECT_EQ(steps[1].value, (std::vector<uint8_t>{1, 2, 3}));
  EXPECT_EQ(steps[2].value, "inverted");
  EXPECT_EQ(steps[3].value["resistance"], 0.123);
  EXPECT_EQ(steps[3].value["inverted"], true);
}

TEST(ProgressReporter, SucceedsWithoutAValue) {
  ProgressReporter reporter(makeTemplate());
  reporter.succeed("first");
  const auto steps = reporter.steps();
  EXPECT_EQ(steps[0].status, ProgressStatus::kSucceeded);
  EXPECT_TRUE(steps[0].value.is_null());
}

TEST(ProgressReporter, RecordsAFailureReason) {
  ProgressReporter reporter(makeTemplate());
  reporter.start("first");
  reporter.fail("first", "current amplitude too low");

  const auto steps = reporter.steps();
  EXPECT_EQ(steps[0].status, ProgressStatus::kFailed);
  ASSERT_TRUE(steps[0].error.has_value());
  EXPECT_EQ(*steps[0].error, "current amplitude too low");
}

TEST(ProgressReporter, StartClearsTheValueAndErrorOfAPreviousRun) {
  ProgressReporter reporter(makeTemplate());
  reporter.succeed("first", 1.5);
  reporter.fail("second", "boom");

  reporter.start("first");
  reporter.start("second");
  const auto steps = reporter.steps();
  EXPECT_TRUE(steps[0].value.is_null());
  EXPECT_FALSE(steps[1].error.has_value());
}

TEST(ProgressReporter, IgnoresAStepNotInTheTemplate) {
  ProgressReporter reporter(makeTemplate());
  reporter.succeed("nonexistent", 1.0);  // logged as a programming error, must not throw or add
  EXPECT_EQ(reporter.steps().size(), 2u);
}

// --- JSON ----------------------------------------------------------------------------------------

TEST(ProcedureSnapshotJson, OmitsAbsentFields) {
  ProcedureSnapshot snapshot;
  snapshot.steps = makeTemplate();
  const nlohmann::json j = snapshot;

  EXPECT_EQ(j["status"], "idle");
  EXPECT_EQ(j["runCount"], 0);
  EXPECT_FALSE(j.contains("startedAt"));
  EXPECT_FALSE(j.contains("finishedAt"));
  // A never-run snapshot is still well formed: the full template, so a client renders one
  // component with no empty-state special case.
  ASSERT_EQ(j["steps"].size(), 2u);
  EXPECT_EQ(j["steps"][0]["id"], "first");
  EXPECT_EQ(j["steps"][0]["status"], "idle");
  EXPECT_FALSE(j["steps"][0].contains("value"));
  EXPECT_FALSE(j["steps"][0].contains("error"));
}

TEST(ProcedureSnapshotJson, RendersACompletedRun) {
  ProgressReporter reporter(makeTemplate());
  reporter.succeed("first", PhaseResult{.resistance = 0.123, .inverted = true});
  reporter.fail("second", "no response");

  ProcedureSnapshot snapshot;
  snapshot.status = ProcedureStatus::kFailed;
  snapshot.runCount = 3;
  snapshot.startedAt = 1735821000123;
  snapshot.finishedAt = 1735821042456;
  snapshot.steps = reporter.steps();
  const nlohmann::json j = snapshot;

  EXPECT_EQ(j["status"], "failed");
  EXPECT_EQ(j["runCount"], 3);
  EXPECT_EQ(j["startedAt"], 1735821000123);
  EXPECT_EQ(j["finishedAt"], 1735821042456);
  EXPECT_EQ(j["steps"][0]["value"]["resistance"], 0.123);
  EXPECT_EQ(j["steps"][1]["error"], "no response");
}

TEST(ProcedureStatusJson, NamesCancellationDistinctlyFromFailure) {
  EXPECT_EQ(toString(ProcedureStatus::kCancelled), "cancelled");
  EXPECT_EQ(toString(ProcedureStatus::kFailed), "failed");
}

}  // namespace
