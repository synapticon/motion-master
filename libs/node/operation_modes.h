#pragma once

#include <cstdint>
#include <expected>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <vector>

#include "node/device.h"
#include "node/device_manager.h"

namespace mm::node {

/// @file
/// @brief What operation modes a drive has, and which of them it says it supports.
///
/// Sits outward of @c Device and names both profiles, the way @c procedure_catalogue does: the
/// standard half is CiA402 vocabulary read against the drive's own capability field, the
/// manufacturer half is the vendor's, and joining them is neither one's business. Nothing in
/// @c cia402.h or @c somanet_drive.h knows this file exists.
///
/// **Why an endpoint at all**: 0x6061 answers with a number, and a negative one names a mode no
/// standard table can decode — a drive sitting in -2 reads as "Unknown" to anything that only knows
/// CiA402. This is the table that turns the number into a mode.

/// @brief Where a mode is defined, which is also how much can be said about it.
enum class OperationModeKind : uint8_t {
  kStandard,      ///< Defined by CiA402; its support is advertised by a bit of 0x6502.
  kManufacturer,  ///< Defined by the vendor; 0x6502 has no bit this code can read for it.
};

/// @brief One operation mode of one device.
struct OperationModeInfo {
  int value = 0;      ///< The 0x6060 / 0x6061 value.
  std::string name;   ///< PascalCase identifier, as @c toString spells it.
  std::string label;  ///< The profile's or the vendor's own wording.
  OperationModeKind kind{OperationModeKind::kStandard};

  /// The 0x6502 bit that advertises this mode, when one does. Absent for every manufacturer mode
  /// and for @c NoMode, which is always legal and so has no bit.
  std::optional<int> bit;

  /// The profile's short form ("csp", "hm", …); empty for manufacturer modes and @c NoMode.
  std::string abbreviation;

  /// Whether the drive says it supports this mode. **Absent, rather than false, for every
  /// manufacturer mode**: 0x6502 reserves bits 16-31 for the vendor without defining them, so a
  /// drive that implements a vendor mode has no way to say so that this code could read. Absent
  /// means "the drive does not answer that question", which is not the same as no.
  std::optional<bool> supported;

  /// Whether the firmware marks the mode deprecated. Only a manufacturer mode carries this.
  bool deprecated = false;
};

/// @brief Every operation mode of one device, with what the drive says about each.
struct OperationModes {
  uint32_t supportedDriveModes = 0;  ///< 0x6502 verbatim, for a client that wants the raw field.

  /// The bits of 0x6502 at or above @c cia402::kFirstManufacturerDriveModeBit that are set.
  ///
  /// Reported as bit numbers and nothing else, deliberately: the profile leaves their meaning
  /// entirely to the vendor, and SOMANET publishes none — a SOMANET drive sets bits 16 and 17 and
  /// neither its EDS, its ESI nor its firmware source says what they advertise. Naming them would
  /// be invention; dropping them would hide that the drive claims something.
  std::vector<int> manufacturerBits;

  /// Every mode, ascending by value — the manufacturer modes (negative) first, then the standard
  /// ones. A client can render the list as it comes.
  std::vector<OperationModeInfo> modes;
};
void to_json(nlohmann::json& j, const OperationModes& modes);

/// @brief Builds the operation-mode table for @p device.
///
/// Reads 0x6502 through a @c Cia402Drive view — the one bus transaction here, and a cached one —
/// and marks each standard mode from its bit. The manufacturer half is appended only for a
/// Synapticon device, from @c somanet::kOperationModes, since a vendor's modes mean nothing on
/// another vendor's drive.
///
/// @param device Device to describe, borrowed for the call.
/// @return The table, or why 0x6502 could not be read.
std::expected<OperationModes, std::string> deviceOperationModes(Device& device);

/// @brief Borrow-and-delegate wrapper over @c deviceOperationModes, for a caller holding a bus
///        position rather than a device (the HTTP layer).
std::expected<OperationModes, std::string> operationModes(DeviceManager& deviceManager,
                                                          uint16_t slavePosition);

}  // namespace mm::node
