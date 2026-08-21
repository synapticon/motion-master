#pragma once

#include <expected>
#include <nlohmann/json_fwd.hpp>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "node/device.h"
#include "node/procedure.h"
#include "node/profile_device.h"

namespace mm::node {

/// @file
/// @brief Generic CANopen (CiA301) procedure bodies — the non-volatile storage commands every CoE
///        device carries, whatever profile it implements.
///
/// The @c ProfileDevice counterpart of @c somanet_procedures.h, and the reason the catalogue is not
/// a vendor's list: these apply to any device with a CoE mailbox, so a third-party slave offers
/// them too.
///
/// Both are command-and-wait walks — write a signature, then poll the same sub-entry until the
/// device reports the command complete — which is what makes them procedures rather than requests.
/// A store takes about a second while the device writes to flash, and a poll during that window can
/// fail outright; each body runs on its own thread, reports one step, and retains its result.

/// @brief Procedure name for storing parameters, as it appears in its URL and its snapshot key.
inline constexpr std::string_view kStoreParametersProcedure = "store-parameters";

/// @brief The single step storing parameters reports against.
inline constexpr std::string_view kStoreParametersStep = "store";

/// @brief Store parameters' step template — one step, idle.
std::vector<ProgressStep> storeParametersSteps();

/// @brief Runs a parameter store (0x1010) as a procedure body.
///
/// Writes the ASCII "save" signature to 0x1010:01, waits for the device to begin, then polls until
/// it reads back 1 — the CiA301 "save completed" value. A poll that does not yet confirm is retried
/// within the built-in budget, because a device busy writing to flash may refuse the read outright.
///
/// **Cancellation abandons the wait, not the store.** By the time a run can be cancelled the
/// signature is on the wire and the device is already persisting; stopping only means the master
/// stops waiting for the confirmation, which the step says rather than implying the store was
/// undone.
///
/// Needs an active mailbox (PRE-OP or above) and nothing else — no operation mode, no enable, and
/// the shaft does not move.
///
/// @param device   Device to run against, borrowed by the manager for this call.
/// @param reporter Where step progress is recorded.
/// @param stop     Cancellation token; checked between confirmation polls.
/// @param config   Retry/timing. **Not** part of the procedure's client surface — the catalogue
///                 always passes the defaults, since how long a device takes to write flash is a
///                 property of the device rather than a caller's choice. It is a parameter so that
///                 a test can run the confirmation walk without real waiting, exactly as
///                 @c ProfileDevice::runStoreParameters takes one. @c config.stop is ignored;
///                 @p stop is the cancellation that applies.
/// @return Void once the device confirmed the store, otherwise why it did not.
std::expected<void, std::string> runStoreParametersProcedure(Device& device,
                                                             ProgressReporter& reporter,
                                                             std::stop_token stop,
                                                             StoreParametersConfig config = {});

/// @brief Procedure name for restoring default parameters, as it appears in its URL and its
///        snapshot key.
inline constexpr std::string_view kRestoreDefaultParametersProcedure = "restore-default-parameters";

/// @brief The single step restoring default parameters reports against.
inline constexpr std::string_view kRestoreDefaultParametersStep = "restore";

/// @brief What one restore was asked to do.
struct RestoreDefaultParametersRequest {
  /// Which group of defaults to restore. Defaults to @c kAll, which is the whole point of the
  /// command for most callers; the narrower groups exist for a device where only one area should
  /// go back.
  RestoreGroup group{RestoreGroup::kAll};
};

/// @brief Parses and validates a client's restore request body.
///
/// Accepts `{"group": "all"}` — or `communication`, `application`, `manufacturer`. The group is
/// optional and defaults as @c RestoreDefaultParametersRequest declares.
///
/// @param body  Parsed request JSON.
/// @return The validated request, or a message naming what is wrong with it.
std::expected<RestoreDefaultParametersRequest, std::string> parseRestoreDefaultParametersRequest(
    const nlohmann::json& body);

/// @brief What restoring default parameters accepts, as its descriptor advertises it.
std::vector<ProcedureParameter> restoreDefaultParametersParameters();

/// @brief Restore default parameters' step template — one step, idle.
std::vector<ProgressStep> restoreDefaultParametersSteps();

/// @brief Runs a restore of default parameters (0x1011) as a procedure body.
///
/// The counterpart of @c runStoreParametersProcedure in shape — write the ASCII "load" signature to
/// the sub-entry the group selects, wait, poll until it reads back 1 — and in cancellation, which
/// likewise abandons only the wait.
///
/// **Destructive:** it overwrites the selected group's live values with the device's defaults.
/// Anything unsaved is gone, and anything saved comes back on the next power cycle unless a store
/// follows — some devices apply the restored values only after a reset.
///
/// @param device   Device to run against, borrowed by the manager for this call.
/// @param reporter Where step progress is recorded.
/// @param stop     Cancellation token; checked between confirmation polls.
/// @param request  Which group of defaults to restore.
/// @param config   Retry/timing, as in @c runStoreParametersProcedure — a testing seam, not a
///                 client surface.
/// @return Void once the device confirmed the restore, otherwise why it did not.
std::expected<void, std::string> runRestoreDefaultParametersProcedure(
    Device& device, ProgressReporter& reporter, std::stop_token stop,
    const RestoreDefaultParametersRequest& request, RestoreDefaultParametersConfig config = {});

}  // namespace mm::node
