#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <string>

#include "node/cia402.h"
#include "node/cia402_drive.h"
#include "node/device_manager.h"

namespace mm::node {

/// @file
/// @brief CiA402 drive control by bus position — resolve, bind a view, run one operation.
///
/// These are free functions over @c DeviceManager& rather than methods on it, because owning the
/// device set and knowing the CiA402 vocabulary are different jobs: @c DeviceManager lends locked
/// access through @c withDevice and never names a profile type, and the profile layer builds on
/// top of it. Each function is the same three lines — @c withDevice to hold the bus lock and
/// resolve, @c createCia402Drive to validate the device really is a drive, then one call on the
/// view — so the domain logic stays on @c Cia402Drive and these are only the lifetime-safe way in.
///
/// The bus lock is held (shared) for each call's whole duration, including @c runCia402Command's
/// multi-second enable walk. That blocks only the exclusive rebuilders (@c scan / @c reset), never
/// the RT loop or the WebSocket: each individual read and write takes the driver's control-plane
/// lock only for its own transaction.
///
/// **These are deliberately not procedures.** They are short, interactive device control, and —
/// decisively — they are what procedures are *built from*: offset detection has to enable the
/// drive as one of its steps, so if enabling were itself a registered procedure the parent would
/// deadlock trying to acquire a device token it already holds. Procedures compose out of plain
/// calls like these. (@c profile_procedures.h holds the operations that genuinely are procedures.)
///
/// All of them require the device to be exchanging for the state machine to actually advance.

/// @brief Reads a device's CiA402 control snapshot (state, status/control words, active mode).
///
/// @param deviceManager  Owner of the device set; lends locked access for the call.
/// @param slavePosition  1-based bus position of the target device.
/// @return The snapshot, or an error string if the device is unknown, not a CiA402 drive, or a
///         read fails.
std::expected<Cia402Status, std::string> cia402Status(DeviceManager& deviceManager,
                                                      uint16_t slavePosition);

/// @brief Requests an operation mode (0x6060) by its raw value, then reads back the snapshot.
///
/// **Raw rather than @c cia402::OperationMode, because that enum cannot name every mode a drive
/// has**: the negative half of 0x6060 belongs to the vendor, so a SOMANET's diagnostics (-2) or
/// system identification (-4) would be unrepresentable and therefore unrequestable.
///
/// **The write is confirmed, not assumed.** A drive is free to ignore a mode request, and this one
/// does so routinely: SOMANET firmware refuses a change to a non-"dynamic" mode while the drive is
/// in Operation Enabled, and refuses outright any mode its @c opmode_update does not list —
/// deprecated system identification (-4), for instance. Every one of those refusals is a successful
/// SDO write followed by nothing happening, so this polls 0x6061 up to @p timeout and reports a
/// mode the drive declined rather than returning a snapshot that quietly contradicts the request.
/// The drive's own reason is usually in 0x603F, and is included when it is.
///
/// @c deviceOperationModes is what answers "does it have this mode" beforehand; this answers "did
/// it take it".
///
/// Mode 0 is exempt from the confirmation: it means *no* mode requested, so there is nothing for
/// the drive to arrive at and a display object that keeps showing the previous mode is not a
/// refusal.
///
/// @param deviceManager  Owner of the device set; lends locked access for the call.
/// @param slavePosition  1-based bus position of the target device.
/// @param mode           The 0x6060 value to request.
/// @param timeout        How long to wait for 0x6061 to reflect @p mode.
/// @return The snapshot once the drive adopted the mode, or an error string if the device is
///         unknown, not a CiA402 drive, the write fails, or the drive declined the mode.
std::expected<Cia402Status, std::string> setCia402OperationMode(
    DeviceManager& deviceManager, uint16_t slavePosition, int8_t mode,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(200));

/// @brief Runs a CiA402 state-machine command (enable / disable / quick stop / fault reset), then
///        reads back the resulting snapshot.
///
/// @c kEnable walks every intermediate transition to OperationEnabled (up to @p timeout, clearing
/// a fault first if needed); the others issue a single controlword edge. The drive must be
/// exchanging for @c kEnable to make progress.
///
/// @param deviceManager  Owner of the device set; lends locked access for the call.
/// @param slavePosition  1-based bus position of the target device.
/// @param command        The action to perform.
/// @param timeout        Maximum time to wait for @c kEnable to reach OperationEnabled.
/// @return The post-command snapshot, or an error string if the device is unknown, not a CiA402
///         drive, or the command fails (including an enable timeout).
std::expected<Cia402Status, std::string> runCia402Command(
    DeviceManager& deviceManager, uint16_t slavePosition, Cia402Command command,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));

/// @brief Brings a drive to @p target, walking whatever transitions that takes, then reads back
///        the resulting snapshot.
///
/// The state-oriented counterpart of @c runCia402Command: a caller names a destination rather than
/// the edges to get there, and the master issues them one at a time, re-reading the drive between
/// each (@c Cia402Drive::transitionToState). Multi-hop paths — Switch On Disabled to Operation
/// Enabled, Quick Stop Active back down to Ready To Switch On — are ordinary, as is starting from
/// Fault, which begins with a reset.
///
/// **The quick-stop override is on here, and that is the difference from @c runCia402Command's
/// @c kEnable.** Asking for Operation Enabled while the drive sits in Quick Stop Active is an
/// explicit instruction to leave that stop, so transition 16 is used; the same request through
/// @c kEnable — which procedures make to prepare a drive, not to override anybody — is refused.
///
/// The drive must be exchanging process data for any of this to progress: the statusword has to
/// update between polls, and the firmware refuses Operation Enabled outright unless master
/// communication is live.
///
/// @param deviceManager  Owner of the device set; lends locked access for the call.
/// @param slavePosition  1-based bus position of the target device.
/// @param target         Where to bring the drive. Must be @c cia402::isCommandableState.
/// @param timeout        Maximum time to spend walking.
/// @return The snapshot after arriving, or an error string if the device is unknown, not a CiA402
///         drive, the target is not commandable, a fault will not clear, or the walk times out.
std::expected<Cia402Status, std::string> transitionToCia402State(
    DeviceManager& deviceManager, uint16_t slavePosition, cia402::State target,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));

/// @brief Writes the one CiA402 cyclic setpoint that matches the active operation mode.
///
/// A drive follows a single setpoint at a time — position in PP/CSP, velocity in PV/CSV, torque in
/// PT/CST — so the caller names which @p kind it is setting (from the selected mode) rather than
/// writing all three. Routes through the live PDO image when the object is mapped and the device
/// is exchanging, else an SDO download (see @c Device::writeValue). @p setpoint is the raw value;
/// for @c kTorque it is per-mille of rated and is narrowed to INTEGER16.
///
/// @param deviceManager  Owner of the device set; lends locked access for the call.
/// @param slavePosition  1-based bus position of the target device.
/// @param kind           Which setpoint to write (position / velocity / torque).
/// @param setpoint       The setpoint value in the object's units.
/// @return Void on success, or an error string if the device is unknown, not a CiA402 drive, or
///         the write fails.
std::expected<void, std::string> setCia402Target(DeviceManager& deviceManager,
                                                 uint16_t slavePosition, Cia402TargetKind kind,
                                                 int32_t setpoint);

}  // namespace mm::node
