#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <string>

#include "node/device_manager.h"
#include "node/somanet_drive.h"

namespace mm::node {

/// @file
/// @brief SOMANET drive control by bus position — resolve, bind the vendor view, run one operation.
///
/// The SOMANET sibling of @c cia402_control.h, and the same three lines each: @c withDevice to hold
/// the bus lock and resolve the position, @c createSomanetDrive to validate the device really is a
/// SOMANET drive, then one call on the view. The domain logic stays on @c SomanetDrive; these are
/// only the lifetime-safe way in for code that has a bus position rather than a borrowed device.
///
/// **These are not procedures**, for the reason spelled out in @c cia402_control.h: they are short
/// interactive control, and they are what procedures are *built from*. A diagnostics procedure
/// releases the brake as one of its steps by calling @c SomanetDrive::releaseBrake on the device it
/// has already borrowed — never by starting a brake "procedure", which would deadlock against the
/// device token it already holds.

/// @brief Reads a drive's brake configuration and current state (0x2004).
///
/// @param deviceManager  Owner of the device set; lends locked access for the call.
/// @param slavePosition  1-based bus position of the target device.
/// @return The brake state, or an error if the device is unknown, is not a SOMANET drive, or a read
///         fails.
std::expected<BrakeState, std::string> brakeState(DeviceManager& deviceManager,
                                                  uint16_t slavePosition);

/// @brief Releases (disengages) a drive's brake and waits for it to have done so.
///
/// Thin forward to @c SomanetDrive::releaseBrake — read its documentation before calling this,
/// because two of its properties are surprising. The release only actually happens while the drive
/// is in OP ENABLED (elsewhere the write merely energises phase D), and on a pin brake the release
/// procedure **moves the shaft**. A brake that is not firmware-controlled is left alone, which the
/// returned state reports rather than treating as a failure.
///
/// Blocks for the drive's pull time (0x2004:03) plus @p settle.
///
/// @param deviceManager  Owner of the device set; lends locked access for the call.
/// @param slavePosition  1-based bus position of the target device.
/// @param settle         Extra wait on top of the drive's pull time.
/// @return The brake state read back afterwards, or why the attempt failed.
std::expected<BrakeState, std::string> releaseBrake(DeviceManager& deviceManager,
                                                    uint16_t slavePosition,
                                                    std::chrono::milliseconds settle);

/// @brief Engages a drive's brake and waits @p settle for it to bite.
///
/// @param deviceManager  Owner of the device set; lends locked access for the call.
/// @param slavePosition  1-based bus position of the target device.
/// @param settle         How long to wait after commanding the brake before returning.
/// @return The brake state read back afterwards, or why the attempt failed.
std::expected<BrakeState, std::string> engageBrake(DeviceManager& deviceManager,
                                                   uint16_t slavePosition,
                                                   std::chrono::milliseconds settle);

}  // namespace mm::node
