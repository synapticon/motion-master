#include "node/somanet_drive.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <bit>
#include <charconv>
#include <format>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "core/util.h"
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
  // core::fromBytes zero-pads a short buffer rather than refusing it, which is right for an SDO
  // value of a declared type and wrong here — a measurement command answering with too few bytes
  // produced no measurement — so the length is checked above and the decode reused.
  return core::fromBytes<T>(payload, std::endian::big);
}

// Whether a command has command-specific error codes, and if so which table.
//
// Motor phase order detection (4) and torque constant measurement (10) have **none** — neither
// defines a command-specific code, so their status 3 can only ever carry a general one — and
// decoding a code 0 from either as the current-amplitude fault its siblings share would invent a
// motor diagnosis out of a code that means nothing.
enum class CommandSpecificFault {
  kNone,
  kCurrentAmplitude,  // Pole pair (7), phase resistance (8), phase inductance (9).
};

// Names what the drive is doing, to be appended to a general OS error that does not say.
//
// Only for the two codes that mean the drive refused or dropped the command over its own
// preconditions, and they are the two that need it most: **251 and 252 name a *precondition*
// without naming which one**. The firmware re-checks the whole set — diagnostics mode, Operation
// Enabled, no limit switch, the brake — on every control cycle a command runs, and aborts with 252
// the moment one stops holding. So a drive that faulted half way through an eleven-second
// measurement reports exactly what a drive that was never enabled reports, and the fault that
// caused it appears nowhere in the message.
//
// Best-effort and quiet: an empty suffix when the state cannot be read, or when the drive is still
// enabled and so is not itself the explanation — leaving the caller's message as it was rather than
// padding it with a state that rules nothing out.
std::string driveStateSuffix(const SomanetDrive& drive, uint8_t code) {
  if (code != static_cast<uint8_t>(OsCommandError::kNotAllowed) &&
      code != static_cast<uint8_t>(OsCommandError::kAborted)) {
    return {};
  }
  auto state = drive.state();
  if (!state || *state == cia402::State::kOperationEnabled) {
    return {};
  }
  auto suffix = std::format(" — the drive is in {}", cia402::toString(*state));
  if (*state == cia402::State::kFault || *state == cia402::State::kFaultReactionActive) {
    // Best-effort within a best-effort: a description is what makes the fault actionable, but
    // failing to read one must not cost the state we already know.
    if (auto report = drive.errorReport(); report && !report->empty()) {
      suffix += std::format(" (drive error report: {})", *report);
    }
  }
  return suffix;
}

// Issues a parameterless motor-measurement command and returns its response payload.
//
// The shared front half of motor phase order (4), pole pair (7), phase resistance (8), phase
// inductance (9) and torque constant (10): all take no parameters and differ only in how wide the
// value in the payload is — which each caller decodes itself with decodeBigEndian — and in whether
// they have a command-specific error code.
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
      return std::unexpected(std::format("{} was not performed: {} (OS error {}){}", what, *general,
                                         code, driveStateSuffix(drive, code)));
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

// Byte layout of the encoder register communication request (command 0) — the one command here
// that takes parameters, so the only place an OS command request is more than its ID.
constexpr size_t kEncoderOrdinalByte = 1;
constexpr size_t kEncoderAccessByte = 2;
constexpr size_t kEncoderRegisterAddressByte = 3;
constexpr size_t kEncoderRegisterValueByte = 4;

// Byte 2 is `(slaveAddress << 1) | direction`. The BiSS service supports a single slave per device,
// so the address is always 0 and the byte is the direction bit alone — which is why these are the
// two packed byte values rather than a shift over a constant zero. A firmware that ever addressed a
// second slave would build this byte instead of choosing between two constants.
constexpr uint8_t kEncoderRegisterRead = 0;
constexpr uint8_t kEncoderRegisterWrite = 1;

// iC-MU calibration mode (command 1) packs its mode above the encoder ordinal in the same byte the
// ordinal occupies alone in command 0 — bits 0-2 the ordinal, bits 3-4 the mode.
constexpr unsigned kIcMuModeShift = 3;

// Reads or writes one encoder register — the shared body of readEncoderRegister and
// writeEncoderRegister, which differ only in the direction bit and in whether the value byte is
// used. The drive answers both the same way, by reporting what the register holds.
std::expected<EncoderRegisterResult, std::string> accessEncoderRegister(
    SomanetDrive& drive, somanet::EncoderOrdinal encoder, bool write, uint8_t registerAddress,
    uint8_t value, const OsCommandConfig& config) {
  const std::string what = std::format("{} {} register 0x{:02X}", write ? "writing" : "reading",
                                       somanet::toString(encoder), registerAddress);

  std::vector<uint8_t> command(kOsCommandSize, 0);
  command[0] = static_cast<uint8_t>(somanet::OsCommandId::kEncoderRegisterCommunication);
  command[kEncoderOrdinalByte] = static_cast<uint8_t>(encoder);
  command[kEncoderAccessByte] = write ? kEncoderRegisterWrite : kEncoderRegisterRead;
  command[kEncoderRegisterAddressByte] = registerAddress;
  // Ignored by the drive on a read, and sent as 0 rather than as whatever the caller passed, so a
  // read is one request whatever it was called with.
  command[kEncoderRegisterValueByte] = write ? value : 0;

  auto response = drive.runOsCommand(command, config);
  if (!response) {
    return std::unexpected(response.error());
  }

  if (response->failed()) {
    if (!response->errorCode) {
      // Status 2: the register transaction failed and the drive sent no code with it. The
      // specification names the causes, and every one of them is something a user can act on, so
      // they are named here rather than leaving a bare "failed".
      return std::unexpected(
          std::format("{} failed: the register transaction did not complete, which a BiSS timeout "
                      "set too short, a BiSS clock frequency set too high, or a register that does "
                      "not exist can all cause",
                      what));
    }
    const uint8_t code = *response->errorCode;
    if (auto general = osCommandErrorName(code)) {
      return std::unexpected(
          std::format("{} was not performed: {} (OS error {})", what, *general, code));
    }
    if (code <= static_cast<uint8_t>(somanet::EncoderRegisterFault::kRegisterNotAllowed)) {
      const auto fault = static_cast<somanet::EncoderRegisterFault>(code);
      return std::unexpected(std::format("{} failed: {} — {}", what, somanet::toString(fault),
                                         somanet::describe(fault)));
    }
    // A command-specific code this build does not name. Report the number rather than guessing, so
    // a firmware that grows the table stays actionable.
    return std::unexpected(
        std::format("{} failed with OS error {} (command-specific)", what, code));
  }

  EncoderRegisterResult result{.encoder = encoder,
                               .registerAddress = registerAddress,
                               .wrote = write,
                               .value = std::nullopt};
  if (response->status == OsCommandStatus::kCompletedWithData) {
    auto reported = decodeBigEndian<uint8_t>(response->data, what);
    if (!reported) {
      return std::unexpected(reported.error());
    }
    result.value = *reported;
  }
  // Status 0 is a completion with no response at all. The firmware documents exactly one access
  // that answers this way — the iC-MU soft reset, which restarts the chip — so it is a success
  // without a reading rather than a truncated payload.
  return result;
}

// Byte layout of the HRD streaming request (command 3). The drive reads the data index and the
// duration only for the configure action; a start request is the ID and the action alone.
constexpr size_t kHrdActionByte = 1;
constexpr size_t kHrdDataIndexByte = 2;
constexpr size_t kHrdDurationMsbByte = 3;
constexpr size_t kHrdDurationLsbByte = 4;

constexpr uint8_t kHrdConfigureAction = 0;
constexpr uint8_t kHrdStartAction = 1;

// The two 14-bit tracks packed into one raw encoder sample: master in bits 0-13, nonius above it.
constexpr uint32_t kHrdTrackMask = 0x3FFF;
constexpr unsigned kHrdNoniusShift = 14;

// Fractional bits of the Q15 fixed-point velocity in a system identification sample.
constexpr unsigned kHrdVelocityFractionalBits = 15;

// The pseudo-file Synapticon firmware serves its directory as. Not a file on the device — reading
// this name runs a listing.
constexpr std::string_view kFileListName = "fs-getlist";

// The two files that say what a device is. Both begin with a period, which is what makes the
// bootloader protect them from being modified or deleted without a password (specification §3.1).
constexpr std::string_view kHardwareDescriptionName = ".hardware_description";
constexpr std::string_view kVariantName = ".variant";

// Names a failed HRD streaming response. Shared by both actions because the drive answers them from
// the same error table, and a configure that fails for a reason a start could also fail for should
// not read differently.
std::string describeHrdFailure(const OsCommandResponse& response, std::string_view what) {
  if (!response.errorCode) {
    // Status 2: failed with no code. Nothing more can be said than that it did not happen.
    return std::format("{} failed and the drive reported no error code", what);
  }
  const uint8_t code = *response.errorCode;
  if (auto general = osCommandErrorName(code)) {
    return std::format("{} was not performed: {} (OS error {})", what, *general, code);
  }
  if (code <= static_cast<uint8_t>(somanet::HrdStreamFault::kAction)) {
    const auto fault = static_cast<somanet::HrdStreamFault>(code);
    return std::format("{} failed: {} — {}", what, somanet::toString(fault),
                       somanet::describe(fault));
  }
  // A command-specific code this build does not name. Report the number rather than guessing, so a
  // firmware that grows the table stays actionable.
  return std::format("{} failed with OS error {} (command-specific)", what, code);
}

// The recording index of an HRD file, or nullopt when @p name is not one. The number is what orders
// them, and it has to be read as a number: a lexicographic sort of the names would put a tenth file
// before the second, silently reassembling the stream out of order.
std::optional<uint32_t> hrdFileIndex(std::string_view name) {
  constexpr std::string_view kPrefix = "hr_data";
  constexpr std::string_view kSuffix = ".bin";
  if (!name.starts_with(kPrefix) || !name.ends_with(kSuffix)) {
    return std::nullopt;
  }
  const auto digits = name.substr(kPrefix.size(), name.size() - kPrefix.size() - kSuffix.size());
  if (digits.empty()) {
    return std::nullopt;
  }
  uint32_t index = 0;
  auto [p, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), index);
  if (ec != std::errc() || p != digits.data() + digits.size()) {
    return std::nullopt;
  }
  return index;
}

// Reads the `, size: <bytes>` suffix of a file list line, or nullopt when @p tail is not one.
std::optional<size_t> parseFileSizeSuffix(std::string_view tail) {
  constexpr std::string_view kSizeLabel = "size:";
  const auto skipSpace = [](std::string_view text) {
    const auto at = text.find_first_not_of(" \t");
    return at == std::string_view::npos ? std::string_view{} : text.substr(at);
  };
  tail = skipSpace(tail);
  if (!tail.starts_with(kSizeLabel)) {
    return std::nullopt;
  }
  tail = skipSpace(tail.substr(kSizeLabel.size()));
  size_t size = 0;
  auto [p, ec] = std::from_chars(tail.data(), tail.data() + tail.size(), size);
  if (ec != std::errc()) {
    return std::nullopt;
  }
  // Anything but trailing space after the digits means the line is not a size line after all.
  if (!skipSpace(std::string_view(p, static_cast<size_t>(tail.data() + tail.size() - p))).empty()) {
    return std::nullopt;
  }
  return size;
}

}  // namespace

namespace somanet {

std::optional<HrdData> parseHrdData(std::string_view token) {
  if (token == toString(HrdData::kEncoderRawData)) {
    return HrdData::kEncoderRawData;
  }
  if (token == toString(HrdData::kSystemIdentificationData)) {
    return HrdData::kSystemIdentificationData;
  }
  return std::nullopt;
}

std::optional<ChirpSignalType> parseChirpSignalType(std::string_view token) {
  if (token == toString(ChirpSignalType::kLogarithmic)) {
    return ChirpSignalType::kLogarithmic;
  }
  if (token == toString(ChirpSignalType::kLinear)) {
    return ChirpSignalType::kLinear;
  }
  return std::nullopt;
}

std::optional<SystemIdentificationStart> parseSystemIdentificationStart(std::string_view token) {
  if (token == toString(SystemIdentificationStart::kNone)) {
    return SystemIdentificationStart::kNone;
  }
  if (token == toString(SystemIdentificationStart::kImmediately)) {
    return SystemIdentificationStart::kImmediately;
  }
  if (token == toString(SystemIdentificationStart::kAfterHrdStreamStart)) {
    return SystemIdentificationStart::kAfterHrdStreamStart;
  }
  return std::nullopt;
}

std::optional<FirmwareService> parseFirmwareService(std::string_view token) {
  if (token == toString(FirmwareService::kDriveControl)) {
    return FirmwareService::kDriveControl;
  }
  if (token == toString(FirmwareService::kMotionControl)) {
    return FirmwareService::kMotionControl;
  }
  return std::nullopt;
}

std::optional<IcMuCalibrationMode> parseIcMuCalibrationMode(std::string_view token) {
  if (token == toString(IcMuCalibrationMode::kConfiguration)) {
    return IcMuCalibrationMode::kConfiguration;
  }
  if (token == toString(IcMuCalibrationMode::kRaw)) {
    return IcMuCalibrationMode::kRaw;
  }
  if (token == toString(IcMuCalibrationMode::kStandard)) {
    return IcMuCalibrationMode::kStandard;
  }
  return std::nullopt;
}

}  // namespace somanet

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

  // Drain any response left unread, because the drive returns to its idle state only when 0x1023:03
  // has been read. **Both preconditions for accepting a command are made true rather than assumed,
  // and this is the second of them**: a drive still holding an unread response ignores the write to
  // 0x1023:01 exactly as silently as one whose mode is not 0, and then the first poll below reads
  // that *stale* terminal status and reports it as this command's verdict — a failure this command
  // never produced, or worse a success with someone else's payload. A response can be left behind
  // by any client on the bus, or by this process being killed between the write and the read.
  //
  // Reading it while a command is genuinely in progress is harmless: that returns 255 and drains
  // nothing. A failed read is not fatal either — the command write below will fail for the same
  // reason if the mailbox is really unusable — so it is logged and stepped over rather than
  // returned, which keeps a drive that answers oddly here from being unusable.
  if (auto drained = osCommandResponse(); !drained) {
    spdlog::debug("Device {}: could not drain the OS command response before command 0x{:02X}: {}",
                  position, id, drained.error());
  }

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
      // Logged with the whole reply, not just the status: a terminal answer that turns out to be
      // wrong on hardware — a stale response, a payload of an unexpected width — is diagnosable
      // from the bytes and from nothing else, and by the time a caller sees the error they are
      // gone.
      spdlog::debug("Device {}: OS command 0x{:02X} terminal response [{}]", position, id,
                    core::toHex(*response, " "));
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

std::expected<std::string, std::string> SomanetDrive::errorReport() const {
  return device_.readValue<std::string>(somanet::kErrorReport, somanet::kErrorReportDescription);
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

std::expected<void, std::string> SomanetDrive::setIcMuCalibrationMode(
    somanet::EncoderOrdinal encoder, somanet::IcMuCalibrationMode mode,
    const OsCommandConfig& config) {
  const std::string what =
      std::format("setting {} to {} mode", somanet::toString(encoder), somanet::toString(mode));

  std::vector<uint8_t> command(kOsCommandSize, 0);
  command[0] = static_cast<uint8_t>(somanet::OsCommandId::kIcMuCalibrationMode);
  // One byte carries both fields: the ordinal in bits 0-2 and the mode above it in bits 3-4.
  command[kEncoderOrdinalByte] = static_cast<uint8_t>(
      (static_cast<uint8_t>(mode) << kIcMuModeShift) | static_cast<uint8_t>(encoder));

  auto response = runOsCommand(command, config);
  if (!response) {
    return std::unexpected(response.error());
  }

  if (response->failed()) {
    if (!response->errorCode) {
      // Status 2, the only failure this command reports itself: the sensor service did not
      // recognise the mode. It cannot be reached through this typed surface — the enum is closed —
      // so it means the firmware and this build disagree about the mode numbering.
      return std::unexpected(
          std::format("{} failed: the sensor service did not accept the mode value", what));
    }
    // The specification gives this command no command-specific codes at all, so anything here is a
    // general one — and a code that is not general either is reported as the number rather than
    // decoded against a table that does not exist.
    const uint8_t code = *response->errorCode;
    if (auto general = osCommandErrorName(code)) {
      return std::unexpected(
          std::format("{} was not performed: {} (OS error {})", what, *general, code));
    }
    return std::unexpected(std::format("{} failed with OS error {}", what, code));
  }
  return {};
}

std::vector<DeviceFile> parseDeviceFileList(std::string_view text) {
  std::vector<DeviceFile> files;
  while (!text.empty()) {
    const auto newline = text.find('\n');
    std::string_view line = text.substr(0, newline);
    text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);

    // The firmware's own line endings are not specified anywhere, so both are accepted.
    if (line.ends_with('\r')) {
      line.remove_suffix(1);
    }
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
      continue;
    }
    line = line.substr(first, line.find_last_not_of(" \t") - first + 1);

    // The size follows the *first* comma whose remainder is a size — not simply the first comma,
    // because a filename may contain one and only the tail says which comma was the separator.
    DeviceFile file{.name = std::string(line), .byteCount = std::nullopt};
    for (size_t comma = line.find(','); comma != std::string_view::npos;
         comma = line.find(',', comma + 1)) {
      if (auto size = parseFileSizeSuffix(line.substr(comma + 1))) {
        auto name = line.substr(0, comma);
        if (const auto last = name.find_last_not_of(" \t"); last != std::string_view::npos) {
          name = name.substr(0, last + 1);
        }
        file = DeviceFile{.name = std::string(name), .byteCount = size};
        break;
      }
    }
    files.push_back(std::move(file));
  }
  return files;
}

void to_json(nlohmann::json& j, const DeviceFile& file) {
  j = nlohmann::json{{"name", file.name}};
  // Omitted rather than null when the device reported no size, following the convention every
  // optional on this surface follows: a null would read as "zero bytes" to a client that only
  // checks for the key.
  if (file.byteCount) {
    j["byteCount"] = *file.byteCount;
  }
}

std::vector<std::string_view> hrdColumns(somanet::HrdData data) {
  if (data == somanet::HrdData::kEncoderRawData) {
    return {"raw", "masterCount", "noniusCount"};
  }
  return {"velocityRpm", "torquePermil"};
}

size_t HrdRecording::sampleCount() const {
  if (const auto* encoder = std::get_if<std::vector<HrdEncoderSample>>(&samples)) {
    return encoder->size();
  }
  return std::get<std::vector<HrdSystemIdentificationSample>>(samples).size();
}

void to_json(nlohmann::json& j, const HrdRecording& recording) {
  j = nlohmann::json{
      {"data", somanet::toString(recording.data)}, {"files", recording.files},
      {"byteCount", recording.byteCount},          {"trailingBytes", recording.trailingBytes},
      {"sampleCount", recording.sampleCount()},    {"columns", hrdColumns(recording.data)}};

  auto& rows = j["samples"] = nlohmann::json::array();
  if (const auto* encoder = std::get_if<std::vector<HrdEncoderSample>>(&recording.samples)) {
    for (const auto& sample : *encoder) {
      rows.push_back({sample.raw, sample.masterCount, sample.noniusCount});
    }
    return;
  }
  for (const auto& sample :
       std::get<std::vector<HrdSystemIdentificationSample>>(recording.samples)) {
    rows.push_back({sample.velocityRpm, sample.torquePermil});
  }
}

std::string toCsv(const HrdRecording& recording) {
  std::string csv;
  // A recording is up to ten thousand rows of three numbers; growing the string a row at a time
  // would reallocate its way there.
  csv.reserve(recording.sampleCount() * 32 + 64);

  const auto columns = hrdColumns(recording.data);
  for (size_t i = 0; i < columns.size(); ++i) {
    csv += i == 0 ? "" : ",";
    csv += columns[i];
  }
  csv += '\n';

  if (const auto* encoder = std::get_if<std::vector<HrdEncoderSample>>(&recording.samples)) {
    for (const auto& sample : *encoder) {
      std::format_to(std::back_inserter(csv), "{},{},{}\n", sample.raw, sample.masterCount,
                     sample.noniusCount);
    }
    return csv;
  }
  for (const auto& sample :
       std::get<std::vector<HrdSystemIdentificationSample>>(recording.samples)) {
    // The velocity is the one non-integer column. Left to std::format's shortest round-trip form
    // rather than fixed to n decimals: it came out of a Q15 fixed point, so it has an exact short
    // decimal form and padding it would only add noise.
    std::format_to(std::back_inserter(csv), "{},{}\n", sample.velocityRpm, sample.torquePermil);
  }
  return csv;
}

HrdSamples decodeHrdSamples(std::span<const uint8_t> bytes, somanet::HrdData data) {
  const size_t sampleSize = somanet::hrdSampleSize(data);
  const size_t count = bytes.size() / sampleSize;

  if (data == somanet::HrdData::kEncoderRawData) {
    std::vector<HrdEncoderSample> samples;
    samples.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      const uint8_t* at = bytes.data() + i * sampleSize;
      const uint32_t raw = static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8) |
                           (static_cast<uint32_t>(at[2]) << 16) |
                           (static_cast<uint32_t>(at[3]) << 24);
      samples.push_back(HrdEncoderSample{
          .raw = raw,
          .masterCount = static_cast<uint16_t>(raw & kHrdTrackMask),
          .noniusCount = static_cast<uint16_t>((raw >> kHrdNoniusShift) & kHrdTrackMask)});
    }
    return samples;
  }

  std::vector<HrdSystemIdentificationSample> samples;
  samples.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const uint8_t* at = bytes.data() + i * sampleSize;
    const uint32_t velocityBits =
        static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8) |
        (static_cast<uint32_t>(at[2]) << 16) | (static_cast<uint32_t>(at[3]) << 24);
    const uint16_t torqueBits =
        static_cast<uint16_t>(static_cast<uint16_t>(at[4]) | (static_cast<uint16_t>(at[5]) << 8));
    samples.push_back(HrdSystemIdentificationSample{
        .velocityRpm = static_cast<double>(static_cast<int32_t>(velocityBits)) /
                       static_cast<double>(1U << kHrdVelocityFractionalBits),
        .torquePermil = static_cast<int16_t>(torqueBits)});
  }
  return samples;
}

std::expected<void, std::string> SomanetDrive::configureHrdStream(
    somanet::HrdData data, std::chrono::milliseconds duration, const OsCommandConfig& config) {
  const std::string what =
      std::format("configuring a {} ms {} recording", duration.count(), somanet::toString(data));

  // Refused here rather than sent: the drive rejects a duration over 10000 ms but *accepts* an
  // over-long system identification stream, then overruns its five files and truncates the
  // recording mid-sample. See somanet::maxHrdStreamDuration.
  const auto maxDuration = somanet::maxHrdStreamDuration(data);
  if (duration <= std::chrono::milliseconds::zero() || duration > maxDuration) {
    return std::unexpected(std::format("{} is not possible: {} can be recorded for 1 to {} ms",
                                       what, somanet::toString(data), maxDuration.count()));
  }

  std::vector<uint8_t> command(kOsCommandSize, 0);
  command[0] = static_cast<uint8_t>(somanet::OsCommandId::kHrdStreaming);
  command[kHrdActionByte] = kHrdConfigureAction;
  command[kHrdDataIndexByte] = static_cast<uint8_t>(data);
  // Big-endian, like every other multi-byte OS command payload — and safe to narrow because the
  // check above bounds the duration well inside 16 bits.
  const auto milliseconds = static_cast<uint16_t>(duration.count());
  command[kHrdDurationMsbByte] = static_cast<uint8_t>(milliseconds >> 8);
  command[kHrdDurationLsbByte] = static_cast<uint8_t>(milliseconds & 0xFF);

  auto response = runOsCommand(command, config);
  if (!response) {
    return std::unexpected(response.error());
  }
  if (response->failed()) {
    return std::unexpected(describeHrdFailure(*response, what));
  }
  return {};
}

std::expected<void, std::string> SomanetDrive::startHrdStream(const OsCommandConfig& config) {
  constexpr std::string_view kWhat = "recording the configured HRD stream";

  std::vector<uint8_t> command(kOsCommandSize, 0);
  command[0] = static_cast<uint8_t>(somanet::OsCommandId::kHrdStreaming);
  command[kHrdActionByte] = kHrdStartAction;

  auto response = runOsCommand(command, config);
  if (!response) {
    return std::unexpected(response.error());
  }
  if (response->failed()) {
    return std::unexpected(describeHrdFailure(*response, kWhat));
  }
  return {};
}

std::expected<std::vector<DeviceFile>, std::string> SomanetDrive::readFileList() const {
  auto listing = device().readFile(std::string(kFileListName));
  if (!listing) {
    return std::unexpected(std::format("reading the device file list ('{}') failed: {}",
                                       kFileListName, listing.error().message));
  }
  return parseDeviceFileList(std::string(listing->begin(), listing->end()));
}

std::expected<HardwareDescription, std::string> SomanetDrive::readHardwareDescription() const {
  auto content = device().readFile(std::string(kHardwareDescriptionName));
  if (!content) {
    return std::unexpected(
        std::format("reading '{}' failed: {}", kHardwareDescriptionName, content.error().message));
  }
  auto description = parseHardwareDescription(std::string(content->begin(), content->end()));
  if (!description) {
    return std::unexpected(std::format("'{}' on the device could not be read: {}",
                                       kHardwareDescriptionName, description.error()));
  }
  return description;
}

std::expected<std::optional<IntegroVariant>, std::string> SomanetDrive::readIntegroVariant() const {
  auto content = device().readFile(std::string(kVariantName));
  if (!content) {
    // Any FoE failure means "no variant here", not just FileNotFound. A device without the file
    // answers with a generic FoE Error packet, which SOEM reports as EC_ERR_TYPE_FOE_ERROR — the
    // same return as no answer at all (see foeError in soem_fieldbus_driver.cc). Missing and failed
    // cannot be told apart, so checking for FileNotFound alone made every Node and Circulo fail.
    //
    // Callers read the hardware description first, where a failure is fatal. FoE is therefore known
    // to work by the time this runs, so a failure here is about the file rather than the bus.
    return std::nullopt;
  }
  auto variant = parseIntegroVariant(*content);
  if (!variant) {
    // Read but undecodable stays an error: the device has a variant this build cannot make sense
    // of, which is worth reporting rather than answering "no variant" and building a descriptor
    // without its fieldbus character.
    return std::unexpected(
        std::format("'{}' on the device could not be read: {}", kVariantName, variant.error()));
  }
  return *variant;
}

std::expected<FullFirmwareDescriptors, std::string> SomanetDrive::readFullFirmwareDescriptors()
    const {
  auto description = readHardwareDescription();
  if (!description) {
    return std::unexpected(description.error());
  }
  auto variant = readIntegroVariant();
  if (!variant) {
    return std::unexpected(variant.error());
  }
  return fullFirmwareDescriptors(*description, variant->has_value() ? &**variant : nullptr);
}

std::expected<FirmwareCompatibility, std::string> SomanetDrive::checkFirmwarePackage(
    std::string_view packageFilename) const {
  // The filename first, so a name that is not a package name costs no bus traffic at all.
  auto name = parseFirmwarePackageName(packageFilename);
  if (!name) {
    return std::unexpected(name.error());
  }
  auto description = readHardwareDescription();
  if (!description) {
    return std::unexpected(description.error());
  }
  auto variant = readIntegroVariant();
  if (!variant) {
    return std::unexpected(variant.error());
  }
  return checkFirmwareCompatibility(*description, *name,
                                    variant->has_value() ? &**variant : nullptr);
}

std::expected<HrdRecording, std::string> SomanetDrive::readHrdRecording(
    somanet::HrdData data) const {
  auto listing = readFileList();
  if (!listing) {
    return std::unexpected(listing.error());
  }

  std::vector<std::pair<uint32_t, DeviceFile>> recorded;
  for (auto& file : *listing) {
    if (auto index = hrdFileIndex(file.name)) {
      recorded.emplace_back(*index, file);
    }
  }
  if (recorded.empty()) {
    return std::unexpected(
        "the device holds no high resolution data recording (it lists no hr_data<n>.bin file) — "
        "record "
        "one before reading it back");
  }
  std::ranges::sort(recorded, {}, &std::pair<uint32_t, DeviceFile>::first);

  HrdRecording recording;
  recording.data = data;
  std::vector<uint8_t> bytes;
  for (const auto& [index, file] : recorded) {
    auto content = device().readFile(file.name);
    // Fatal rather than skipped: the files are one byte stream chunked at a fixed size, so a
    // missing middle file does not cost its own samples but misaligns every sample after it — a
    // recording that decodes to plausible nonsense. A failure here is worth a retry, not a graph.
    if (!content) {
      return std::unexpected(std::format("reading '{}' of the recording failed: {}", file.name,
                                         content.error().message));
    }
    bytes.insert(bytes.end(), content->begin(), content->end());
    recording.files.push_back(file);
  }

  recording.byteCount = bytes.size();
  recording.trailingBytes = bytes.size() % somanet::hrdSampleSize(data);
  recording.samples = decodeHrdSamples(bytes, data);
  return recording;
}

std::string EncoderRegisterResult::describe() const {
  if (!value) {
    return std::format("{} register 0x{:02X} {} acknowledged without a response",
                       somanet::toString(encoder), registerAddress, wrote ? "write" : "read");
  }
  return std::format("{} register 0x{:02X} = 0x{:02X} ({})", somanet::toString(encoder),
                     registerAddress, *value, *value);
}

void to_json(nlohmann::json& j, const EncoderRegisterResult& result) {
  j = nlohmann::json{{"encoder", static_cast<uint8_t>(result.encoder)},
                     {"registerAddress", result.registerAddress},
                     {"wrote", result.wrote},
                     {"description", result.describe()}};
  if (result.value) {
    j["value"] = *result.value;
  }
}

std::expected<EncoderRegisterResult, std::string> SomanetDrive::readEncoderRegister(
    somanet::EncoderOrdinal encoder, uint8_t registerAddress, const OsCommandConfig& config) {
  return accessEncoderRegister(*this, encoder, false, registerAddress, 0, config);
}

std::expected<EncoderRegisterResult, std::string> SomanetDrive::writeEncoderRegister(
    somanet::EncoderOrdinal encoder, uint8_t registerAddress, uint8_t value,
    const OsCommandConfig& config) {
  return accessEncoderRegister(*this, encoder, true, registerAddress, value, config);
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
      return std::unexpected(
          std::format("open phase detection was not performed: {} (OS error {}){}", *general,
                      *response->errorCode, driveStateSuffix(*this, *response->errorCode)));
    }
    result.faultCode = response->errorCode;
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

std::string CommutationOffsetResult::describe() const {
  return std::format("commutation angle offset {} ({} method)", angleOffset,
                     somanet::toString(method));
}

void to_json(nlohmann::json& j, const CommutationOffsetResult& result) {
  j = nlohmann::json{{"angleOffset", result.angleOffset},
                     {"method", somanet::toString(result.method)},
                     {"description", result.describe()}};
}

std::expected<somanet::CommutationOffsetMethod, std::string> SomanetDrive::commutationOffsetMethod()
    const {
  // INTEGER8, not UNSIGNED8: the drive declares the method signed, and reading it as unsigned is
  // refused outright ("parameter 0x2009:03 holds a different type") rather than quietly misread.
  auto raw = device_.readValue<int8_t>(somanet::kCommutationOffsetDetection,
                                       somanet::kCommutationOffsetMethod);
  if (!raw) {
    return std::unexpected(raw.error());
  }
  if (*raw < 0 || *raw > static_cast<int8_t>(somanet::CommutationOffsetMethod::kStationary)) {
    return std::unexpected(
        std::format("commutation offset method {} is outside the defined range 0-2", *raw));
  }
  return static_cast<somanet::CommutationOffsetMethod>(*raw);
}

std::expected<CommutationOffsetResult, std::string> SomanetDrive::runCommutationOffsetMeasurement(
    somanet::CommutationOffsetMethod method, const OsCommandConfig& config) {
  static constexpr std::string_view kWhat = "commutation offset measurement";
  auto payload = runMotorMeasurement(*this, somanet::OsCommandId::kCommutationOffsetMeasurement,
                                     kWhat, CommandSpecificFault::kNone, config);
  if (!payload) {
    return std::unexpected(payload.error());
  }
  auto offset = decodeBigEndian<int16_t>(*payload, kWhat);
  if (!offset) {
    return std::unexpected(offset.error());
  }
  return CommutationOffsetResult{.angleOffset = *offset, .method = method};
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

std::string TorqueConstantResult::describe() const {
  return std::format("torque constant {} mNm/A_rms", milliNewtonMetresPerAmpere);
}

void to_json(nlohmann::json& j, const TorqueConstantResult& result) {
  j = nlohmann::json{{"milliNewtonMetresPerAmpere", result.milliNewtonMetresPerAmpere},
                     {"description", result.describe()}};
}

std::expected<TorqueConstantResult, std::string> SomanetDrive::runTorqueConstantMeasurement(
    const OsCommandConfig& config) {
  static constexpr std::string_view kWhat = "torque constant measurement";
  auto payload = runMotorMeasurement(*this, somanet::OsCommandId::kTorqueConstantMeasurement, kWhat,
                                     CommandSpecificFault::kNone, config);
  if (!payload) {
    return std::unexpected(payload.error());
  }
  // Signed, where its siblings are not: the drive's own field is an int32_t and this command's
  // arithmetic can reach below zero. See TorqueConstantResult.
  auto torqueConstant = decodeBigEndian<int32_t>(*payload, kWhat);
  if (!torqueConstant) {
    return std::unexpected(torqueConstant.error());
  }
  return TorqueConstantResult{.milliNewtonMetresPerAmpere = *torqueConstant};
}

std::string SkippedCyclesResult::describe() const {
  return std::format("{} skipped {} cycle{} since it started", somanet::toString(service),
                     skippedCycles, skippedCycles == 1 ? "" : "s");
}

void to_json(nlohmann::json& j, const SkippedCyclesResult& result) {
  j = nlohmann::json{{"service", somanet::toString(result.service)},
                     {"skippedCycles", result.skippedCycles},
                     {"description", result.describe()}};
}

// Byte layout of the skipped cycles counter request (command 13) — the ID and the service it asks.
constexpr size_t kFirmwareServiceByte = 1;

std::expected<SkippedCyclesResult, std::string> SomanetDrive::readSkippedCycles(
    somanet::FirmwareService service, const OsCommandConfig& config) {
  const std::string what =
      std::format("reading the {} skipped cycles counter", somanet::toString(service));

  std::vector<uint8_t> command(kOsCommandSize, 0);
  command[0] = static_cast<uint8_t>(somanet::OsCommandId::kSkippedCyclesCounter);
  command[kFirmwareServiceByte] = static_cast<uint8_t>(service);

  auto response = runOsCommand(command, config);
  if (!response) {
    return std::unexpected(response.error());
  }

  if (response->failed()) {
    if (!response->errorCode) {
      return std::unexpected(std::format("{} failed and the drive reported no error code", what));
    }
    const uint8_t code = *response->errorCode;
    // The command defines no command-specific codes, so anything here is general. A timeout is the
    // one worth expanding: it is what a drive answers when *no* service recognised the request,
    // which for this command means the addressed one is not running rather than that the drive is
    // slow.
    if (auto general = osCommandErrorName(code)) {
      if (code == static_cast<uint8_t>(OsCommandError::kTimeout)) {
        return std::unexpected(std::format(
            "{} was not answered by any firmware service, so this drive has no {} service running "
            "(OS error {})",
            what, somanet::toString(service), code));
      }
      return std::unexpected(
          std::format("{} was not performed: {} (OS error {})", what, *general, code));
    }
    return std::unexpected(
        std::format("{} failed with OS error {} (command-specific)", what, code));
  }

  auto skipped = decodeBigEndian<uint32_t>(response->data, what);
  if (!skipped) {
    return std::unexpected(skipped.error());
  }
  return SkippedCyclesResult{.service = service, .skippedCycles = *skipped};
}

// Byte layout of the ignore-BiSS-status-bits request (command 14). Its byte 1 packs the encoder
// **above** a trigger bit, where command 0 puts the same ordinal in the low bits and command 1 puts
// a mode above it — three commands, three packings of one byte, so none of them shares a constant.
constexpr size_t kBissIgnoreByte = 1;
constexpr unsigned kBissIgnoreEncoderShift = 1;
constexpr uint8_t kBissIgnoreTrigger = 0x01;

std::expected<void, std::string> SomanetDrive::setIgnoreBissStatusBits(
    somanet::EncoderOrdinal encoder, bool ignore, const OsCommandConfig& config) {
  const std::string what =
      std::format("{} the status bits of {}", ignore ? "ignoring" : "stopping ignoring",
                  somanet::toString(encoder));

  std::vector<uint8_t> command(kOsCommandSize, 0);
  command[0] = static_cast<uint8_t>(somanet::OsCommandId::kIgnoreBissStatusBits);
  command[kBissIgnoreByte] =
      static_cast<uint8_t>((static_cast<uint8_t>(encoder) << kBissIgnoreEncoderShift) |
                           (ignore ? kBissIgnoreTrigger : 0));

  auto response = runOsCommand(command, config);
  if (!response) {
    return std::unexpected(response.error());
  }

  if (response->failed()) {
    if (!response->errorCode) {
      return std::unexpected(std::format("{} failed and the drive reported no error code", what));
    }
    const uint8_t code = *response->errorCode;
    // No command-specific codes, so anything here is general — and the timeout is the one that
    // means something specific. Only the BiSS service instance owning the addressed encoder
    // answers this command, so nothing answering at all *is* the precondition failing.
    if (auto general = osCommandErrorName(code)) {
      if (code == static_cast<uint8_t>(OsCommandError::kTimeout)) {
        return std::unexpected(
            std::format("{} was not answered by any BiSS service, so {} is either not configured "
                        "or not a BiSS encoder (OS error {})",
                        what, somanet::toString(encoder), code));
      }
      return std::unexpected(
          std::format("{} was not performed: {} (OS error {})", what, *general, code));
    }
    return std::unexpected(
        std::format("{} failed with OS error {} (command-specific)", what, code));
  }
  // Status 0: completed with no reply, which is all this command ever answers on success.
  return {};
}

// Byte layout of the system identification request (command 15): a parameter index, then its value
// big-endian across the next four bytes — the one command here that carries a 32-bit argument.
constexpr size_t kSystemIdParameterByte = 1;
constexpr size_t kSystemIdValueMsbByte = 2;

std::expected<void, std::string> SomanetDrive::setSystemIdentificationParameter(
    somanet::SystemIdentificationParameter parameter, uint32_t value,
    const OsCommandConfig& config) {
  const std::string what = std::format("setting the system identification {} to {}",
                                       somanet::toString(parameter), value);

  std::vector<uint8_t> command(kOsCommandSize, 0);
  command[0] = static_cast<uint8_t>(somanet::OsCommandId::kSystemIdentification);
  command[kSystemIdParameterByte] = static_cast<uint8_t>(parameter);
  command[kSystemIdValueMsbByte] = static_cast<uint8_t>(value >> 24);
  command[kSystemIdValueMsbByte + 1] = static_cast<uint8_t>(value >> 16);
  command[kSystemIdValueMsbByte + 2] = static_cast<uint8_t>(value >> 8);
  command[kSystemIdValueMsbByte + 3] = static_cast<uint8_t>(value);

  auto response = runOsCommand(command, config);
  if (!response) {
    return std::unexpected(response.error());
  }

  if (response->failed()) {
    if (!response->errorCode) {
      // Status 2 is this command's own failure, and it has exactly one cause: the motion control
      // service did not recognise the parameter index. Unreachable through the typed enum, so it
      // means the firmware and this build disagree about the numbering.
      return std::unexpected(
          std::format("{} failed: the drive does not recognise parameter index {}", what,
                      static_cast<int>(parameter)));
    }
    const uint8_t code = *response->errorCode;
    if (auto general = osCommandErrorName(code)) {
      return std::unexpected(
          std::format("{} was not performed: {} (OS error {})", what, *general, code));
    }
    return std::unexpected(
        std::format("{} failed with OS error {} (command-specific)", what, code));
  }
  return {};
}

std::expected<void, std::string> SomanetDrive::setOperationMode(somanet::OperationMode mode) {
  return setOperationModeValue(static_cast<int8_t>(mode));
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
