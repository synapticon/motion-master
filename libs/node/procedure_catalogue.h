#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "node/device.h"
#include "node/device_manager.h"
#include "node/procedure.h"
#include "node/procedure_manager.h"

namespace mm::node {

/// @file
/// @brief The registry of every procedure the server knows, and the four operations a client
///        performs against it.
///
/// This is the layer that makes one generic route triple serve every procedure: the HTTP handler
/// forwards a name and a JSON body, and the catalogue resolves both. Adding a procedure is adding a
/// row here — a descriptor, a predicate saying which devices have it, and a factory turning a
/// request into a body — with no route, no handler and no @c ProcedureManager change.
///
/// It sits *outward* of @c DeviceManager and @c ProcedureManager and may name profile types freely;
/// both of those stay profile-ignorant. It is also where the two error kinds those classes cannot
/// judge come from: whether a procedure exists at all, and whether a request for it validates.

/// @brief One procedure, as the catalogue knows it: what it is, who has it, and how to run it.
struct ProcedureCatalogueEntry {
  ProcedureDescriptor descriptor;  ///< What a client is told about it.

  /// @brief Whether @p device supports this procedure.
  ///
  /// Normally a profile check — an OS command procedure asks whether a @c SomanetDrive binds. It is
  /// what keeps a vendor's procedures off a third-party device's list instead of offering controls
  /// that could only ever fail, and it is why the list is per device rather than global.
  std::function<bool(Device& device)> applies;

  /// @brief Turns a client's request into the body that will run.
  ///
  /// Where a procedure's parameter validation lives, which is why it returns a plain string error:
  /// the caller reports it verbatim as a bad request. A procedure taking no parameters ignores the
  /// argument (an absent request body arrives as an empty object, so it need not be sent at all).
  ///
  /// The variant is which of the two body shapes the procedure needs: almost all take a borrowed
  /// @c Device&, while one that must change AL state takes the manager and borrows per step (see
  /// overload, so the choice costs an entry nothing but naming the right signature.
  std::function<std::expected<ProcedureBody, std::string>(const nlohmann::json& request)> makeBody;
};

/// @brief Every procedure the server knows, in the order a client should present them.
///
/// A stable reference to a table built once on first use; entries are never added or removed at
/// runtime, so a caller may hold the reference.
const std::vector<ProcedureCatalogueEntry>& procedureCatalogue();

/// @brief One procedure paired with how its last run on a device went — an entry of
///        @c GET /api/devices/:pos/procedures.
///
/// The pairing is what lets a per-device page render in a single request instead of one per
/// procedure, and it is why the snapshot is never absent here: a procedure that has never run
/// reports an @c idleSnapshot, so every entry has the same shape.
struct ProcedureListing {
  ProcedureDescriptor descriptor;
  ProcedureSnapshot snapshot;
};
void to_json(nlohmann::json& j, const ProcedureListing& listing);

/// @brief Every procedure @p devicePosition supports, each with its current or last-run snapshot.
///
/// @return The listings (empty if the device supports none), or @c kUnknownDevice.
std::expected<std::vector<ProcedureListing>, ProcedureError> listProcedures(
    DeviceManager& deviceManager, ProcedureManager& procedureManager, uint16_t devicePosition);

/// @brief The current or last-run state of one procedure on one device.
///
/// Never-run is not an error: it reports the descriptor's all-idle snapshot, so a client polls one
/// shape whether or not the procedure has ever been run. @c kUnknownProcedure is reserved for a
/// name that does not exist or that this device does not support — a genuine addressing mistake.
std::expected<ProcedureSnapshot, ProcedureError> procedureSnapshot(
    DeviceManager& deviceManager, ProcedureManager& procedureManager, uint16_t devicePosition,
    std::string_view name);

/// @brief Starts @p name on @p devicePosition with @p request as its parameters.
///
/// @return The initial snapshot (status running, @c runCount already bumped), or why it could not
///         start — any of the four @c ProcedureError kinds.
std::expected<ProcedureSnapshot, ProcedureError> startProcedure(DeviceManager& deviceManager,
                                                                ProcedureManager& procedureManager,
                                                                uint16_t devicePosition,
                                                                std::string_view name,
                                                                const nlohmann::json& request);

/// @brief Asks the running @p name on @p devicePosition to stop.
///
/// Cancels the *run*, not the record: the retained snapshot stays behind reporting how far it got.
///
/// @return Void if a run was in flight and has been asked to stop. @c kUnknownProcedure covers both
///         an unrecognised name and a recognised one with nothing running — from a client's side
///         both mean "there is no run here to cancel".
std::expected<void, ProcedureError> cancelProcedure(DeviceManager& deviceManager,
                                                    ProcedureManager& procedureManager,
                                                    uint16_t devicePosition, std::string_view name);

}  // namespace mm::node
