#include "node/somanet_drive.h"

#include <spdlog/spdlog.h>

#include <format>
#include <iterator>
#include <string>
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
