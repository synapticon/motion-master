#include "node/somanet_procedures.h"

#include <format>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace mm::node {

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

}  // namespace mm::node
