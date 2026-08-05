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

/// @brief What the OS command procedure accepts, as its descriptor advertises it.
///
/// Lives beside @c parseOsCommandRequest deliberately: the description a client builds a form from
/// and the validation a request is actually held to are two views of one thing, and keeping them
/// apart is how they drift.
std::vector<ProcedureParameter> osCommandParameters();

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

/// @brief Procedure name for encoder register communication, as it appears in its URL and its
///        snapshot key.
inline constexpr std::string_view kEncoderRegisterProcedure = "encoder-register-communication";

/// @brief The single step encoder register communication reports against.
inline constexpr std::string_view kEncoderRegisterStep = "register-access";

/// @brief What one encoder register access was asked to do.
///
/// The request behind @c POST @c /api/devices/:pos/procedures/encoder-register-communication, and
/// **the first procedure request made of named parameters** rather than raw command bytes — which
/// is what @c ProcedureParameter exists to describe.
struct EncoderRegisterRequest {
  somanet::EncoderOrdinal encoder{somanet::EncoderOrdinal::kEncoder1};  ///< Which encoder.
  bool write = false;           ///< Write the register rather than read it.
  uint8_t registerAddress = 0;  ///< The register to access; no default, so a request must say.
  uint8_t value = 0;            ///< The byte to write; ignored when reading.
};

/// @brief Parses and validates a client's encoder register request body.
///
/// Accepts `{"encoder": 1, "write": false, "registerAddress": 117, "value": 7}`. Only
/// `registerAddress` is required; the rest default as @c EncoderRegisterRequest declares, which is
/// exactly what @c encoderRegisterParameters advertises.
///
/// @param body  Parsed request JSON.
/// @return The validated request, or a message naming what is wrong with it.
std::expected<EncoderRegisterRequest, std::string> parseEncoderRegisterRequest(
    const nlohmann::json& body);

/// @brief What encoder register communication accepts, as its descriptor advertises it.
std::vector<ProcedureParameter> encoderRegisterParameters();

/// @brief Encoder register communication's step template — one step, idle.
std::vector<ProgressStep> encoderRegisterSteps();

/// @brief Runs one encoder register access as a procedure body.
///
/// **The one procedure here that prepares nothing.** OS command 0's only restrictions are that the
/// addressed encoder is configured and that it is a BiSS encoder — no diagnostics mode, no
/// Operation Enabled, no brake, and no motion — so there is no preparation to do and nothing to
/// restore, and it runs from PRE-OP up on a drive that may be exchanging process data. Its single
/// step is the access itself.
///
/// A read and a write are one procedure because they are one firmware command, told apart by
/// @c EncoderRegisterRequest::write; the drive answers both by reporting what the register holds,
/// so a write confirms itself.
///
/// @param device   Device to run against, borrowed by the manager for this call.
/// @param reporter Where step progress is recorded.
/// @param stop     Cancellation token; passed into the command so an in-flight access is aborted,
///                 not merely abandoned.
/// @param request  Which encoder, which register, and whether to write it.
/// @return Void when the drive performed the access, otherwise why it did not.
std::expected<void, std::string> runEncoderRegisterProcedure(Device& device,
                                                             ProgressReporter& reporter,
                                                             std::stop_token stop,
                                                             const EncoderRegisterRequest& request);

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

/// @brief Procedure name for motor phase order detection, as it appears in its URL and its snapshot
///        key.
inline constexpr std::string_view kMotorPhaseOrderDetectionProcedure =
    "motor-phase-order-detection";

/// @brief The step motor phase order detection's own measurement reports against.
inline constexpr std::string_view kMotorPhaseOrderDetectionStep = "motor-phase-order-detection";

/// @brief Motor phase order detection's step template — prepare, release the brake, detect,
/// restore.
std::vector<ProgressStep> motorPhaseOrderDetectionSteps();

/// @brief Runs motor phase order detection as a procedure body.
///
/// Four steps, with a brake release like @c runPolePairDetectionProcedure and for the same reason —
/// this command's restrictions require a disengaged brake, and diagnostics mode suppresses the
/// automatic release. The brake and the operation mode are put back on every path out.
///
/// **This is the first procedure whose success changes the drive's configuration.** The firmware
/// writes the detected order into 0x2003:05 itself, so there is nothing to save afterwards — and
/// nothing to undo either: the restore puts back the mode and the brake, not the phase order,
/// because the new value *is* the result the run was for. It is also a prerequisite: commutation
/// offset measurement requires that this has been run.
///
/// **The command rotates the rotor**, so the shaft must be free and its load safe to move.
///
/// @param device   Device to run against, borrowed by the manager for this call.
/// @param reporter Where step progress is recorded.
/// @param stop     Cancellation token; checked between steps and passed into the OS command so a
///                 running detection is aborted rather than abandoned.
/// @return Void when the drive reported a phase order, otherwise why it did not.
std::expected<void, std::string> runMotorPhaseOrderDetectionProcedure(Device& device,
                                                                      ProgressReporter& reporter,
                                                                      std::stop_token stop);

/// @brief Procedure name for the whole commissioning sequence, as it appears in its URL and its
///        snapshot key.
///
/// *Offset detection* is what this sequence has always been called, and the name people will look
/// for. Not to be confused with @c kCommutationOffsetDetectionProcedure, which is the last two
/// commands of this sequence on their own — the pair an incremental-encoder axis repeats after
/// every power-on, once a full offset detection has been done.
inline constexpr std::string_view kOffsetDetectionProcedure = "offset-detection";

/// @brief The commissioning sequence's step template — every command in the order it runs, plus the
///        shared prepare, brake and restore steps.
std::vector<ProgressStep> offsetDetectionSteps();

/// @brief Runs the whole commissioning sequence as one procedure body.
///
/// Every measurement a motor needs, in the order they depend on each other, in **one prepared
/// session**: open phase detection (6), phase resistance (8), phase inductance (9), pole pair
/// detection (7), motor phase order detection (4), commutation offset measurement (5). Each step
/// records its own result, so a run that stops half way still reports everything it established.
///
/// **Why one procedure rather than six runs.** The drive is put into diagnostics mode and enabled
/// once, and the brake is released once, so the axis spends the minimum time in a state where it
/// can move; and the sequence cannot be got wrong — open phase detection before the measurements
/// that assume connected phases, and motor phase order before the offset that is meaningless
/// without it.
///
/// **The brake is released as late as the sequence allows**, not at the start: open phase detection
/// and the two winding measurements do not require it, so the load stays held until pole pair
/// detection needs it free. It is put where the offset method needs it before the last step, and
/// restored to what it was on the way out.
///
/// **A stopping failure stops the run.** Every step depends on the ones before it, so a failed
/// measurement is not skipped past — carrying on would measure against a value that was never
/// established. An open phase fails the run outright for the same reason.
///
/// **What it does not do is store the measurements.** Commands 7, 8 and 9 report a value without
/// writing it (unlike 4 and 5, which the firmware stores itself), and the objects it would belong
/// in — 0x2003:01, :03 and :04 — hold their values in units this code has not confirmed against the
/// firmware. Writing an unverified scaling into a motor's configuration is worse than reporting the
/// number and letting the caller place it.
///
/// @param device   Device to run against, borrowed by the manager for this call.
/// @param reporter Where step progress is recorded.
/// @param stop     Cancellation token; checked between every step and passed into each command so a
///                 running measurement is aborted rather than abandoned.
/// @return Void when every step succeeded, otherwise why the run stopped.
std::expected<void, std::string> runOffsetDetectionProcedure(Device& device,
                                                             ProgressReporter& reporter,
                                                             std::stop_token stop);

/// @brief Procedure name for commutation offset measurement, as it appears in its URL and its
///        snapshot key.
inline constexpr std::string_view kCommutationOffsetDetectionProcedure =
    "commutation-offset-detection";

/// @brief The step commutation offset measurement's own measurement reports against.
inline constexpr std::string_view kCommutationOffsetMeasurementStep =
    "commutation-offset-measurement";

/// @brief The step that puts the brake where the configured method needs it.
///
/// Its own id rather than @c kReleaseBrakeStep, because this is the one procedure that may
/// **engage** the brake instead of releasing it: the stationary method cannot hold the load, so it
/// runs with the brake on. A step called "release-brake" that sometimes engages would be a lie in
/// the step array.
inline constexpr std::string_view kSetBrakeStep = "set-brake";

/// @brief Commutation offset detection's step template — prepare, release the brake, detect the
/// phase
///        order, set the brake, measure, restore.
std::vector<ProgressStep> commutationOffsetDetectionSteps();

/// @brief Runs commutation offset detection as a procedure body — **motor phase order detection
///        (command 4) followed by commutation offset measurement (command 5)**.
///
/// The pair is the unit, not two things a caller may sequence itself: command 5 is only meaningful
/// once the phase order is known, the drive does not check that it has been, and an offset measured
/// against an unknown phase order is simply wrong. Running command 4 here is what makes the result
/// trustworthy — and it is also exactly the sequence an incremental-encoder axis repeats after
/// every power-on.
///
/// **The one procedure whose physical behaviour is configured on the drive rather than fixed
/// here**: the method in 0x2009:03 decides whether the offset measurement turns the rotor and which
/// way the brake has to go. So the method is read first — before the drive is touched at all, since
/// a method that cannot be read or is out of range means the brake would be handled by guesswork —
/// and is reported with the result.
///
/// Six steps: the shared **prepare**; **release-brake**, which command 4 requires unconditionally;
/// **motor-phase-order-detection**; **set-brake**, which puts the brake where the offset *method*
/// needs it (still released for the rotating methods, *engaged* for the stationary one, which
/// cannot hold the load and so has to undo the release just made for command 4); the measurement;
/// and the shared **restore**.
///
/// **The rotor turns whatever the method.** The stationary offset method does not turn it, but
/// command 4 always does, so no configuration of this procedure leaves the shaft still.
///
/// **A successful run changes the drive's configuration twice over** — command 4 writes 0x2003:05,
/// command 5 writes 0x2001 and sets 0x2009:01 to OFFSET_VALID — and the restore deliberately leaves
/// both alone: they are the result, not side effects.
///
/// @param device   Device to run against, borrowed by the manager for this call.
/// @param reporter Where step progress is recorded.
/// @param stop     Cancellation token; checked between steps and passed into the OS command so a
///                 running measurement is aborted rather than abandoned.
/// @return Void when the drive reported an offset, otherwise why it did not.
std::expected<void, std::string> runCommutationOffsetDetectionProcedure(Device& device,
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
