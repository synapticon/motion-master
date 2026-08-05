#include "node/profile_procedures.h"

#include <expected>
#include <format>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace mm::node {

std::vector<ProgressStep> storeParametersSteps() { return stepsFrom({kStoreParametersStep}); }

std::expected<void, std::string> runStoreParametersProcedure(Device& device,
                                                             ProgressReporter& reporter,
                                                             std::stop_token stop,
                                                             StoreParametersConfig config) {
  auto profile = createProfileDevice(device);
  if (!profile) {
    // Before the step starts: a device with no generic device area is not the command failing, it
    // is the wrong device for it.
    return std::unexpected(profile.error());
  }

  config.stop = std::move(stop);

  reporter.start(kStoreParametersStep);
  if (auto r = profile->runStoreParameters(config); !r) {
    reporter.fail(kStoreParametersStep, r.error());
    return std::unexpected(r.error());
  }
  reporter.succeed(kStoreParametersStep);
  return {};
}

std::expected<RestoreDefaultParametersRequest, std::string> parseRestoreDefaultParametersRequest(
    const nlohmann::json& body) {
  if (!body.is_object()) {
    return std::unexpected("the request body must be a JSON object");
  }
  RestoreDefaultParametersRequest request;
  auto group = body.find("group");
  if (group == body.end() || group->is_null()) {
    return request;
  }
  if (!group->is_string()) {
    return std::unexpected("'group' must be a string");
  }
  auto parsed = parseRestoreGroup(group->get<std::string>());
  if (!parsed) {
    return std::unexpected(
        std::format("'group' must be one of {}, {}, {} or {}", toString(RestoreGroup::kAll),
                    toString(RestoreGroup::kCommunication), toString(RestoreGroup::kApplication),
                    toString(RestoreGroup::kManufacturer)));
  }
  request.group = *parsed;
  return request;
}

std::vector<ProcedureParameter> restoreDefaultParametersParameters() {
  const RestoreDefaultParametersRequest defaults;
  return {
      enumParameter(
          "group", "Group",
          "Which group of defaults to restore. \"All\" reloads every group; the others target a "
          "single 0x1011 sub-entry, for a device where only one area should go back.",
          std::string(toString(defaults.group)),
          {
              ParameterOption{.value = std::string(toString(RestoreGroup::kAll)),
                              .title = "All (0x1011:01)"},
              ParameterOption{.value = std::string(toString(RestoreGroup::kCommunication)),
                              .title = "Communication (0x1011:02)"},
              ParameterOption{.value = std::string(toString(RestoreGroup::kApplication)),
                              .title = "Application (0x1011:03)"},
              ParameterOption{.value = std::string(toString(RestoreGroup::kManufacturer)),
                              .title = "Manufacturer (0x1011:04)"},
          }),
  };
}

std::vector<ProgressStep> restoreDefaultParametersSteps() {
  return stepsFrom({kRestoreDefaultParametersStep});
}

std::expected<void, std::string> runRestoreDefaultParametersProcedure(
    Device& device, ProgressReporter& reporter, std::stop_token stop,
    const RestoreDefaultParametersRequest& request, RestoreDefaultParametersConfig config) {
  auto profile = createProfileDevice(device);
  if (!profile) {
    return std::unexpected(profile.error());
  }

  config.stop = std::move(stop);

  reporter.start(kRestoreDefaultParametersStep);
  if (auto r = profile->runRestoreDefaultParameters(request.group, config); !r) {
    reporter.fail(kRestoreDefaultParametersStep, r.error());
    return std::unexpected(r.error());
  }
  // The group is recorded rather than left implicit: a snapshot read later has to say *what* was
  // restored, and "restore" alone would not.
  reporter.succeed(kRestoreDefaultParametersStep,
                   nlohmann::json{{"group", toString(request.group)}});
  return {};
}

}  // namespace mm::node
