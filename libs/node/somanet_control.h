#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "node/device_manager.h"
#include "node/somanet_drive.h"

namespace mm::node {

/// @file
/// @brief SOMANET drive control by bus position — resolve, bind the vendor view, run one operation.
///
/// The SOMANET sibling of @c cia402_control.h, and the same three lines each:
/// @c DeviceManager::deviceAt to resolve the position and hold the device alive, @c
/// createSomanetDrive to validate the device really is a SOMANET drive, then one call on the view.
/// The domain logic stays on @c SomanetDrive; these are only the lifetime-safe way in for code that
/// has a bus position rather than a device.
///
/// **These are not procedures**, for the reason spelled out in @c cia402_control.h: they are short
/// interactive control, and they are what procedures are *built from*. A diagnostics procedure
/// releases the brake as one of its steps by calling @c SomanetDrive::releaseBrake on the device it
/// already borrowed — never by starting a brake "procedure", which would deadlock against the
/// device token it already holds.

/// @brief Reads a drive's brake configuration and current state (0x2004).
///
/// @param deviceManager  Owner of the device set; lends locked access for the call.
/// @param slavePosition  1-based bus position of the target device.
/// @return The brake state, or an error if the device is unknown, is not a SOMANET drive, or a read
///         fails.
std::expected<BrakeState, std::string> brakeState(DeviceManager& deviceManager,
                                                  uint16_t slavePosition);

/// @brief Releases (disengages) a drive's brake, then waits for the release to finish.
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

/// @brief Reads the list of files stored on a device (FoE read of "fs-getlist").
///
/// Thin forward to @c SomanetDrive::readFileList. Vendor-specific despite reading like a filesystem
/// primitive: the listing is a pseudo-file Synapticon firmware serves, not a standard service, so a
/// device that is not a SOMANET drive is refused rather than asked.
///
/// @param deviceManager  Owner of the device set; lends locked access for the call.
/// @param slavePosition  1-based bus position of the target device.
/// @return The files the device reported, or why the listing could not be read.
std::expected<std::vector<DeviceFile>, std::string> readFileList(DeviceManager& deviceManager,
                                                                 uint16_t slavePosition);

/// @brief Deletes one file from a device's flash.
///
/// Thin forward to @c SomanetDrive::removeFile. Vendor-specific for the same reason as
/// @c readFileList: the drive has no delete operation, only a pseudo-file that acts on the name it
/// carries. A file that was not there counts as removed.
///
/// @param deviceManager  Owner of the device set; lends locked access for the call.
/// @param slavePosition  1-based bus position of the target device.
/// @param filename       Name as the drive lists it, with no prefix.
/// @return Void once the file is gone, or why the removal failed.
std::expected<void, std::string> removeFile(DeviceManager& deviceManager, uint16_t slavePosition,
                                            const std::string& filename);

/// @brief Reads and parses a device's @c .hardware_description file.
///
/// Thin forward to @c SomanetDrive::readHardwareDescription — what the device says it is, and the
/// source of the descriptor that decides which firmware belongs on it.
///
/// @param deviceManager  Owner of the device set; lends locked access for the call.
/// @param slavePosition  1-based bus position of the target device.
/// @return The parsed description, or why it could not be read.
std::expected<HardwareDescription, std::string> readHardwareDescription(
    DeviceManager& deviceManager, uint16_t slavePosition);

/// @brief Reads and parses a device's @c .variant file.
///
/// Thin forward to @c SomanetDrive::readIntegroVariant. Only Integro drives carry one; an empty
/// optional means the device has none — which a Node or a Circulo correctly does, and which is
/// therefore not a failure. Read its documentation for why *any* unreadable variant is reported
/// that way rather than only a not-found one.
///
/// @param deviceManager  Owner of the device set; lends locked access for the call.
/// @param slavePosition  1-based bus position of the target device.
/// @return The parsed file, nothing if the device has none, or why the read failed.
std::expected<std::optional<IntegroVariant>, std::string> readIntegroVariant(
    DeviceManager& deviceManager, uint16_t slavePosition);

/// @brief Reads what a device is and decides whether a firmware package belongs on it.
///
/// Thin forward to @c SomanetDrive::checkFirmwarePackage. Two FoE reads; an incompatible package is
/// a verdict rather than an error, so an error here means the question could not be asked.
///
/// @param deviceManager    Owner of the device set; lends locked access for the call.
/// @param slavePosition    1-based bus position of the target device.
/// @param packageFilename  Bare package filename, with no directory part.
/// @return The verdict, or why no verdict could be reached.
std::expected<FirmwareCompatibility, std::string> checkFirmwarePackage(
    DeviceManager& deviceManager, uint16_t slavePosition, std::string_view packageFilename);

/// @brief Reads a drive's high resolution data recording back and decodes it.
///
/// Thin forward to @c SomanetDrive::readHrdRecording. Blocks for the transfer — up to five 8 KB FoE
/// reads plus the listing — which is why it is not the tail of the recording procedure: a recording
/// is worth reading more than once, and a run's snapshot is no place to keep ten thousand samples.
///
/// @param deviceManager  Owner of the device set; lends locked access for the call.
/// @param slavePosition  1-based bus position of the target device.
/// @param data           Which layout the files hold — the selection the recording was made with.
/// @return The decoded recording, or why it could not be read.
std::expected<HrdRecording, std::string> readHrdRecording(DeviceManager& deviceManager,
                                                          uint16_t slavePosition,
                                                          somanet::HrdData data);

}  // namespace mm::node
