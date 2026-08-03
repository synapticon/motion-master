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
/// The request behind @c POST @c /api/devices/:pos/procedures/os-command: the caller supplies the
/// command bytes, which is the direct route to the drive's whole OS command set and a perfectly
/// ordinary way to drive it. A typed procedure is the other route to the same mechanism — it builds
/// the bytes itself and exposes only the parameters that make sense for its command — not a
/// replacement that makes this one a fallback.
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

/// @brief Procedure name for open phase detection, as it appears in its URL and its snapshot key.
inline constexpr std::string_view kOpenPhaseDetectionProcedure = "open-phase-detection";

/// @brief The steps open phase detection reports against, in order.
///
/// The middle one is the measurement; the two around it are the drive preparation the firmware
/// requires and the undoing of it. They are steps rather than hidden setup because either can fail
/// on real hardware — a drive that will not enable, a brake that will not restore — and a user
/// staring at a stalled procedure needs to see *which* part stalled.
inline constexpr std::string_view kOpenPhasePrepareStep = "prepare";
inline constexpr std::string_view kOpenPhaseReleaseBrakeStep = "release-brake";
inline constexpr std::string_view kOpenPhaseDetectStep = "open-phase-detection";
inline constexpr std::string_view kOpenPhaseRestoreStep = "restore";

/// @brief Open phase detection's step template — the four steps above, all idle.
std::vector<ProgressStep> openPhaseDetectionSteps();

/// @brief Runs open phase detection as a procedure body.
///
/// Prepares the drive, releases the brake, runs the check, and puts everything back:
///
/// 1. **prepare** — saves the current operation mode and brake state, sets
///    @c somanet::OperationMode::kDiagnostics, and walks the CiA402 state machine to Operation
///    Enabled. Diagnostics mode and Operation Enabled are both preconditions the firmware enforces
///    by refusing the command with OS error 251.
/// 2. **release-brake** — has to come *after* step 1, and cannot be folded into it. Writing the
///    brake state only performs a real release while the drive is in OP ENABLED, and in diagnostics
///    mode enabling the drive does **not** release the brake the way normal operation does. So the
///    brake is the master's to release here, and only once the drive is enabled.
/// 3. **open-phase-detection** — the OS command. An open phase *fails* this step, naming the
///    offending terminal or FET: the check ran and found a fault, which is a result the user must
///    act on, so it is not reported as a success carrying bad news.
/// 4. **restore** — puts back the brake state and the operation mode as they were found and returns
///    the drive to Switch On Disabled. It runs on **every** path out, including a failure or a
///    cancellation, because a procedure that leaves a brake released and a drive in diagnostics
///    mode is worse than one that never ran.
///
/// Needs the bus exchanging process data: the CiA402 state machine only advances while the
/// statusword is updating, so a device that is not in OP will fail at step 1 rather than hanging.
///
/// @param device   Device to run against, borrowed by the manager for this call.
/// @param reporter Where step progress is recorded.
/// @param stop     Cancellation token; checked between steps and passed into the OS command so a
///                 running check is aborted rather than abandoned.
/// @return Void when the drive found no open phase, otherwise why not.
std::expected<void, std::string> runOpenPhaseDetectionProcedure(Device& device,
                                                                ProgressReporter& reporter,
                                                                std::stop_token stop);

}  // namespace mm::node
