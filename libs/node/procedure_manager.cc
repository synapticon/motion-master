#include "node/procedure_manager.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mm::node {

namespace {

int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

ProcedureManager::~ProcedureManager() {
  std::vector<std::jthread> threads;
  {
    const std::lock_guard lock(mutex_);
    for (auto& [key, run] : runs_) {
      if (run->thread.joinable()) {
        run->thread.request_stop();
        threads.push_back(std::move(run->thread));
      }
    }
  }
  // Joined here, by leaving this scope with the mutex released: a run on its way out takes the bus
  // lock, and a poller may be mid-snapshot, so waiting while holding ours would serialise both for
  // no reason. Nothing can start a new run — the manager is being destroyed.
}

void ProcedureManager::discardIfRescanned() const {
  const uint64_t generation = deviceManager_.topologyGeneration();
  const size_t before = runs_.size();
  // Sweep on every call, keyed on each run's own generation, rather than firing once when the
  // generation changes: a run that was still going at the moment of the rescan must be left alone
  // (see the header — dropping it would have its thread self-join), so the collection of that entry
  // has to be able to happen on a later pass.
  std::erase_if(runs_, [generation](const auto& entry) {
    return entry.second->topologyGeneration != generation && !entry.second->running.load();
  });
  if (runs_.size() != before) {
    spdlog::debug("Device set rebuilt; discarded {} retained procedure snapshot(s)",
                  before - runs_.size());
  }
}

std::expected<void, ProcedureError> ProcedureManager::start(uint16_t devicePosition,
                                                            std::string name,
                                                            std::vector<ProgressStep> steps,
                                                            ProcedureBody body) {
  return startRun(devicePosition, std::move(name), std::move(steps),
                  [this, devicePosition, body = std::move(body)](ProgressReporter& reporter,
                                                                 std::stop_token stop) {
                    // The handle is the run's hold on its device: it keeps the device constructed
                    // for the whole body without keeping any lock, so a concurrent scan publishes a
                    // new set and this run keeps working against the one it started on.
                    const auto device = deviceManager_.deviceAt(devicePosition);
                    if (!device) {
                      return std::expected<void, std::string>(deviceNotFound(devicePosition));
                    }
                    const ProcedureContext context{.manager = deviceManager_,
                                                   .device = *device,
                                                   .devicePosition = devicePosition};
                    return body(context, reporter, std::move(stop));
                  });
}

std::expected<void, ProcedureError> ProcedureManager::startRun(uint16_t devicePosition,
                                                               std::string name,
                                                               std::vector<ProgressStep> steps,
                                                               ResolvedWork work) {
  // Resolve now rather than inside the thread, so an unknown position is reported to the caller
  // instead of surfacing later as a run that failed immediately. Capturing the topology generation
  // here is the accurate reading: it is the generation the device was resolved under, and a rescan
  // landing between here and the insert below simply makes discardIfRescanned() collect the run on
  // a later pass.
  if (!deviceManager_.deviceAt(devicePosition)) {
    return std::unexpected(
        ProcedureError{.kind = ProcedureError::Kind::kUnknownDevice,
                       .message = std::format("device {} not found", devicePosition)});
  }
  const uint64_t topologyGeneration = deviceManager_.topologyGeneration();

  const std::lock_guard lock(mutex_);
  discardIfRescanned();

  for (const auto& [key, run] : runs_) {
    if (key.first == devicePosition && run->running.load()) {
      return std::unexpected(ProcedureError{
          .kind = ProcedureError::Kind::kBusy,
          .message = std::format("device {} is busy running '{}'", devicePosition, key.second)});
    }
  }

  const Key key{devicePosition, std::move(name)};
  auto previous = runs_.find(key);
  const uint32_t runCount = (previous == runs_.end() ? 0 : previous->second->runCount) + 1;

  auto run = std::make_shared<Run>();
  run->reporter = std::make_shared<ProgressReporter>(std::move(steps));
  run->runCount = runCount;
  run->startedAt = nowMs();
  run->topologyGeneration = topologyGeneration;

  // Replacing the entry drops the previous run, joining its thread — already finished, since the
  // busy check above passed.
  runs_[key] = run;

  run->thread = std::jthread([run, work = std::move(work)](const std::stop_token& stop) {
    auto result = work(*run->reporter, stop);

    if (!result) {
      run->setError(result.error());
    }
    run->finishedAt.store(nowMs());
    if (result) {
      run->status.store(ProcedureStatus::kSucceeded);
    } else if (stop.stop_requested()) {
      // A stop was asked for and the body did not complete: report why it ended, not the error it
      // ended with — "I stopped it" is the useful distinction, and the failing step still records
      // whatever the drive last said.
      run->status.store(ProcedureStatus::kCancelled);
    } else {
      run->status.store(ProcedureStatus::kFailed);
    }
    // Last, so a poller that sees the run finished also sees its outcome.
    run->running.store(false);
  });

  return {};
}

std::optional<ProcedureSnapshot> ProcedureManager::snapshot(uint16_t devicePosition,
                                                            std::string_view name) const {
  const std::lock_guard lock(mutex_);
  discardIfRescanned();
  auto it = runs_.find(Key{devicePosition, std::string(name)});
  if (it == runs_.end()) {
    return std::nullopt;
  }
  const Run& run = *it->second;

  ProcedureSnapshot snapshot;
  snapshot.status = run.status.load();
  snapshot.runCount = run.runCount;
  snapshot.startedAt = run.startedAt;
  if (const int64_t finishedAt = run.finishedAt.load(); finishedAt != 0) {
    snapshot.finishedAt = finishedAt;
  }
  if (auto error = run.error()) {
    snapshot.error = *error;
  }
  snapshot.steps = run.reporter->steps();
  return snapshot;
}

bool ProcedureManager::cancel(uint16_t devicePosition, std::string_view name) {
  const std::lock_guard lock(mutex_);
  discardIfRescanned();
  auto it = runs_.find(Key{devicePosition, std::string(name)});
  if (it == runs_.end() || !it->second->running.load()) {
    return false;
  }
  spdlog::debug("Cancelling procedure '{}' on device {}", name, devicePosition);
  it->second->thread.request_stop();
  return true;
}

}  // namespace mm::node
