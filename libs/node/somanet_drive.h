#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "node/cia402_drive.h"

namespace mm::node {

/// @brief Byte length of the OS command and response octet strings (0x1023:01 / 0x1023:03).
///        Manufacturer-specific — the firmware specification allows it to grow, so read it from
///        here rather than writing 8 at a call site.
inline constexpr size_t kOsCommandSize = 8;

/// @brief Terminal OS command status — byte 0 of the response (0x1023:03), which the firmware
///        mirrors from 0x1023:02. Only these four values end a command; 100-200 (executing, with
///        percentage) and 255 (executing) are transient and never surface to a caller.
enum class OsCommandStatus : uint8_t {
  kCompleted = 0,          ///< Completed, no error, no response data.
  kCompletedWithData = 1,  ///< Completed, no error, response data available.
  kFailed = 2,             ///< Completed with error, no response data.
  kFailedWithData = 3,     ///< Completed with error, OS error code and response data available.
};

/// @brief The general OS error codes — the ones any command can report, counting down from 254.
///        Command-specific codes count *up* from 0 and are named by the command that issued them,
///        not here (open phase detection's 0 is "open terminal A", phase resistance's 0 is
///        "current amplitude error", ...).
enum class OsCommandError : uint8_t {
  kNotAllowed = 251,   ///< Preconditions not met (wrong mode of operation, say).
  kAborted = 252,      ///< The abort the master requested via 0x1024 was carried out.
  kTimeout = 253,      ///< No downstream service acknowledged the command (or the abort) in 5 s.
  kUnsupported = 254,  ///< The command ID does not exist on this drive.
  kReserved = 255,     ///< Reserved for future expansion of the feature.
};

/// @brief Names a general OS error code, or @c std::nullopt when @p code is command-specific.
///
/// A caller that knows which command it issued should try its own error table first and fall back
/// to this one; a caller that does not can report the raw code alongside whatever this returns.
std::optional<std::string_view> osCommandErrorName(uint8_t code);

/// @brief A completed OS command's response — the decoded form of 0x1023:03.
///
/// @c status is the terminal status byte; @c data is the service response payload (bytes 2-7 when
/// the command succeeded with data, bytes 3-7 when it failed with data, empty otherwise — byte 1
/// is unused by the protocol); @c errorCode is the OS error code, present only for
/// @c kFailedWithData.
struct OsCommandResponse {
  OsCommandStatus status{OsCommandStatus::kCompleted};  ///< Terminal status (response byte 0).
  std::vector<uint8_t> data;                            ///< Service response payload, if any.
  std::optional<uint8_t> errorCode;                     ///< OS error code, if the drive sent one.

  /// @brief Whether the drive reported the command as failed (status 2 or 3). A failed command is
  ///        still a response — the drive rendered a verdict — so it arrives as a value, not an
  ///        error; consult @c errorCode for the reason.
  bool failed() const {
    return status == OsCommandStatus::kFailed || status == OsCommandStatus::kFailedWithData;
  }
};

/// @brief Timing and cancellation for @c SomanetDrive::runOsCommand.
///
/// @c timeout is a ceiling on the whole command, not a liveness check — the drive fails a command
/// no downstream service acknowledges within 5 s on its own — so size it for the command being
/// run (milliseconds for a register read, tens of seconds for a measurement). Hitting it, or a
/// stop request on @c stop, makes the master abort the running command; @c abortTimeout then
/// bounds how long the drive is given to report that abort (its own internal abort path is
/// bounded by 5 s).
struct OsCommandConfig {
  std::chrono::milliseconds timeout{1000};        ///< Ceiling on the whole command.
  std::chrono::milliseconds pollInterval{10};     ///< Delay between response polls.
  std::chrono::milliseconds abortTimeout{10000};  ///< Time allowed to confirm a forced abort.
  std::stop_token stop{};  ///< Requesting a stop aborts the command; default never stops.
};

/// @brief Borrowed view of a SOMANET drive — a CiA402 drive plus Synapticon-specific
///        object-dictionary access (encoder/motor configuration, custom OS commands, etc.).
///
/// SOMANET drives implement CiA402 in full, so this *is-a* @c Cia402Drive and inherits the whole
/// state machine and setpoint surface; it adds only the vendor-specific objects in the
/// manufacturer range. Like every profile view it is a thin, stateless borrow over a @c Device
/// (see @c ProfileDevice) — construct it for one operation and drop it.
///
/// Construct via @c createSomanetDrive, which checks the vendor ID before binding.
///
/// **Multi-cycle procedures do not live here.** Commutation-offset detection, auto-tuning, and
/// the like are command-and-wait procedures that run off-RT on a background thread, taking a
/// @c DeviceManager& and re-resolving their @c Device each step (a long-lived view would dangle
/// across a bus rescan). Whoever owns that procedure runs it; this view's job is the synchronous,
/// single-shot SOMANET object access that frames those procedures (reading/writing config,
/// checking preconditions).
class SomanetDrive : public Cia402Drive {
 public:
  /// @brief Binds an (unchecked) SOMANET view to @p device. Prefer @c createSomanetDrive.
  explicit SomanetDrive(Device& device) : Cia402Drive(device) {}

  /// @brief Issues an OS command (0x1023) and blocks until the drive reports it finished.
  ///
  /// The mechanism behind every typed SOMANET command: write the 8-byte request to 0x1023:01,
  /// poll 0x1023:03 until its status byte goes terminal, decode the response. The objects are
  /// CiA301, but everything about how they are driven is Synapticon firmware behaviour — the
  /// status mirrored into response byte 0, the 100-200 percentage band, the OS error code in
  /// byte 2, the eight-byte payloads — which is why this lives on the vendor view rather than on
  /// @c ProfileDevice beside the raw accessors it is built from.
  ///
  /// Two firmware rules (both stated in the OS command specification) shape the implementation and
  /// are easy to break by "tidying" it:
  /// - **Only reading 0x1023:03 re-arms 0x1023:01.** The status byte is mirrored in 0x1023:02, but
  ///   reading *that* does not make the command object writable again — the drive returns to its
  ///   idle state only once the response has been read — so a command is never polled through
  ///   0x1023:02, and 0x1023:03 is read even when its payload is not wanted.
  /// - **0x1024 must be 0 for a command to be accepted**, and a write to 0x1023:01 while it is not
  ///   is *silently ignored* rather than refused. That would make the next poll read the previous
  ///   command's terminal status and report it as this command's response, so the mode is written
  ///   to 0 before every command, and an abort (mode 3) is followed by a restore to 0 on every
  ///   path out.
  ///
  /// No pre-check is made for a command already in progress: the drive enforces that itself by
  /// aborting the write to 0x1023:01 (SDO abort 0x08000021, "local control"), and that error is
  /// forwarded verbatim. Progress reported by the drive (status 100-200) is logged at debug level
  /// as it changes and is not otherwise surfaced — this call reports only the final outcome.
  ///
  /// Blocks the calling thread for up to @c config.timeout (plus @c config.abortTimeout if the
  /// command has to be aborted). Control-plane only: it sleeps between polls, but each poll takes
  /// the driver's lock for one transaction, so it never blocks the RT loop. Requires the mailbox
  /// to be active (PRE-OP/SAFE-OP/OP).
  ///
  /// @param command  The 8-byte request: byte 0 is the OS command ID, bytes 1-7 its parameters.
  /// @param config   Timing and cancellation (see @c OsCommandConfig).
  /// @return The decoded response — including one the drive marked failed, which is a verdict and
  ///         not a transport error (check @c OsCommandResponse::failed). An error string if the
  ///         command was not run or produced no verdict: a malformed request, an SDO failure
  ///         (forwarded as-is), an unknown status byte, a timeout, or a cancellation.
  std::expected<OsCommandResponse, std::string> runOsCommand(const std::vector<uint8_t>& command,
                                                             const OsCommandConfig& config = {});

  // SOMANET-specific synchronous object-dictionary accessors are added here as their object
  // indices are confirmed (encoder type/resolution, motor pole pairs, commutation offset, ...).
  // Each is a thin typed read/write over device(); none holds state.
};

/// @brief Validates that @p device is a SOMANET drive, then binds a view to it.
///
/// Requires the vendor ID to be Synapticon's (@c kSynapticonVendorId) and the device to satisfy
/// the CiA402 check (@c createCia402Drive). Both are offline-safe — vendor ID is immutable
/// identity read at scan, the CiA402 check reads the parameter map — so no bus I/O is performed.
///
/// @param device  Device to view. The reference must outlive the returned view.
/// @return A @c SomanetDrive bound to @p device, or an error string if it is not a SOMANET drive.
std::expected<SomanetDrive, std::string> createSomanetDrive(Device& device);

}  // namespace mm::node
