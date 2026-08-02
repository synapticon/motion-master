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

/// @brief Requests a CiA402 operation mode (0x6060), then reads back the resulting snapshot.
///
/// @param deviceManager  Owner of the device set; lends locked access for the call.
/// @param slavePosition  1-based bus position of the target device.
/// @param mode           Operation mode to request.
/// @return The post-write snapshot, or an error string if the device is unknown, not a CiA402
///         drive, or the write/read-back fails.
std::expected<Cia402Status, std::string> setCia402OperationMode(DeviceManager& deviceManager,
                                                                uint16_t slavePosition,
                                                                cia402::OperationMode mode);

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
