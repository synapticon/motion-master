#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "comm/fieldbus_driver.h"
#include "node/device.h"
#include "node/device_manager.h"
#include "node/firmware_package.h"
#include "node/kuebler_registers.h"
#include "node/procedure.h"
#include "node/procedure_manager.h"
#include "node/somanet_drive.h"

namespace mm::node {

/// @file
/// @brief SOMANET procedure bodies — the work a @c ProcedureManager run performs.
///
/// Distinct from @c cia402_control.h in shape as well as content: a control
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

/// @brief Procedure name for setting an iC-MU calibration mode, as it appears in its URL and its
///        snapshot key.
inline constexpr std::string_view kIcMuCalibrationModeProcedure = "ic-mu-calibration-mode";

/// @brief The single step setting an iC-MU calibration mode reports against.
inline constexpr std::string_view kIcMuCalibrationModeStep = "set-mode";

/// @brief What one iC-MU calibration mode change was asked to do.
struct IcMuCalibrationModeRequest {
  somanet::EncoderOrdinal encoder{somanet::EncoderOrdinal::kEncoder1};  ///< Which encoder.

  /// Which mode to put it in. Has no default in the request a client sends — the value here is
  /// only what an unset struct holds — because the mode *is* the instruction, and defaulting it
  /// would let a run that named nothing change how an encoder is read.
  somanet::IcMuCalibrationMode mode{somanet::IcMuCalibrationMode::kStandard};
};

/// @brief Parses and validates a client's iC-MU calibration mode request body.
///
/// Accepts `{"encoder": 1, "mode": "configuration"}` — or `raw`, or `standard`. @c mode is
/// required; @c encoder defaults to encoder 1.
///
/// @param body  Parsed request JSON.
/// @return The validated request, or a message naming what is wrong with it.
std::expected<IcMuCalibrationModeRequest, std::string> parseIcMuCalibrationModeRequest(
    const nlohmann::json& body);

/// @brief What setting an iC-MU calibration mode accepts, as its descriptor advertises it.
std::vector<ProcedureParameter> icMuCalibrationModeParameters();

/// @brief The iC-MU calibration mode procedure's step template — one step, idle.
std::vector<ProgressStep> icMuCalibrationModeSteps();

/// @brief Sets an iC-MU encoder's calibration mode as a procedure body.
///
/// Prepares nothing and moves nothing, like @c runEncoderRegisterProcedure — the command needs
/// only an active mailbox — but it differs from every other procedure here in one way worth
/// stating plainly: **it has no restore.** Configuration and raw are modes an encoder is left in,
/// so a run that puts one there has changed how the drive reads position until another run puts it
/// back to standard.
///
/// @param device   Device to run against, borrowed by the manager for this call.
/// @param reporter Where step progress is recorded.
/// @param stop     Cancellation token; passed into the command so an in-flight change is aborted.
/// @param request  Which encoder, and which mode to put it in.
/// @return Void once the drive applied the mode, otherwise why it did not.
std::expected<void, std::string> runIcMuCalibrationModeProcedure(
    Device& device, ProgressReporter& reporter, std::stop_token stop,
    const IcMuCalibrationModeRequest& request);

/// @brief Procedure name for recording a high resolution data stream, as it appears in its URL and
/// its
///        snapshot key.
inline constexpr std::string_view kHrdStreamingProcedure = "hrd-streaming";

/// @brief The step that arms the recording — chooses the signal and the duration, and clears the
///        files the last recording left behind.
inline constexpr std::string_view kHrdConfigureStep = "configure-stream";

/// @brief The step that records. Occupies the whole configured duration.
inline constexpr std::string_view kHrdRecordStep = "record";

/// @brief What one high resolution data recording was asked to capture.
struct HrdStreamingRequest {
  /// Which signal to record. No default a client can rely on — the value here is only what an unset
  /// struct holds — because it decides both what is captured and how the files decode afterwards.
  somanet::HrdData data{somanet::HrdData::kEncoderRawData};

  /// How long to record for. Required, and bounded by @c somanet::maxHrdStreamDuration for the
  /// chosen @c data rather than by one shared ceiling.
  std::chrono::milliseconds duration{};
};

/// @brief Parses and validates a client's HRD streaming request body.
///
/// Accepts `{"data": "encoder-raw", "durationMs": 5000}` — or `system-identification`. Both fields
/// are required, and the duration is checked against the limit for the chosen data, which is
/// **6000 ms for system identification and 10000 ms for encoder raw**. The drive applies the same
/// two limits itself, so checking here only turns a rejected round trip into a 400 that names the
/// limit for the data actually chosen.
///
/// @param body  Parsed request JSON.
/// @return The validated request, or a message naming what is wrong with it.
std::expected<HrdStreamingRequest, std::string> parseHrdStreamingRequest(
    const nlohmann::json& body);

/// @brief What HRD streaming accepts, as its descriptor advertises it.
///
/// The advertised duration range is the wider of the two formats, since a descriptor carries one
/// set of bounds per parameter; the parse applies the narrower one once the format is known.
std::vector<ProcedureParameter> hrdStreamingParameters();

/// @brief The HRD streaming procedure's step template — configure, then record.
std::vector<ProgressStep> hrdStreamingSteps();

/// @brief Records one high resolution data stream as a procedure body.
///
/// Two steps because the firmware has two commands: configuring arms the recording and deletes the
/// files of the previous one (seconds of work on its own), and starting it records for the whole
/// requested duration. Splitting them in the report is what makes a run that failed to arm
/// distinguishable from one that failed while recording.
///
/// Prepares nothing and moves nothing, like @c runEncoderRegisterProcedure — but **what the
/// recording is worth depends on preparation this procedure does not do**: encoder raw data records
/// zeros unless the encoder was first put into raw mode (@c runIcMuCalibrationModeProcedure), and
/// system identification data records an unexcited drive unless a system identification run was
/// configured and started first. See @c somanet::HrdData.
///
/// **The recording is left on the drive.** This body does not read it back — the files are read
/// through @c readHrdRecording, which needs the same @c data selection to decode them, and the
/// configure step reports what was used so a client that polled the run knows what to ask for.
///
/// @param device   Device to run against, borrowed by the manager for this call.
/// @param reporter Where step progress is recorded.
/// @param stop     Cancellation token; passed into both commands, so cancelling during the
///                 recording aborts it on the drive and leaves a short recording behind.
/// @param request  Which signal to record, and for how long.
/// @return Void once the recording finished, otherwise why it did not.
std::expected<void, std::string> runHrdStreamingProcedure(Device& device,
                                                          ProgressReporter& reporter,
                                                          std::stop_token stop,
                                                          const HrdStreamingRequest& request);

/// @brief Procedure name for a Kübler encoder register access, as it appears in its URL and its
///        snapshot key.
inline constexpr std::string_view kKueblerRegisterProcedure = "kuebler-register-communication";

/// @brief The single step a Kübler register access reports against.
inline constexpr std::string_view kKueblerRegisterStep = "register-access";

/// @brief What one Kübler register access was asked to do.
struct KueblerRegisterRequest {
  uint8_t address = 0;  ///< Which register; no default, so a request must name one.
  uint8_t length = 1;   ///< Width in bytes, 1 to 4. Must match the register's real width.
  bool write = false;   ///< Write @c value rather than read.
  uint32_t value = 0;   ///< The value to write; ignored when reading.
};

/// @brief Parses and validates a client's Kübler register request body.
///
/// Accepts `{"address": 48, "length": 4, "write": false, "value": 0}`. @c address and @c length are
/// required — the width is not inferred from the register map, because the map is a vendor draft
/// and the command addresses bytes the draft does not document.
///
/// @c GET @c /api/meta/kuebler-registers is the map a client builds its picker from; it carries
/// each register's width, so a picker fills both fields.
std::expected<KueblerRegisterRequest, std::string> parseKueblerRegisterRequest(
    const nlohmann::json& body);

/// @brief What a Kübler register access accepts, as its descriptor advertises it.
std::vector<ProcedureParameter> kueblerRegisterParameters();

/// @brief The Kübler register procedure's step template — one step, idle.
std::vector<ProgressStep> kueblerRegisterSteps();

/// @brief Reads or writes one Integro internal-encoder register as a procedure body.
///
/// Prepares nothing and moves nothing, like @c runEncoderRegisterProcedure — its BiSS counterpart —
/// and runs from PRE-OP up on a drive that may be exchanging process data. The step records the
/// bytes as they came off the wire, the assembled value, and the register's name when the vendor's
/// draft documents the address.
///
/// @param device   Device to run against, borrowed by the manager for this call.
/// @param reporter Where step progress is recorded.
/// @param stop     Cancellation token; passed into the command.
/// @param request  Which register, how wide, and which direction.
/// @return Void once the encoder performed the access, otherwise why it did not.
std::expected<void, std::string> runKueblerRegisterProcedure(Device& device,
                                                             ProgressReporter& reporter,
                                                             std::stop_token stop,
                                                             const KueblerRegisterRequest& request);

/// @brief Procedure name for choosing the velocity feedback source, as it appears in its URL and
///        its snapshot key.
inline constexpr std::string_view kVelocitySourceProcedure = "velocity-source";

/// @brief The single step choosing the velocity source reports against.
inline constexpr std::string_view kVelocitySourceStep = "set-source";

/// @brief Which velocity the control loop should use.
struct VelocitySourceRequest {
  /// Which source. No default a client can rely on — the value here is only what an unset struct
  /// holds — because the source *is* the instruction, and which one is already active depends on
  /// the product (see @c somanet::VelocitySource).
  somanet::VelocitySource source{somanet::VelocitySource::kFirmware};
};

/// @brief Parses and validates a client's velocity source request body.
///
/// Accepts `{"source": "firmware"}` — or `encoder`. Required.
std::expected<VelocitySourceRequest, std::string> parseVelocitySourceRequest(
    const nlohmann::json& body);

/// @brief What choosing the velocity source accepts, as its descriptor advertises it.
std::vector<ProcedureParameter> velocitySourceParameters();

/// @brief The velocity-source procedure's step template — one step, idle.
std::vector<ProgressStep> velocitySourceSteps();

/// @brief Chooses the velocity loop's feedback source as a procedure body.
///
/// Prepares nothing, restores nothing, and commands no motion — but it is not inert on a moving
/// drive, since the velocity loop's feedback changes under it. See
/// @c SomanetDrive::setVelocitySource, and note that which source is already active depends on the
/// product rather than on the specification's claim.
///
/// @param device   Device to run against, borrowed by the manager for this call.
/// @param reporter Where step progress is recorded.
/// @param stop     Cancellation token; passed into the command.
/// @param request  Which source to use.
/// @return Void once the drive accepted the choice, otherwise why it did not.
std::expected<void, std::string> runVelocitySourceProcedure(Device& device,
                                                            ProgressReporter& reporter,
                                                            std::stop_token stop,
                                                            const VelocitySourceRequest& request);

/// @brief Procedure name for measuring a firmware latency, as it appears in its URL and its
/// snapshot
///        key.
inline constexpr std::string_view kFirmwareLatencyProcedure = "firmware-latency-measurement";

/// @brief The step that starts a measurement — clearing whatever the latency had recorded.
inline constexpr std::string_view kFirmwareLatencyStartStep = "start-measurement";

/// @brief The step that lets the drive run while the measurement collects. Occupies the whole
///        requested duration, and only @c FirmwareLatencyAction::kMeasure performs it.
inline constexpr std::string_view kFirmwareLatencyObserveStep = "observe";

/// @brief The step that reads and clears the recorded maximum.
inline constexpr std::string_view kFirmwareLatencyReadStep = "read-maximum";

/// @brief The step that stops both measurements.
inline constexpr std::string_view kFirmwareLatencyStopStep = "stop-measurements";

/// @brief What one firmware latency run should do.
///
/// **Three of these are the firmware's own actions and the fourth is this master's.** OS command 22
/// starts a measurement, reads its maximum, or stops measuring — a worthwhile figure therefore
/// takes two commands with the drive running in between, which is a workflow rather than a command.
/// @c kMeasure is that workflow performed as one run; the other three are the raw actions, kept
/// because a measurement worth having may need to span a whole production cycle rather than the
/// seconds a single procedure run can wait.
enum class FirmwareLatencyAction : uint8_t {
  /// Start, let the drive run for the requested duration, then read and clear the maximum. The one
  /// action that answers with a number.
  kMeasure,
  /// Start measuring and leave it running, to be read by a later run.
  kStart,
  /// Read and clear the maximum of a measurement already running (or already stopped).
  kReadMaximum,
  /// Stop measuring — necessarily both latencies, as the command has no per-latency stop.
  kStop,
};

/// @brief Name of a firmware latency action (for JSON). Never returns @c nullptr.
constexpr std::string_view toString(FirmwareLatencyAction action) {
  switch (action) {
    case FirmwareLatencyAction::kMeasure:
      return "measure";
    case FirmwareLatencyAction::kStart:
      return "start";
    case FirmwareLatencyAction::kReadMaximum:
      return "read-maximum";
    case FirmwareLatencyAction::kStop:
      return "stop";
  }
  return "unknown";
}

/// @brief Parses a firmware latency action token. @c std::nullopt if it names none of them.
std::optional<FirmwareLatencyAction> parseFirmwareLatencyAction(std::string_view token);

/// @brief What one firmware latency run was asked to do.
struct FirmwareLatencyRequest {
  /// What to do. Defaults to the composite measurement, which is the action that answers with a
  /// figure and needs no follow-up run.
  FirmwareLatencyAction action{FirmwareLatencyAction::kMeasure};

  /// Which latency. Ignored by @c FirmwareLatencyAction::kStop, which stops both.
  somanet::FirmwareLatency latency{somanet::FirmwareLatency::kSetpoint};

  /// How long to let the measurement collect, used only by @c FirmwareLatencyAction::kMeasure.
  std::chrono::milliseconds duration{2000};
};

/// @brief Parses and validates a client's firmware latency request body.
///
/// Accepts `{"action": "measure", "latency": "setpoint", "durationMs": 2000}`. Every field is
/// optional and defaults as @c FirmwareLatencyRequest declares.
///
/// @param body  Parsed request JSON.
/// @return The validated request, or a message naming what is wrong with it.
std::expected<FirmwareLatencyRequest, std::string> parseFirmwareLatencyRequest(
    const nlohmann::json& body);

/// @brief What measuring a firmware latency accepts, as its descriptor advertises it.
std::vector<ProcedureParameter> firmwareLatencyParameters();

/// @brief The firmware latency procedure's step template — start, observe, read, stop.
///
/// **The union of four actions, and no run performs all of it.** A measurement performs the first
/// three, a raw action performs the one that names it, and the rest stay idle — which is honest
/// about a run that never attempted them, and is why the ids name the actions rather than
/// positions.
std::vector<ProgressStep> firmwareLatencySteps();

/// @brief Measures one internal firmware latency as a procedure body.
///
/// Prepares nothing, restores nothing, needs no operation mode, no CiA402 state and no brake, and
/// moves nothing — the measurement is two timer reads inside a cycle the drive control service was
/// running anyway. It runs from PRE-OP up on a drive that may be exchanging process data.
///
/// **What the figure is worth depends on what the drive was doing while it collected**, which this
/// procedure does not arrange: measuring an idle drive for two seconds reports the worst case of an
/// idle drive. A maximum only means something over a window that contained the load it is meant to
/// characterise — which is what @c FirmwareLatencyAction::kStart and @c kReadMaximum exist for.
///
/// **A measurement is left running.** The read action does not stop it and stopping is not
/// per-latency, so a @c kMeasure run leaves the drive measuring rather than silently ending a
/// measurement of the *other* latency that something else may be collecting. See
/// @c SomanetDrive::readMaximumFirmwareLatency.
///
/// @param device   Device to run against, borrowed by the manager for this call.
/// @param reporter Where step progress is recorded.
/// @param stop     Cancellation token; checked during the observation window and passed into each
///                 command.
/// @param request  Which action, which latency, and for how long.
/// @return Void once the requested action completed, otherwise why it did not.
std::expected<void, std::string> runFirmwareLatencyProcedure(Device& device,
                                                             ProgressReporter& reporter,
                                                             std::stop_token stop,
                                                             const FirmwareLatencyRequest& request);

/// @brief Procedure name for provoking a firmware error, as it appears in its URL and its snapshot
///        key.
inline constexpr std::string_view kTriggerErrorProcedure = "trigger-error";

/// @brief The single step provoking a firmware error reports against.
inline constexpr std::string_view kTriggerErrorStep = "trigger";

/// @brief Which service to break, and how.
struct TriggerErrorRequest {
  somanet::FirmwareService service{somanet::FirmwareService::kDriveControl};

  /// Which error to raise. No default a client can rely on — the value here is only what an unset
  /// struct holds — because eight of the twelve types stop a service for good, and none of them is
  /// a reasonable thing to do to a drive by accident.
  somanet::FirmwareErrorType errorType{somanet::FirmwareErrorType::kResettableFirmwareError};
};

/// @brief Parses and validates a client's trigger-error request body.
///
/// Accepts `{"service": "drive-control", "errorType": "resettable-firmware-error"}`. @c errorType
/// is required; @c service defaults to drive control.
std::expected<TriggerErrorRequest, std::string> parseTriggerErrorRequest(
    const nlohmann::json& body);

/// @brief What provoking a firmware error accepts, as its descriptor advertises it.
std::vector<ProcedureParameter> triggerErrorParameters();

/// @brief The trigger-error procedure's step template — one step, idle.
std::vector<ProgressStep> triggerErrorSteps();

/// @brief Provokes a firmware error in a control service as a procedure body.
///
/// **The only deliberately destructive procedure in the catalogue.** It exists because the firmware
/// has the command, and because the behaviour around a stopped control service — what the master
/// sees, what the bus does, what the other devices do — cannot be tested without being able to stop
/// one on purpose. It is a test instrument, not a commissioning step.
///
/// Prepares nothing and restores nothing. What it does depends entirely on the error type, and
/// @c SomanetDrive::triggerError is where that is set out: seven types do nothing at all, four stop
/// the addressed service until the drive is power-cycled, and one raises a resettable fault.
///
/// @param device   Device to run against, borrowed by the manager for this call.
/// @param reporter Where step progress is recorded.
/// @param stop     Cancellation token; passed into the command.
/// @param request  Which service, and which error.
/// @return Void once the command was issued and its outcome established, otherwise why not.
std::expected<void, std::string> runTriggerErrorProcedure(Device& device,
                                                          ProgressReporter& reporter,
                                                          std::stop_token stop,
                                                          const TriggerErrorRequest& request);

/// @brief Procedure name for configuring a system identification run, as it appears in its URL and
///        its snapshot key.
inline constexpr std::string_view kSystemIdentificationProcedure = "system-identification";

/// @brief The step that writes the chirp's five settings.
inline constexpr std::string_view kSystemIdConfigureStep = "configure-chirp";

/// @brief The step that arms the run — or disarms it.
inline constexpr std::string_view kSystemIdArmStep = "arm";

/// @brief One system identification run, as a client describes it.
struct SystemIdentificationRequest {
  uint32_t startFrequencyMilliHz = 0;   ///< Where the sweep begins. Required.
  uint32_t targetFrequencyMilliHz = 0;  ///< Where it ends. Required, and not below the start.
  uint32_t targetAmplitudePermil = 0;   ///< Peak excitation, per-mille of rated torque. Required.
  uint32_t transitionTimeMs = 0;        ///< How long the sweep takes. Required.
  somanet::ChirpSignalType signalType{somanet::ChirpSignalType::kLogarithmic};  ///< Which chirp.

  /// Whether to arm the run, and on what trigger. Defaults to not arming, so a request that says
  /// nothing configures the drive and excites nothing.
  somanet::SystemIdentificationStart start{somanet::SystemIdentificationStart::kNone};
};

/// @brief Parses and validates a client's system identification request body.
///
/// Accepts `{"startFrequencyMilliHz": 1000, "targetFrequencyMilliHz": 100000,
/// "targetAmplitudePermil": 50, "transitionTimeMs": 5000, "signalType": "logarithmic",
/// "start": "after-hrd-stream-start"}`. The four numbers are required; `signalType` and `start`
/// default.
///
/// **The bounds checked here are the firmware's own, and checking them is not politeness.** The
/// drive stores each setting without looking at it and validates the set on the rising edge of the
/// start parameter — where a bad one raises @c IvldPara *with a quick-stop reaction*. Rejecting the
/// request is the difference between a 400 and a faulted drive.
///
/// @param body  Parsed request JSON.
/// @return The validated request, or a message naming what is wrong with it.
std::expected<SystemIdentificationRequest, std::string> parseSystemIdentificationRequest(
    const nlohmann::json& body);

/// @brief What system identification accepts, as its descriptor advertises it.
std::vector<ProcedureParameter> systemIdentificationParameters();

/// @brief The system identification procedure's step template — configure, then arm.
std::vector<ProgressStep> systemIdentificationSteps();

/// @brief Configures and optionally arms a system identification run as a procedure body.
///
/// Two steps, because the drive has two distinct pieces of state and one is a trigger:
///
/// 1. **configure-chirp** — disarms first, then writes the five settings. Disarming is not
///    housekeeping: the drive starts on the *rising edge* of the start parameter, so a run left
///    armed by a previous one would never see an edge and would silently not start.
/// 2. **arm** — writes the requested trigger. With @c SystemIdentificationStart::kNone this leaves
///    the drive configured and idle, which is the default.
///
/// **What it does not do is excite the motor.** Arming with @c kImmediately does — on the next
/// control cycle, if the drive is enabled — and arming with @c kAfterHrdStreamStart waits for a
/// recording to begin. This procedure needs no operation mode, no CiA402 state and no brake,
/// because configuring needs none of them; whether anything moves is decided by the drive's own
/// state when the trigger fires.
///
/// @param device   Device to run against, borrowed by the manager for this call.
/// @param reporter Where step progress is recorded.
/// @param stop     Cancellation token; checked between writes and passed into each command.
/// @param request  The chirp, and whether to arm it.
/// @return Void once every setting was written, otherwise why it stopped.
std::expected<void, std::string> runSystemIdentificationProcedure(
    Device& device, ProgressReporter& reporter, std::stop_token stop,
    const SystemIdentificationRequest& request);

/// @brief Procedure name for ignoring a BiSS encoder's status bits, as it appears in its URL and
///        its snapshot key.
inline constexpr std::string_view kIgnoreBissStatusBitsProcedure = "ignore-biss-status-bits";

/// @brief The single step ignoring BiSS status bits reports against.
inline constexpr std::string_view kIgnoreBissStatusBitsStep = "set-ignore";

/// @brief Which encoder's status bits to act on, and which way.
struct IgnoreBissStatusBitsRequest {
  somanet::EncoderOrdinal encoder{somanet::EncoderOrdinal::kEncoder1};  ///< Which encoder.

  /// Whether to ignore them. Has no default in the request a client sends — the value here is only
  /// what an unset struct holds — because the direction *is* the instruction, and defaulting it
  /// either way would let a run that named nothing change how the drive reacts to its encoder.
  bool ignore = false;
};

/// @brief Parses and validates a client's ignore-BiSS-status-bits request body.
///
/// Accepts `{"encoder": 1, "ignore": true}`. @c ignore is required; @c encoder defaults to
/// encoder 1.
///
/// @param body  Parsed request JSON.
/// @return The validated request, or a message naming what is wrong with it.
std::expected<IgnoreBissStatusBitsRequest, std::string> parseIgnoreBissStatusBitsRequest(
    const nlohmann::json& body);

/// @brief What ignoring BiSS status bits accepts, as its descriptor advertises it.
std::vector<ProcedureParameter> ignoreBissStatusBitsParameters();

/// @brief The ignore-BiSS-status-bits procedure's step template — one step, idle.
std::vector<ProgressStep> ignoreBissStatusBitsSteps();

/// @brief Starts or stops ignoring a BiSS encoder's status bits as a procedure body.
///
/// Prepares nothing and moves nothing — the command needs only an active mailbox, and runs from
/// PRE-OP up on a drive that may be exchanging process data. Like
/// @c runIcMuCalibrationModeProcedure it has **no restore**: ignoring is a state the encoder is
/// left in until another run ends it or the drive is power-cycled.
///
/// **What it suppresses is a fault, not a nuisance.** See
/// @c SomanetDrive::setIgnoreBissStatusBits: a BiSS error bit otherwise faults the drive into
/// active short circuit, and ignoring the bits means the drive keeps running on an encoder that is
/// reporting its own position as unreliable.
///
/// @param device   Device to run against, borrowed by the manager for this call.
/// @param reporter Where step progress is recorded.
/// @param stop     Cancellation token; passed into the command so an in-flight change is aborted.
/// @param request  Which encoder, and which way.
/// @return Void once the drive applied the change, otherwise why it did not.
std::expected<void, std::string> runIgnoreBissStatusBitsProcedure(
    Device& device, ProgressReporter& reporter, std::stop_token stop,
    const IgnoreBissStatusBitsRequest& request);

/// @brief Procedure name for reading a skipped cycles counter, as it appears in its URL and its
///        snapshot key.
inline constexpr std::string_view kSkippedCyclesProcedure = "skipped-cycles-counter";

/// @brief The single step reading a skipped cycles counter reports against.
inline constexpr std::string_view kSkippedCyclesStep = "read-counter";

/// @brief Which control loop's counter to read.
struct SkippedCyclesRequest {
  /// The loop to ask. Defaults to the drive control service — the fast loop, and the one whose
  /// skipped cycles a user is usually chasing.
  somanet::FirmwareService service{somanet::FirmwareService::kDriveControl};
};

/// @brief Parses and validates a client's skipped cycles request body.
///
/// Accepts `{"service": "drive-control"}` — or `motion-control`. The field is optional.
///
/// **Validating it here is not ceremony**: each firmware service answers only requests naming
/// itself, so a byte naming neither is answered by nothing at all and costs the drive's whole
/// command timeout before surfacing as a timeout error that says nothing about the real mistake.
///
/// @param body  Parsed request JSON.
/// @return The validated request, or a message naming what is wrong with it.
std::expected<SkippedCyclesRequest, std::string> parseSkippedCyclesRequest(
    const nlohmann::json& body);

/// @brief What reading a skipped cycles counter accepts, as its descriptor advertises it.
std::vector<ProcedureParameter> skippedCyclesParameters();

/// @brief The skipped cycles procedure's step template — one step, idle.
std::vector<ProgressStep> skippedCyclesSteps();

/// @brief Reads one control loop's skipped-cycle counter as a procedure body.
///
/// **The most harmless procedure here**: no preparation, no operation mode, no CiA402 state, no
/// brake, and nothing restored — a pure read, safe on a drive that is enabled and moving. It runs
/// from PRE-OP up, needing only an active mailbox.
///
/// What it reports is cumulative since the addressed service started, so one run establishes a
/// baseline and a later run establishes a rate. See @c SomanetDrive::readSkippedCycles.
///
/// @param device   Device to run against, borrowed by the manager for this call.
/// @param reporter Where step progress is recorded.
/// @param stop     Cancellation token; passed into the command so an in-flight read is aborted.
/// @param request  Which control loop to ask.
/// @return Void once the drive reported a counter, otherwise why it did not.
std::expected<void, std::string> runSkippedCyclesProcedure(Device& device,
                                                           ProgressReporter& reporter,
                                                           std::stop_token stop,
                                                           const SkippedCyclesRequest& request);

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
/// for. Not to be confused with @c kCommutationOffsetMeasurementProcedure, which is the last
/// command of this sequence on its own.
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
inline constexpr std::string_view kCommutationOffsetMeasurementProcedure =
    "commutation-offset-measurement";

/// @brief The step commutation offset measurement's own measurement reports against.
inline constexpr std::string_view kCommutationOffsetMeasurementStep =
    "commutation-offset-measurement";

/// @brief The step that puts the brake where the configured method needs it.
///
/// Its own id rather than @c kReleaseBrakeStep, because the commissioning sequence may **engage**
/// the brake there instead of releasing it: it released the brake for motor phase order detection,
/// and the stationary offset method that follows cannot hold the load, so the release has to be
/// undone. A step called "release-brake" that sometimes engages would be a lie in the step array.
inline constexpr std::string_view kSetBrakeStep = "set-brake";

/// @brief Commutation offset measurement's step template — prepare, release the brake, measure,
///        restore.
std::vector<ProgressStep> commutationOffsetMeasurementSteps();

/// @brief Runs commutation offset measurement as a procedure body — **OS command 5 and nothing
///        else**.
///
/// One command, like every other measurement procedure here. The pairing with motor phase order
/// detection (4) lives in @c runOffsetDetectionProcedure, which runs the whole commissioning
/// sequence; running the two on their own is the caller's to sequence.
///
/// **Command 5 is only meaningful once the phase order is established, and the drive does not check
/// that it has been.** An offset measured against an unknown phase order is simply wrong, and
/// nothing here can tell the difference — 0x2003:05 holds a valid value either way, so there is no
/// "never established" to detect. Run motor phase order detection first, or run offset detection.
///
/// **The one procedure whose physical behaviour is configured on the drive rather than fixed
/// here**: the method in 0x2009:03 decides whether the measurement turns the rotor and whether it
/// needs the brake released. So the method is read first — before the drive is touched at all,
/// since a method that cannot be read or is out of range means the brake would be handled by
/// guesswork — and is reported with the result.
///
/// **The brake is released only for the rotating methods** (0x2009:03 = 0 or 1), which the firmware
/// refuses to run with it engaged. The stationary method (2) needs nothing of the brake, so the
/// brake is never written and the load stays held; its **release-brake** step stays idle, and there
/// is nothing about the brake to restore either.
///
/// **A successful run changes the drive's configuration** — 0x2001 written and 0x2009:01 set to
/// OFFSET_VALID, in the object dictionary rather than in flash — and the restore deliberately
/// leaves both alone: they are the result, not side effects.
///
/// @param device   Device to run against, borrowed by the manager for this call.
/// @param reporter Where step progress is recorded.
/// @param stop     Cancellation token; checked between steps and passed into the OS command so a
///                 running measurement is aborted rather than abandoned.
/// @return Void when the drive reported an offset, otherwise why it did not.
std::expected<void, std::string> runCommutationOffsetMeasurementProcedure(
    Device& device, ProgressReporter& reporter, std::stop_token stop);

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

/// @brief Procedure name for torque constant measurement, as it appears in its URL and its snapshot
///        key.
inline constexpr std::string_view kTorqueConstantMeasurementProcedure =
    "torque-constant-measurement";

/// @brief The step torque constant measurement's own measurement reports against.
inline constexpr std::string_view kTorqueConstantMeasurementStep = "torque-constant-measurement";

/// @brief Torque constant measurement's step template — prepare, release the brake, measure,
///        restore.
std::vector<ProgressStep> torqueConstantMeasurementSteps();

/// @brief Runs torque constant measurement as a procedure body.
///
/// Four steps rather than the winding measurements' three: this command's restrictions require a
/// disengaged brake, so the brake is released after Operation Enabled and put back by the restore
/// on every path out, exactly as in @c runPolePairDetectionProcedure.
///
/// **The rotor turns for the whole run, which is the longest of these procedures** — the drive
/// spins the motor up over about ten seconds and holds it at speed while it measures, because the
/// constant is derived from the back-EMF the motor generates rather than from a torque nobody can
/// measure. The shaft must be free and whatever it drives safe to keep moving.
///
/// **It measures against the drive's stored motor configuration, and this procedure does not check
/// that.** The back-EMF is what is left after the winding impedance is subtracted from the applied
/// voltage, and the drive takes that impedance and the pole pair count from 0x2003:01, :03 and :04.
/// So the useful order is pole pair detection, phase resistance and phase inductance first, their
/// results *written* into those objects, and this last — which is why it is **not** part of
/// @c runOffsetDetectionProcedure: that sequence deliberately stores nothing, so a torque constant
/// measured inside it would be measured against whatever the drive was configured with before.
///
/// @param device   Device to run against, borrowed by the manager for this call.
/// @param reporter Where step progress is recorded.
/// @param stop     Cancellation token; checked between steps and passed into the OS command so a
///                 running measurement is aborted rather than abandoned.
/// @return Void when the drive reported a torque constant, otherwise why it did not.
std::expected<void, std::string> runTorqueConstantMeasurementProcedure(Device& device,
                                                                       ProgressReporter& reporter,
                                                                       std::stop_token stop);

// ── Firmware installation ──────────────────────────────────────────────────────────────────────
//
// The one procedure here that changes AL state. Installing firmware *is* a sequence of transitions
// (BOOT, write the files, back), so it takes the manager and the position rather than one device.

/// @file
/// @brief Firmware installation, the one procedure that changes AL state.
///
/// It takes @c DeviceManager& and a position rather than a @c Device&, because @c transitionToState
/// is a whole-bus operation and this procedure is defined by its transitions. It resolves the
/// device again for each step, so a rescan mid-install ends the run cleanly at the next step.

/// @brief Procedure name, as it appears in its URL and its snapshot key.
inline constexpr std::string_view kFirmwareInstallationProcedure = "firmware-installation";

/// @brief Where an install leaves the device, as an AL state.
///
/// Deliberately @c mm::comm::EtherCatState rather than a firmware-specific enum, and deliberately
/// the same integer encoding the rest of the API uses (@c POST @c /api/devices/state, and the
/// @c alState every state read reports): one concept, one spelling. A parallel vocabulary here
/// would mean a client holding two ways to say PRE-OP.
///
/// **Any AL state is accepted**, and the two worth knowing about are the ends:
///   - @c PreOp is the default and the one that confirms the install. The SOMANET bootloader hands
///     over to the newly written application on the transition out of BOOT, so **reaching PRE-OP is
///     the new firmware booting and answering**. When no valid application is present the drive
///     reports AL status 0x0014 ("No valid firmware") and falls back to INIT — a precise, decodable
///     failure rather than a hang.
///   - @c Boot leaves the device in the bootloader, which is **the right choice when no application
///     will be present**: after erasing one, or between two installs, a PRE-OP transition has
///     nothing to hand over to and would report a failure for an operation that succeeded.
///
/// SAFE-OP and OP climb through PRE-OP and re-map the whole bus on the way, briefly pausing every
/// other device. That is a real cost but not a reason to withhold them — the same re-map happens
/// when a caller reaches those states with @c POST @c /api/devices/state a moment later, so
/// refusing would only split one operation into two.
///
/// The device's state *before* the install is deliberately not restored: it described a device
/// running the old firmware, and it is a snapshot nothing protects, since the body holds no lock
/// between steps.

/// @brief One validated firmware installation request.
///
/// The package arrives as bytes, however it was named: @c parseFirmwareInstallationRequest resolves
/// either the inline upload or a previously cached package into @c package, so the body never has
/// to know which route the caller took.
struct FirmwareInstallationRequest {
  std::vector<uint8_t> package;  ///< The package zip's bytes.
  std::string filename;          ///< As supplied; empty when the caller gave none.
  /// The decoded filename, present only when @c filename parses as a SOMANET package name. Its
  /// absence is what makes a package uncacheable — see @c cachePackage.
  std::optional<FirmwarePackageName> name;
  std::vector<std::string> skipFiles;  ///< Entries not to write; defaults per the descriptor.
  bool cachePackage = true;            ///< Keep a copy under @c firmwareCacheDir when named.
  mm::comm::EtherCatState finalState = mm::comm::EtherCatState::PreOp;

  /// How long to wait after entering BOOT before the first file transfer.
  ///
  /// Deliberately **not** advertised on the descriptor: it is a property of how long a SOMANET
  /// bootloader takes to start servicing file operations, not a choice a user should be asked to
  /// make, and the default is the figure that works. It is a field rather than a constant so a test
  /// can set it to zero — waiting two seconds per case would dominate the suite — and so a device
  /// that needs longer can be accommodated without a new release.
  std::chrono::milliseconds bootWarmUp{2000};
};

/// @brief Where installed packages are kept: a @c firmwares subdirectory of the per-user cache.
///
/// That places it under the same root @c /api/user-cache serves, so a cached package is listable,
/// downloadable and deletable through the API and the Console with no endpoint of its own.
std::filesystem::path firmwareCacheDir();

/// @brief What the procedure accepts, as its descriptor advertises it.
std::vector<ProcedureParameter> firmwareInstallationParameters();

/// @brief The procedure's step template, in order.
std::vector<ProgressStep> firmwareInstallationSteps();

/// @brief Parses and validates a client's installation request.
///
/// Accepts `{"packageContent": "<base64 zip>", "packageFilename": "package_….zip",
/// "skipFiles": [...], "cachePackage": true, "finalState": "preop"}`. Every field is optional
/// except that **one of @c packageContent or @c packageFilename must resolve to a package**:
/// @c packageContent wins when both are given, and a lone @c packageFilename is looked up in
/// @c firmwareCacheDir, so re-installing a package already on the server costs no upload.
///
/// Resolving the bytes here rather than in the body is deliberate: "the package you named is not
/// cached" is a bad request, answerable immediately, not a run that starts and then fails.
///
/// @param body  Parsed request JSON.
/// @return The validated request, or a message naming what is wrong with it.
std::expected<FirmwareInstallationRequest, std::string> parseFirmwareInstallationRequest(
    const nlohmann::json& body);

/// @brief Installs @p request's package on the device at @p devicePosition.
///
/// Takes the device to BOOT, writes what the package holds, and leaves it in
/// @c request.finalState. Borrows the device per step and holds nothing in between, which is what
/// lets it transition — and which means a rescan can interleave; the next borrow then fails and the
/// run ends cleanly.
///
/// The steps and their failure policy, which is not uniform on purpose:
///   - @c package, @c cache — offline; a package that will not open fails before anything is
///     touched on the bus.
///   - @c boot — fatal. Nothing can be written without it.
///   - @c extra-files — **best effort**. These are descriptive files (an ESI, a picture); a failure
///     is recorded in the step's value and the install continues, because aborting a firmware
///     update over a picture would be worse than not having the picture.
///   - @c sii, @c app-firmware, @c com-firmware — fatal. These are the firmware.
///   - @c final-state — fatal, but reported so it cannot be confused with a write failure: the
///     bytes are on the drive either way, and which of the two happened changes what a user does
///     next.
///
/// Whatever went wrong, the device is not left in BOOT unless that is what was asked for: a failure
/// after entering BOOT still attempts the final transition on the way out.
std::expected<void, std::string> runFirmwareInstallationProcedure(
    DeviceManager& deviceManager, uint16_t devicePosition, ProgressReporter& reporter,
    std::stop_token stop, const FirmwareInstallationRequest& request);

}  // namespace mm::node
