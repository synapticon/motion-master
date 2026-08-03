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

/// @brief The step ids shared by every procedure that prepares a drive, measures, and puts it back.
///
/// The preparation and its undoing are the *same work* in each of them — SOMANET's diagnostics
/// operation mode, CiA402 Operation Enabled, and for some commands a released brake — so they carry
/// the same ids everywhere rather than a near-identical set named per procedure, and a client
/// labels them once. What differs is the measurement step in between, which each procedure names
/// itself.
///
/// They are steps rather than hidden setup because each can fail on real hardware — a drive that
/// will not enable, a brake that will not restore — and a user staring at a stalled procedure needs
/// to see *which* part stalled. @c kReleaseBrakeStep appears only in the procedures whose command
/// requires the brake released; the others leave it exactly as they found it.
inline constexpr std::string_view kPrepareStep = "prepare";
inline constexpr std::string_view kReleaseBrakeStep = "release-brake";
inline constexpr std::string_view kRestoreStep = "restore";

/// @brief Procedure name for open phase detection, as it appears in its URL and its snapshot key.
inline constexpr std::string_view kOpenPhaseDetectionProcedure = "open-phase-detection";

/// @brief The step open phase detection's own measurement reports against.
inline constexpr std::string_view kOpenPhaseDetectionStep = "open-phase-detection";

/// @brief Open phase detection's step template — prepare, check, restore, all idle.
std::vector<ProgressStep> openPhaseDetectionSteps();

/// @brief Runs open phase detection as a procedure body.
///
/// Prepares the drive, runs the check, and puts everything back:
///
/// 1. **prepare** — saves the current operation mode, sets @c somanet::OperationMode::kDiagnostics,
///    and walks the CiA402 state machine to Operation Enabled. Diagnostics mode and Operation
///    Enabled are both preconditions the firmware enforces by refusing the command with OS error
///    251.
/// 2. **open-phase-detection** — the OS command. An open phase *fails* this step, naming the
///    offending terminal or FET: the check ran and found a fault, which is a result the user must
///    act on, so it is not reported as a success carrying bad news.
/// 3. **restore** — puts back the operation mode as it was found and returns the drive to Switch On
///    Disabled. It runs on **every** path out, including a failure or a cancellation, because a
///    procedure that leaves a drive in diagnostics mode is worse than one that never ran.
///
/// **The brake is not touched.** This command's restrictions do not require a disengaged one, and
/// the firmware specification says only that the command "might rotate the motor if there is no
/// brake, or if it's disengaged" — so an engaged brake does not prevent the check, it keeps the
/// shaft still while it runs. Contrast @c runPolePairDetectionProcedure, whose command does require
/// it.
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

/// @brief Procedure name for pole pair detection, as it appears in its URL and its snapshot key.
inline constexpr std::string_view kPolePairDetectionProcedure = "pole-pair-detection";

/// @brief The step pole pair detection's own measurement reports against.
inline constexpr std::string_view kPolePairDetectionStep = "pole-pair-detection";

/// @brief Pole pair detection's step template — prepare, release the brake, detect, restore.
std::vector<ProgressStep> polePairDetectionSteps();

/// @brief Runs pole pair detection as a procedure body.
///
/// Four steps, and **the only measurement procedure here that releases the brake**: this command's
/// restrictions do require a disengaged one, and in diagnostics mode enabling the drive does not
/// release it the way normal operation does, so the master has to. It is released after Operation
/// Enabled (never before — the write only performs a real release from that state) and put back by
/// the restore step on every path out, including a failure or a cancellation.
///
/// **The command turns the rotor.** The specification says it needs to, not that it might, so the
/// shaft must be free and whatever it drives must be safe to move. On a vertical or loaded axis,
/// support the load before running it: the brake is released for the duration.
///
/// Needs the bus exchanging process data, for the same reason as its siblings — the CiA402 state
/// machine only advances while the statusword is updating.
///
/// @param device   Device to run against, borrowed by the manager for this call.
/// @param reporter Where step progress is recorded.
/// @param stop     Cancellation token; checked between steps and passed into the OS command so a
///                 running detection is aborted rather than abandoned.
/// @return Void when the drive reported a pole pair count, otherwise why it did not.
std::expected<void, std::string> runPolePairDetectionProcedure(Device& device,
                                                               ProgressReporter& reporter,
                                                               std::stop_token stop);

/// @brief Procedure name for phase resistance measurement, as it appears in its URL and its
///        snapshot key.
inline constexpr std::string_view kPhaseResistanceMeasurementProcedure =
    "phase-resistance-measurement";

/// @brief The step phase resistance measurement's own measurement reports against.
inline constexpr std::string_view kPhaseResistanceMeasurementStep = "phase-resistance-measurement";

/// @brief Phase resistance measurement's step template — prepare, measure, restore, all idle.
std::vector<ProgressStep> phaseResistanceMeasurementSteps();

/// @brief Runs phase resistance measurement as a procedure body.
///
/// Three steps: the shared **prepare** (diagnostics mode, Operation Enabled), the measurement, and
/// the shared **restore**, which runs on every path out including a failure or a cancellation.
///
/// **The brake is not touched, and that is the firmware's requirement rather than an omission.**
/// Unlike motor phase order and pole pair detection, this command does not ask for a released
/// brake, so releasing one here would drop whatever it holds for no benefit — and an engaged brake
/// steadying the shaft while current is driven into the windings is the better state to measure in.
/// The one consequence worth knowing is that a *loose* shaft can still turn.
///
/// Needs the bus exchanging process data: the CiA402 state machine only advances while the
/// statusword is updating, so a device that is not in OP will fail at the prepare step rather than
/// hanging.
///
/// @param device   Device to run against, borrowed by the manager for this call.
/// @param reporter Where step progress is recorded.
/// @param stop     Cancellation token; checked between steps and passed into the OS command so a
///                 running measurement is aborted rather than abandoned.
/// @return Void when the drive reported a resistance, otherwise why it did not.
std::expected<void, std::string> runPhaseResistanceMeasurementProcedure(Device& device,
                                                                        ProgressReporter& reporter,
                                                                        std::stop_token stop);

/// @brief Procedure name for phase inductance measurement, as it appears in its URL and its
///        snapshot key.
inline constexpr std::string_view kPhaseInductanceMeasurementProcedure =
    "phase-inductance-measurement";

/// @brief The step phase inductance measurement's own measurement reports against.
inline constexpr std::string_view kPhaseInductanceMeasurementStep = "phase-inductance-measurement";

/// @brief Phase inductance measurement's step template — prepare, measure, restore, all idle.
std::vector<ProgressStep> phaseInductanceMeasurementSteps();

/// @brief Runs phase inductance measurement as a procedure body.
///
/// Structurally identical to @c runPhaseResistanceMeasurementProcedure — the same three steps, the
/// same preparation, and the same deliberate refusal to touch the brake, which this command no more
/// requires than that one does. Only the quantity measured differs.
///
/// @param device   Device to run against, borrowed by the manager for this call.
/// @param reporter Where step progress is recorded.
/// @param stop     Cancellation token; checked between steps and passed into the OS command so a
///                 running measurement is aborted rather than abandoned.
/// @return Void when the drive reported an inductance, otherwise why it did not.
std::expected<void, std::string> runPhaseInductanceMeasurementProcedure(Device& device,
                                                                        ProgressReporter& reporter,
                                                                        std::stop_token stop);

}  // namespace mm::node
