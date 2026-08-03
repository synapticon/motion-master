#include "node/somanet_drive.h"

#include <spdlog/spdlog.h>

#include <format>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "node/synapticon.h"

namespace mm::node {

namespace {

// Non-terminal values of the OS command status byte (0x1023:03 byte 0). Everything from
// kProgressMin to kProgressMax reports a percentage (kProgressMin = 0%, kProgressMax = 100%);
// kExecuting is the same "still running" answer from a command that does not report progress.
constexpr uint8_t kProgressMin = 100;
constexpr uint8_t kProgressMax = 200;
constexpr uint8_t kExecuting = 255;

// 0x1024 values the firmware implements. Anything else is unhandled by the drive.
constexpr uint8_t kModeExecuteImmediately = 0;
constexpr uint8_t kModeAbort = 3;

// Where a response's payload starts, which depends on whether byte 2 is spent on the OS error
// code: a successful reply carries six data bytes, a failed one five. Byte 1 is unused either way.
constexpr size_t kErrorCodeIndex = 2;
constexpr size_t kDataIndex = 2;
constexpr size_t kDataIndexAfterErrorCode = 3;

std::vector<uint8_t> responsePayload(const std::vector<uint8_t>& response, size_t from) {
  return {std::next(response.begin(), static_cast<std::ptrdiff_t>(from)), response.end()};
}

// Splits a terminal response (byte 0 already known to be 0-3) into its parts.
OsCommandResponse decodeResponse(const std::vector<uint8_t>& response) {
  OsCommandResponse decoded{};
  decoded.status = static_cast<OsCommandStatus>(response[0]);
  switch (decoded.status) {
    case OsCommandStatus::kCompletedWithData:
      decoded.data = responsePayload(response, kDataIndex);
      break;
    case OsCommandStatus::kFailedWithData:
      decoded.errorCode = response[kErrorCodeIndex];
      decoded.data = responsePayload(response, kDataIndexAfterErrorCode);
      break;
    case OsCommandStatus::kCompleted:
    case OsCommandStatus::kFailed:
      break;
  }
  return decoded;
}

// Decodes a big-endian integer from the front of a response payload.
//
// OS command payload integers are **big-endian** — the opposite of the little-endian convention the
// rest of the object dictionary follows — so the most significant byte comes first, which is where
// a payload starts. The width is per command: one byte for a pole pair count, two for a commutation
// offset, four for a resistance.
template <typename T>
std::expected<T, std::string> decodeBigEndian(const std::vector<uint8_t>& payload,
                                              std::string_view what) {
  if (payload.size() < sizeof(T)) {
    return std::unexpected(std::format("{} reported success with {} payload bytes, expected {}",
                                       what, payload.size(), sizeof(T)));
  }
  T value = 0;
  for (size_t i = 0; i < sizeof(T); ++i) {
    value = static_cast<T>(value << 8 | payload[i]);
  }
  return value;
}

// Whether a command has command-specific error codes, and if so which table.
//
// Motor phase order detection (4) has **none** — the specification says its status 3 can only ever
// carry a general code — so decoding its code 0 as the current-amplitude fault its siblings share
// would invent a motor diagnosis out of a code that means nothing.
enum class CommandSpecificFault {
  kNone,
  kCurrentAmplitude,  // Pole pair (7), phase resistance (8), phase inductance (9).
};

// Issues a parameterless motor-measurement command and returns its response payload.
//
// The shared front half of motor phase order (4), pole pair (7), phase resistance (8) and phase
// inductance (9): all take no parameters and differ only in how wide the value in the payload is —
// which each caller decodes itself with decodeBigEndian — and in whether they have a
// command-specific error code.
//
// A failed command is an error here rather than a result, which is the opposite of open phase
// detection: that command's failure *is* its finding, while these either produce a measurement or
// produce nothing, so there is nothing to report but the reason.
//
// @param what  How to name the command in an error message ("phase resistance measurement").
std::expected<std::vector<uint8_t>, std::string> runMotorMeasurement(
    SomanetDrive& drive, somanet::OsCommandId id, std::string_view what,
    CommandSpecificFault commandSpecificFault, const OsCommandConfig& config) {
  std::vector<uint8_t> command(kOsCommandSize, 0);
  command[0] = static_cast<uint8_t>(id);

  auto response = drive.runOsCommand(command, config);
  if (!response) {
    return std::unexpected(response.error());
  }

  if (response->failed()) {
    if (!response->errorCode) {
      // Status 2: failed, and the drive sent no code at all. Nothing more can be said than that.
      return std::unexpected(std::format("{} failed and the drive reported no error code", what));
    }
    const uint8_t code = *response->errorCode;
    if (auto general = osCommandErrorName(code)) {
      return std::unexpected(
          std::format("{} was not performed: {} (OS error {})", what, *general, code));
    }
    if (commandSpecificFault == CommandSpecificFault::kCurrentAmplitude &&
        code == static_cast<uint8_t>(somanet::MotorMeasurementFault::kCurrentAmplitudeError)) {
      return std::unexpected(
          std::format("{} failed: {}", what,
                      somanet::describe(somanet::MotorMeasurementFault::kCurrentAmplitudeError)));
    }
    // A command-specific code this build does not name — including any code from a command the
    // specification says has none. Report the number rather than swallowing it or guessing at a
    // meaning, so a firmware that grows the table is still actionable.
    return std::unexpected(
        std::format("{} failed with OS error {} (command-specific)", what, code));
  }
  return response->data;
}

}  // namespace

std::optional<std::string_view> osCommandErrorName(uint8_t code) {
  switch (static_cast<OsCommandError>(code)) {
    case OsCommandError::kNotAllowed:
      return "command not allowed";
    case OsCommandError::kAborted:
      return "command aborted";
    case OsCommandError::kTimeout:
      return "command timeout";
    case OsCommandError::kUnsupported:
      return "unsupported command";
    case OsCommandError::kReserved:
      return "reserved";
    default:
      return std::nullopt;
  }
}

std::expected<OsCommandResponse, std::string> SomanetDrive::runOsCommand(
    const std::vector<uint8_t>& command, const OsCommandConfig& config) {
  if (command.size() != kOsCommandSize) {
    return std::unexpected(
        std::format("an OS command is {} bytes, got {}", kOsCommandSize, command.size()));
  }
  const uint16_t position = device().slavePosition();
  const uint8_t id = command[0];

  // Restores 0x1024 once the master has forced an abort. The drive ignores a write to 0x1023:01
  // whenever the mode is non-zero, so a mode left at 3 would make the *next* command look like it
  // ran and return the previous one's response — hence a restore on every path out, not just the
  // successful one.
  struct ModeRestorer {
    SomanetDrive* drive = nullptr;
    uint16_t position = 0;
    ~ModeRestorer() {
      if (drive == nullptr) {
        return;
      }
      if (auto restored = drive->setOsCommandMode(kModeExecuteImmediately); !restored) {
        spdlog::error("Device {}: failed to restore OS command mode to {}: {}", position,
                      kModeExecuteImmediately, restored.error());
      }
    }
  } modeRestorer;

  // Make the precondition true rather than assuming it: a stale mode 3 (a process killed
  // mid-abort, another tool on the same bus) would otherwise let the command write succeed while
  // the drive quietly discards it.
  if (auto mode = setOsCommandMode(kModeExecuteImmediately); !mode) {
    return std::unexpected(mode.error());
  }
  // Writing 0x1023:01 executes the command. The drive refuses the write while a command is still
  // in progress (SDO abort 0x08000021, "local control"); that error already says so, so it is
  // forwarded as-is rather than reinterpreted here.
  if (auto written = setOsCommand(command); !written) {
    return std::unexpected(written.error());
  }

  std::string abortReason;
  auto deadline = std::chrono::steady_clock::now() + config.timeout;
  std::optional<uint8_t> lastPercent;

  for (;;) {
    // Read 0x1023:03, never 0x1023:02: both carry the status byte, but only reading the response
    // returns the drive to its idle state and re-arms 0x1023:01 for the next command.
    auto response = osCommandResponse();
    if (!response) {
      return std::unexpected(response.error());
    }
    if (response->size() < kOsCommandSize) {
      return std::unexpected(std::format("an OS command response is {} bytes, got {}",
                                         kOsCommandSize, response->size()));
    }

    const uint8_t status = (*response)[0];
    if (status <= static_cast<uint8_t>(OsCommandStatus::kFailedWithData)) {
      if (!abortReason.empty()) {
        return std::unexpected(abortReason);
      }
      return decodeResponse(*response);
    }
    if (status >= kProgressMin && status <= kProgressMax) {
      const uint8_t percent = static_cast<uint8_t>(status - kProgressMin);
      if (lastPercent != percent) {
        spdlog::debug("Device {}: OS command 0x{:02X} at {}%", position, id, percent);
        lastPercent = percent;
      }
    } else if (status != kExecuting) {
      return std::unexpected(std::format("unknown OS command status {}", status));
    }

    const auto now = std::chrono::steady_clock::now();
    if (abortReason.empty()) {
      std::string reason;
      if (config.stop.stop_requested()) {
        reason = std::format("OS command 0x{:02X} cancelled", id);
      } else if (now >= deadline) {
        reason =
            std::format("OS command 0x{:02X} timed out after {} ms", id, config.timeout.count());
      }
      if (!reason.empty()) {
        // Arm the restore before the abort write, so a write that fails after taking effect still
        // leaves the mode at 0.
        modeRestorer.drive = this;
        modeRestorer.position = position;
        spdlog::debug("Device {}: aborting OS command 0x{:02X} ({})", position, id, reason);
        if (auto aborted = setOsCommandMode(kModeAbort); !aborted) {
          return std::unexpected(std::format("{}; the abort failed: {}", reason, aborted.error()));
        }
        abortReason = std::move(reason);
        deadline = now + config.abortTimeout;
      }
    } else if (now >= deadline) {
      return std::unexpected(std::format("{}; the drive did not report the abort within {} ms",
                                         abortReason, config.abortTimeout.count()));
    }

    std::this_thread::sleep_for(config.pollInterval);
  }
}

void to_json(nlohmann::json& j, const BrakeState& state) {
  j = nlohmann::json{
      {"status", somanet::toString(state.status)},
      {"releaseStrategy", somanet::toString(state.releaseStrategy)},
      {"pullTimeMs", state.pullTime.count()},
      {"pullVoltageMv", state.pullVoltageMv},
      {"holdVoltageMv", state.holdVoltageMv},
      // Derived, and deliberately on the wire: without them a client has to know that strategy 0
      // means "commands do nothing" and strategy 2 means "releasing turns the motor".
      {"softwareControllable", state.softwareControllable()},
      {"releaseMovesShaft", state.releaseMovesShaft()},
  };
}

std::expected<somanet::BrakeStatus, std::string> SomanetDrive::brakeStatus() const {
  auto raw = device_.readValue<uint8_t>(somanet::kBrakeOptions, somanet::kBrakeStatus);
  if (!raw) {
    return std::unexpected(raw.error());
  }
  return static_cast<somanet::BrakeStatus>(*raw);
}

std::expected<void, std::string> SomanetDrive::setBrakeStatus(somanet::BrakeStatus status) {
  return device_.writeValue<uint8_t>(somanet::kBrakeOptions, somanet::kBrakeStatus,
                                     static_cast<uint8_t>(status));
}

std::expected<BrakeState, std::string> SomanetDrive::brakeState() const {
  BrakeState state;

  auto status = brakeStatus();
  if (!status) {
    return std::unexpected(status.error());
  }
  state.status = *status;

  auto strategy =
      device_.readValue<uint8_t>(somanet::kBrakeOptions, somanet::kBrakeReleaseStrategy);
  if (!strategy) {
    return std::unexpected(strategy.error());
  }
  state.releaseStrategy = static_cast<somanet::BrakeReleaseStrategy>(*strategy);

  auto pullTime = device_.readValue<uint16_t>(somanet::kBrakeOptions, somanet::kBrakePullTime);
  if (!pullTime) {
    return std::unexpected(pullTime.error());
  }
  state.pullTime = std::chrono::milliseconds(*pullTime);

  auto pullVoltage =
      device_.readValue<uint32_t>(somanet::kBrakeOptions, somanet::kBrakePullVoltage);
  if (!pullVoltage) {
    return std::unexpected(pullVoltage.error());
  }
  state.pullVoltageMv = *pullVoltage;

  auto holdVoltage =
      device_.readValue<uint32_t>(somanet::kBrakeOptions, somanet::kBrakeHoldVoltage);
  if (!holdVoltage) {
    return std::unexpected(holdVoltage.error());
  }
  state.holdVoltageMv = *holdVoltage;

  return state;
}

std::expected<BrakeState, std::string> SomanetDrive::releaseBrake(
    std::chrono::milliseconds settle) {
  auto state = brakeState();
  if (!state) {
    return std::unexpected(state.error());
  }
  // Nothing to do, and neither case is a failure: a manual brake is not the firmware's to drive,
  // and one already disengaged needs no second write (nor a second pull-time wait).
  if (!state->softwareControllable() || state->status == somanet::BrakeStatus::kDisengaged) {
    return *state;
  }

  if (auto written = setBrakeStatus(somanet::BrakeStatus::kDisengaged); !written) {
    return std::unexpected(written.error());
  }
  // The pull time is the firmware's own gate: it blocks motion, and motion-related OS commands,
  // until that window closes. Release is open-loop, so this wait is the whole guarantee.
  std::this_thread::sleep_for(state->pullTime + settle);

  return brakeState();
}

std::expected<BrakeState, std::string> SomanetDrive::engageBrake(std::chrono::milliseconds settle) {
  auto state = brakeState();
  if (!state) {
    return std::unexpected(state.error());
  }
  if (!state->softwareControllable()) {
    return *state;
  }

  if (auto written = setBrakeStatus(somanet::BrakeStatus::kEngaged); !written) {
    return std::unexpected(written.error());
  }
  // No pull time on the way in: the brake is spring-engaged, so this is the removal of voltage.
  std::this_thread::sleep_for(settle);

  return brakeState();
}

std::string OpenPhaseResult::describe() const {
  if (!phaseOpened) {
    return "no open phase detected";
  }
  if (fault) {
    return std::format("{} — {}", somanet::toString(*fault), somanet::describe(*fault));
  }
  // A code this build does not name: report the number rather than swallowing the finding, so a
  // firmware that extends the table is still actionable.
  return std::format("an open phase was detected (fault code {})",
                     faultCode ? std::to_string(*faultCode) : "unknown");
}

void to_json(nlohmann::json& j, const OpenPhaseResult& result) {
  j = nlohmann::json{{"phaseOpened", result.phaseOpened}, {"description", result.describe()}};
  if (result.faultCode) {
    j["faultCode"] = *result.faultCode;
  }
  if (result.fault) {
    j["fault"] = somanet::toString(*result.fault);
  }
}

std::expected<OpenPhaseResult, std::string> SomanetDrive::runOpenPhaseDetection(
    const OsCommandConfig& config) {
  std::vector<uint8_t> command(kOsCommandSize, 0);
  command[0] = static_cast<uint8_t>(somanet::OsCommandId::kOpenPhaseDetection);

  auto response = runOsCommand(command, config);
  if (!response) {
    return std::unexpected(response.error());
  }

  OpenPhaseResult result;
  if (!response->failed()) {
    // Success is the good news: the drive checked every phase and found none open.
    return result;
  }

  // A failure is the finding — but only when it is command-specific. A general code (251 "command
  // not allowed", 253 timeout, ...) says the drive never performed the check, and reporting that as
  // "a phase is open" would be a false hardware fault, which is far worse than an error.
  if (response->errorCode) {
    if (auto general = osCommandErrorName(*response->errorCode)) {
      return std::unexpected(std::format("open phase detection was not performed: {} (OS error {})",
                                         *general, *response->errorCode));
    }
    result.faultCode = *response->errorCode;
    if (*response->errorCode <= static_cast<uint8_t>(somanet::OpenPhaseFault::kOpenFetCLow)) {
      result.fault = static_cast<somanet::OpenPhaseFault>(*response->errorCode);
    }
  }
  // Status 2 (failed, no data) carries no code at all; the drive still said a phase is open.
  result.phaseOpened = true;
  return result;
}

std::string MotorPhaseOrderResult::describe() const {
  return std::format("motor phase order is {}", somanet::toString(order));
}

void to_json(nlohmann::json& j, const MotorPhaseOrderResult& result) {
  j = nlohmann::json{{"order", somanet::toString(result.order)},
                     {"inverted", result.inverted()},
                     {"description", result.describe()}};
}

std::expected<MotorPhaseOrderResult, std::string> SomanetDrive::runMotorPhaseOrderDetection(
    const OsCommandConfig& config) {
  static constexpr std::string_view kWhat = "motor phase order detection";
  auto payload = runMotorMeasurement(*this, somanet::OsCommandId::kMotorPhaseOrderDetection, kWhat,
                                     CommandSpecificFault::kNone, config);
  if (!payload) {
    return std::unexpected(payload.error());
  }
  auto order = decodeBigEndian<uint8_t>(*payload, kWhat);
  if (!order) {
    return std::unexpected(order.error());
  }
  // Only 0 and 1 are defined. Anything else is a value this build cannot interpret, and guessing at
  // it would misreport how a motor is wired — which the next command, commutation offset
  // measurement, then builds on.
  if (*order > static_cast<uint8_t>(somanet::MotorPhaseOrder::kInverted)) {
    return std::unexpected(std::format(
        "{} reported motor phase order {}, which is neither normal (0) nor inverted (1)", kWhat,
        *order));
  }
  return MotorPhaseOrderResult{.order = static_cast<somanet::MotorPhaseOrder>(*order)};
}

std::string PolePairResult::describe() const {
  return std::format("{} pole pair{}", polePairs, polePairs == 1 ? "" : "s");
}

void to_json(nlohmann::json& j, const PolePairResult& result) {
  j = nlohmann::json{{"polePairs", result.polePairs}, {"description", result.describe()}};
}

std::expected<PolePairResult, std::string> SomanetDrive::runPolePairDetection(
    const OsCommandConfig& config) {
  static constexpr std::string_view kWhat = "pole pair detection";
  auto payload = runMotorMeasurement(*this, somanet::OsCommandId::kPolePairDetection, kWhat,
                                     CommandSpecificFault::kCurrentAmplitude, config);
  if (!payload) {
    return std::unexpected(payload.error());
  }
  auto polePairs = decodeBigEndian<uint8_t>(*payload, kWhat);
  if (!polePairs) {
    return std::unexpected(polePairs.error());
  }
  return PolePairResult{.polePairs = *polePairs};
}

std::string PhaseResistanceResult::describe() const {
  return std::format("phase resistance {} mΩ", milliohms);
}

void to_json(nlohmann::json& j, const PhaseResistanceResult& result) {
  j = nlohmann::json{{"milliohms", result.milliohms}, {"description", result.describe()}};
}

std::expected<PhaseResistanceResult, std::string> SomanetDrive::runPhaseResistanceMeasurement(
    const OsCommandConfig& config) {
  static constexpr std::string_view kWhat = "phase resistance measurement";
  auto payload = runMotorMeasurement(*this, somanet::OsCommandId::kPhaseResistanceMeasurement,
                                     kWhat, CommandSpecificFault::kCurrentAmplitude, config);
  if (!payload) {
    return std::unexpected(payload.error());
  }
  auto milliohms = decodeBigEndian<uint32_t>(*payload, kWhat);
  if (!milliohms) {
    return std::unexpected(milliohms.error());
  }
  return PhaseResistanceResult{.milliohms = *milliohms};
}

std::string PhaseInductanceResult::describe() const {
  return std::format("phase inductance {} µH", microhenries);
}

void to_json(nlohmann::json& j, const PhaseInductanceResult& result) {
  j = nlohmann::json{{"microhenries", result.microhenries}, {"description", result.describe()}};
}

std::expected<PhaseInductanceResult, std::string> SomanetDrive::runPhaseInductanceMeasurement(
    const OsCommandConfig& config) {
  static constexpr std::string_view kWhat = "phase inductance measurement";
  auto payload = runMotorMeasurement(*this, somanet::OsCommandId::kPhaseInductanceMeasurement,
                                     kWhat, CommandSpecificFault::kCurrentAmplitude, config);
  if (!payload) {
    return std::unexpected(payload.error());
  }
  auto microhenries = decodeBigEndian<uint32_t>(*payload, kWhat);
  if (!microhenries) {
    return std::unexpected(microhenries.error());
  }
  return PhaseInductanceResult{.microhenries = *microhenries};
}

std::expected<void, std::string> SomanetDrive::setOperationMode(somanet::OperationMode mode) {
  return setOperationModeValue(static_cast<int8_t>(mode));
}

std::expected<int8_t, std::string> SomanetDrive::operationModeValue() const {
  return device_.readValue<int8_t>(cia402::kModeOfOperation, 0);
}

std::expected<void, std::string> SomanetDrive::setOperationModeValue(int8_t mode) {
  return device_.writeValue<int8_t>(cia402::kModeOfOperation, 0, mode);
}

std::expected<SomanetDrive, std::string> createSomanetDrive(Device& device) {
  if (device.vendorId() != kSynapticonVendorId) {
    return std::unexpected(
        std::format("device {} is not a SOMANET drive (vendor 0x{:08X}, expected 0x{:08X})",
                    device.slavePosition(), device.vendorId(), kSynapticonVendorId));
  }
  // A SOMANET drive must also be a CiA402 drive; reuse that check rather than duplicating it.
  if (auto cia = createCia402Drive(device); !cia) {
    return std::unexpected(cia.error());
  }
  return SomanetDrive(device);
}

}  // namespace mm::node
