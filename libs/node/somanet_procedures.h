#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "node/device.h"
#include "node/procedure.h"
#include "node/somanet_drive.h"

namespace mm::node {

/// @file
/// @brief SOMANET procedure bodies — the work a @c ProcedureManager run performs.
///
/// Distinct from @c cia402_control.h / @c profile_control.h in shape as well as content: a control
/// function is addressed by bus position and borrows the device itself, while a procedure body
/// receives a @c Device& already borrowed by the manager, plus the reporter to record progress on
/// and the token to check for cancellation. Bodies bind whatever profile view they need, which is
/// what keeps @c ProcedureManager profile-ignorant.

/// @brief Procedure name for the raw OS command, as it appears in its URL and its snapshot key.
inline constexpr std::string_view kOsCommandProcedure = "os-command";

/// @brief The single step the OS command procedure reports against.
inline constexpr std::string_view kOsCommandStep = "command";

/// @brief What one raw OS command run was asked to do.
///
/// The escape hatch behind @c POST @c /api/devices/:pos/procedures/os-command: arbitrary command
/// bytes, for bring-up and for the commands that have no typed wrapper yet. A typed procedure
/// builds its own bytes instead and exposes only the parameters that make sense for it.
struct OsCommandRequest {
  std::vector<uint8_t> command;                ///< The 8 request bytes (byte 0 = command ID).
  std::chrono::milliseconds timeout{1000};     ///< Ceiling on the whole command.
  std::chrono::milliseconds pollInterval{10};  ///< Delay between response polls.
};

/// @brief Parses and validates a client's OS command request body.
///
/// Lives here rather than in the HTTP handler because validation is domain knowledge — how many
/// bytes a command is, what a byte may hold, what timing is sane — and the handler's job is only to
/// forward. A C++ caller building a request directly gets the same checks.
///
/// Accepts `{"command": [8, 0, ...], "timeoutMs": 30000, "pollIntervalMs": 10}`. Both timing fields
/// are optional and default as @c OsCommandRequest declares.
///
/// @param body  Parsed request JSON.
/// @return The validated request, or a message naming what is wrong with it.
std::expected<OsCommandRequest, std::string> parseOsCommandRequest(const nlohmann::json& body);

/// @brief What one raw OS command run produced — the step's value.
///
/// A procedure-specific value type with its own @c to_json, which is how a body records something
/// richer than a number without @c ProgressStep needing to know about it (see @c ProgressStep).
struct OsCommandResult {
  uint8_t status = 0;                ///< Terminal status byte (0x1023:03 byte 0).
  std::vector<uint8_t> data;         ///< Service response payload, if the drive sent one.
  std::optional<uint8_t> errorCode;  ///< OS error code, present only when the drive reported one.
};
void to_json(nlohmann::json& j, const OsCommandResult& result);

/// @brief The OS command procedure's step template — one step, all idle.
std::vector<ProgressStep> osCommandSteps();

/// @brief Runs one raw OS command as a procedure body.
///
/// Binds a @c SomanetDrive to @p device, issues @p request through @c SomanetDrive::runOsCommand,
/// and records the outcome on the single @c kOsCommandStep step. A command the drive *answered*
/// with an error is still a failure of the run — the step carries the decoded reason and the run
/// ends as failed — while a timeout or a cancellation ends it the way @c ProcedureManager decides
/// from the returned error and the stop token.
///
/// @param device   Device to run against, borrowed by the manager for this call.
/// @param reporter Where step progress is recorded.
/// @param stop     Cancellation token; passed through so an in-flight command is aborted, not
///                 merely abandoned.
/// @param request  The command bytes and timing.
/// @return Void when the drive completed the command without error, otherwise why not.
std::expected<void, std::string> runOsCommandProcedure(Device& device, ProgressReporter& reporter,
                                                       std::stop_token stop,
                                                       const OsCommandRequest& request);

}  // namespace mm::node
