#include "node/somanet_procedures.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <format>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace mm::node {

namespace {

// Bounds on the client-settable timings. The lower bounds keep a request from busy-polling the
// mailbox or timing out before the drive can answer; the upper bound stops a typo parking a
// device's busy claim for hours.
constexpr auto kMinTimeout = std::chrono::milliseconds(1);
constexpr auto kMaxTimeout = std::chrono::minutes(10);
constexpr auto kMinPollInterval = std::chrono::milliseconds(1);
constexpr auto kMaxPollInterval = std::chrono::seconds(1);

// Reads an optional millisecond field, defaulting to what the request already holds.
std::expected<std::chrono::milliseconds, std::string> readMillis(const nlohmann::json& body,
                                                                 const char* field,
                                                                 std::chrono::milliseconds fallback,
                                                                 std::chrono::milliseconds min,
                                                                 std::chrono::milliseconds max) {
  auto it = body.find(field);
  if (it == body.end() || it->is_null()) {
    return fallback;
  }
  if (!it->is_number_unsigned()) {
    return std::unexpected(
        std::format("'{}' must be a non-negative whole number of milliseconds", field));
  }
  const std::chrono::milliseconds value{it->get<uint64_t>()};
  if (value < min || value > max) {
    return std::unexpected(
        std::format("'{}' must be between {} and {} ms", field, min.count(), max.count()));
  }
  return value;
}

}  // namespace

std::expected<OsCommandRequest, std::string> parseOsCommandRequest(const nlohmann::json& body) {
  if (!body.is_object()) {
    return std::unexpected("the request body must be a JSON object");
  }
  auto command = body.find("command");
  if (command == body.end() || !command->is_array()) {
    return std::unexpected(
        std::format("'command' must be an array of {} byte values", kOsCommandSize));
  }
  if (command->size() != kOsCommandSize) {
    return std::unexpected(std::format("'command' must hold exactly {} bytes, got {}",
                                       kOsCommandSize, command->size()));
  }

  OsCommandRequest request;
  request.command.reserve(kOsCommandSize);
  for (const auto& byte : *command) {
    if (!byte.is_number_unsigned() || byte.get<uint64_t>() > 0xFF) {
      return std::unexpected("every entry of 'command' must be a byte value (0-255)");
    }
    request.command.push_back(static_cast<uint8_t>(byte.get<uint64_t>()));
  }

  auto timeout = readMillis(body, "timeoutMs", request.timeout, kMinTimeout, kMaxTimeout);
  if (!timeout) {
    return std::unexpected(timeout.error());
  }
  request.timeout = *timeout;

  auto pollInterval =
      readMillis(body, "pollIntervalMs", request.pollInterval, kMinPollInterval, kMaxPollInterval);
  if (!pollInterval) {
    return std::unexpected(pollInterval.error());
  }
  request.pollInterval = *pollInterval;
  return request;
}

void to_json(nlohmann::json& j, const OsCommandResult& result) {
  j = nlohmann::json{{"status", result.status}, {"data", result.data}};
  if (result.errorCode) {
    j["errorCode"] = *result.errorCode;
  }
}

std::vector<ProgressStep> osCommandSteps() {
  ProgressStep step;
  step.id = std::string(kOsCommandStep);
  return {step};
}

std::expected<void, std::string> runOsCommandProcedure(Device& device, ProgressReporter& reporter,
                                                       std::stop_token stop,
                                                       const OsCommandRequest& request) {
  auto drive = createSomanetDrive(device);
  if (!drive) {
    // Before the step starts: this is not the command failing, it is the device turning out not to
    // be one that has OS commands, so nothing is reported against the step.
    return std::unexpected(drive.error());
  }

  reporter.start(kOsCommandStep);
  auto response = drive->runOsCommand(
      request.command,
      {.timeout = request.timeout, .pollInterval = request.pollInterval, .stop = std::move(stop)});
  if (!response) {
    reporter.fail(kOsCommandStep, response.error());
    return std::unexpected(response.error());
  }

  if (response->failed()) {
    // The drive answered with a refusal. Name the general codes; a command-specific one (counting
    // up from 0) can only be named by a typed procedure that knows which command it issued, so the
    // raw path reports the number and lets the caller look it up.
    std::string reason;
    if (response->errorCode) {
      auto name = osCommandErrorName(*response->errorCode);
      reason = name ? std::format("OS error {} ({})", *response->errorCode, *name)
                    : std::format("OS error {} (command-specific)", *response->errorCode);
    } else {
      reason = "the drive reported an error with no code";
    }
    reporter.fail(kOsCommandStep, reason);
    return std::unexpected(reason);
  }

  reporter.succeed(kOsCommandStep, OsCommandResult{.status = static_cast<uint8_t>(response->status),
                                                   .data = response->data,
                                                   .errorCode = response->errorCode});
  return {};
}

std::vector<ProgressStep> openPhaseDetectionSteps() {
  std::vector<ProgressStep> steps;
  for (auto id : {kOpenPhasePrepareStep, kOpenPhaseReleaseBrakeStep, kOpenPhaseDetectStep,
                  kOpenPhaseRestoreStep}) {
    ProgressStep step;
    step.id = std::string(id);
    steps.push_back(std::move(step));
  }
  return steps;
}

std::expected<void, std::string> runOpenPhaseDetectionProcedure(Device& device,
                                                                ProgressReporter& reporter,
                                                                std::stop_token stop) {
  auto drive = createSomanetDrive(device);
  if (!drive) {
    return std::unexpected(drive.error());
  }

  // --- prepare -----------------------------------------------------------------------------------
  reporter.start(kOpenPhasePrepareStep);

  auto savedMode = drive->operationModeValue();
  if (!savedMode) {
    reporter.fail(kOpenPhasePrepareStep, savedMode.error());
    return std::unexpected(savedMode.error());
  }
  auto savedBrake = drive->brakeStatus();
  if (!savedBrake) {
    reporter.fail(kOpenPhasePrepareStep, savedBrake.error());
    return std::unexpected(savedBrake.error());
  }

  // Everything below this point has to be undone. The guard runs on every path out — an early
  // return, a failure, a cancellation — because a procedure that leaves the brake released and the
  // drive in diagnostics mode has left the machine in a worse state than it found it. It reports
  // through the restore step rather than only logging, so a restore that itself fails is visible.
  struct Restorer {
    SomanetDrive* drive = nullptr;
    ProgressReporter* reporter = nullptr;
    int8_t mode = 0;
    somanet::BrakeStatus brake{somanet::BrakeStatus::kEngaged};

    ~Restorer() {
      if (drive == nullptr) {
        return;
      }
      reporter->start(kOpenPhaseRestoreStep);
      std::string failures;
      const auto note = [&failures](const std::string& what) {
        failures += failures.empty() ? what : "; " + what;
      };
      // Brake first, then the mode, then the state: the brake is only the master's to command while
      // the drive is still enabled and in diagnostics mode, so putting it back has to happen before
      // either of those is undone.
      if (auto r = drive->setBrakeStatus(brake); !r) {
        note(std::format("failed to restore the brake: {}", r.error()));
      }
      if (auto r = drive->setOperationModeValue(mode); !r) {
        note(std::format("failed to restore operation mode {}: {}", mode, r.error()));
      }
      if (auto r = drive->disable(); !r) {
        note(std::format("failed to return the drive to Switch On Disabled: {}", r.error()));
      }
      if (failures.empty()) {
        reporter->succeed(kOpenPhaseRestoreStep);
      } else {
        spdlog::error("Device {}: open phase detection restore: {}",
                      drive->device().slavePosition(), failures);
        reporter->fail(kOpenPhaseRestoreStep, failures);
      }
    }
  } restorer;

  if (auto r = drive->setOperationMode(somanet::OperationMode::kDiagnostics); !r) {
    reporter.fail(kOpenPhasePrepareStep, r.error());
    return std::unexpected(r.error());
  }
  // Armed only now: the mode write is the first change worth undoing.
  restorer.drive = &*drive;
  restorer.reporter = &reporter;
  restorer.mode = *savedMode;
  restorer.brake = *savedBrake;

  if (auto r = drive->enable(); !r) {
    const auto reason = std::format(
        "the drive did not reach Operation Enabled, which open phase detection requires: {}",
        r.error());
    reporter.fail(kOpenPhasePrepareStep, reason);
    return std::unexpected(reason);
  }
  reporter.succeed(kOpenPhasePrepareStep);

  if (stop.stop_requested()) {
    return std::unexpected("open phase detection was cancelled");
  }

  // --- release the brake -------------------------------------------------------------------------
  // After Operation Enabled, never before: the write only performs a real release while the drive
  // is enabled, and diagnostics mode suppresses the automatic release that normal operation would
  // do.
  reporter.start(kOpenPhaseReleaseBrakeStep);
  auto brake = drive->releaseBrake();
  if (!brake) {
    reporter.fail(kOpenPhaseReleaseBrakeStep, brake.error());
    return std::unexpected(brake.error());
  }
  reporter.succeed(kOpenPhaseReleaseBrakeStep, *brake);

  if (stop.stop_requested()) {
    return std::unexpected("open phase detection was cancelled");
  }

  // --- the check ---------------------------------------------------------------------------------
  reporter.start(kOpenPhaseDetectStep);
  auto result = drive->runOpenPhaseDetection({.timeout = std::chrono::seconds(10),
                                              .pollInterval = std::chrono::milliseconds(100),
                                              .stop = std::move(stop)});
  if (!result) {
    reporter.fail(kOpenPhaseDetectStep, result.error());
    return std::unexpected(result.error());
  }
  if (result->phaseOpened) {
    // The check ran and found a fault. That fails the step: a green step next to an unconnected
    // motor terminal would be a worse lie than any loss of nuance here, and the run's overall
    // status is what tells a user this drive has a problem.
    const auto reason = result->describe();
    reporter.fail(kOpenPhaseDetectStep, reason);
    return std::unexpected(reason);
  }
  reporter.succeed(kOpenPhaseDetectStep, *result);
  return {};
}

}  // namespace mm::node
