#pragma once

#include <cstdint>
#include <expected>
#include <string>

#include "node/device_manager.h"
#include "node/profile_device.h"

namespace mm::node {

/// @file
/// @brief Generic CANopen device-profile (CiA301) operations by bus position.
///
/// The @c ProfileDevice counterpart of @c cia402_control.h, and the same shape: free functions over
/// @c DeviceManager& that borrow the device under the bus lock via @c withDevice, bind a validated
/// view, and delegate one operation to it. They live here rather than as @c DeviceManager methods
/// so that owning the device set stays separate from knowing any profile's vocabulary — which is
/// what lets a new profile arrive as "a view plus a control header" without touching
/// @c DeviceManager at all.
///
/// Both operations here are multi-second command-and-wait walks, so the shared bus lock is held for
/// the whole call. That blocks only the exclusive rebuilders (@c scan / @c reset) — never the RT
/// loop or the WebSocket, since each poll takes the driver's control-plane lock for one transaction
/// only. Both require the device's mailbox to be active (PRE-OP/SAFE-OP/OP).
///
/// Both are also procedures by the definition @c ProcedureManager will use — multi-second,
/// command-and-wait, worth a retained result — and are expected to gain a procedure body later.
/// That body takes the *view* (@c runXxx(ProfileDevice&, ProgressReporter&, std::stop_token)), so
/// it is a rewrite rather than a move, and these stay as the synchronous entry point.

/// @brief Commands a parameter store (0x1010) and waits for the device to confirm completion.
///
/// Writes the ASCII "save" signature and polls 0x1010:01 until it reads back 1, retrying a poll
/// that does not yet confirm — a value mismatch or a transient mailbox read error while the device
/// writes to flash, handled alike — within @p config's budget.
///
/// @param deviceManager  Owner of the device set; lends locked access for the call.
/// @param slavePosition  1-based bus position of the target device.
/// @param config         Retry/timing configuration (see @c StoreParametersConfig).
/// @return Void once the store is confirmed, or an error string if the device is unknown, has no
///         generic device area, the command write fails, or it is not confirmed within the budget.
std::expected<void, std::string> runStoreParameters(DeviceManager& deviceManager,
                                                    uint16_t slavePosition,
                                                    const StoreParametersConfig& config = {});

/// @brief Commands a restore of default parameters (0x1011) and waits for the device to confirm.
///
/// The restore counterpart of @c runStoreParameters: writes the ASCII "load" signature to the
/// sub-entry @p group selects, then polls it until it reads back 1.
///
/// **Destructive:** overwrites the selected group's live (volatile) parameter values with the
/// device's defaults.
///
/// @param deviceManager  Owner of the device set; lends locked access for the call.
/// @param slavePosition  1-based bus position of the target device.
/// @param group          Which group of defaults to restore.
/// @param config         Retry/timing configuration (see @c RestoreDefaultParametersConfig).
/// @return Void once the restore is confirmed, or an error string if the device is unknown, has no
///         generic device area, the command write fails, or it is not confirmed within the budget.
std::expected<void, std::string> runRestoreDefaultParameters(
    DeviceManager& deviceManager, uint16_t slavePosition, RestoreGroup group,
    const RestoreDefaultParametersConfig& config = {});

}  // namespace mm::node
