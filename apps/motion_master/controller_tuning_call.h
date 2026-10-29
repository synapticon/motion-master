#pragma once

#include <expected>
#include <optional>
#include <string>

#include "auto_tuning/client.h"
#include "node/controller_tuning.h"

/// @file
/// @brief The four controller tuning functions of the auto-tuning program, in the shape the
///        procedure registry takes.
///
/// Everything that knows the request and reply formats of those functions is here, so that
/// @c mm::node knows neither. The pure functions are the testable parts. The last binds them to a
/// client, or to the absence of one.

namespace mm {

/// @brief The program's function for @p call.
///
/// `full_auto_tune_position_controller` or `full_auto_tune_velocity_controller` when the call
/// carries a drive configuration, otherwise `auto_tune_position_controller` or
/// `auto_tune_velocity_controller`.
std::string controllerTuningFunction(const node::ControllerTuningCall& call);

/// @brief Builds the request body of one call.
///
/// A full position tuning sends `highFriction` 0, the standard tuning rule, because the program
/// requires the field and nothing here chooses the high-friction rule.
std::string controllerTuningRequest(const node::ControllerTuningCall& call);

/// @brief Reads the program's reply to @p call.
///
/// The program answers a refused input with status 200 and an @c error property, so both the
/// status and the body decide. A full tuning reports the damping ratio and the settling time or
/// the bandwidth it chose. A tuning to targets reports none, so the reply carries the targets.
std::expected<node::ControllerTuningReply, std::string> parseControllerTuningReply(
    const node::ControllerTuningCall& call, int status, const std::string& body);

/// @brief The tuning callback for the procedure registry.
///
/// @param client  The running auto-tuning program, or nothing when none runs. Borrowed: the
///                optional must outlive the returned function. Without a client, the function
///                refuses every call with a message that names the missing program.
node::ControllerTuningFn makeControllerTuning(const std::optional<auto_tuning::Client>& client);

}  // namespace mm
