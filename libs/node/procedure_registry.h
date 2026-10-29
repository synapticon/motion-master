#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "node/controller_tuning.h"
#include "node/device.h"
#include "node/device_manager.h"
#include "node/plant_identification.h"
#include "node/procedure.h"
#include "node/procedure_manager.h"

namespace mm::node {

/// @file
/// @brief The registry of every procedure the server knows, and the four operations a client
///        performs against it.
///
/// This is the layer that makes one generic route triple serve every procedure: the HTTP handler
/// forwards a name and a JSON body, and the registry resolves both. Adding a procedure is adding a
/// row here — a descriptor, a predicate saying which devices have it, and a factory turning a
/// request into a body — with no route, no handler and no @c ProcedureManager change.
///
/// It sits *outward* of @c DeviceManager and @c ProcedureManager and may name profile types freely;
/// both of those stay profile-ignorant. It is also where the two error kinds those classes cannot
/// judge come from: whether a procedure exists at all, and whether a request for it validates.

/// @brief One procedure, as the registry knows it: what it is, who has it, and how to run it.
struct ProcedureRegistryEntry {
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

/// @brief One procedure paired with how its last run on a device went — an entry of
///        @c GET /api/devices/:pos/procedures.
///
/// The pairing is what lets a per-device page render in a single request instead of one per
/// procedure, and it is why the snapshot is never absent here: a procedure that never ran
/// reports an @c idleSnapshot, so every entry has the same shape.
struct ProcedureListing {
  ProcedureDescriptor descriptor;
  ProcedureSnapshot snapshot;
};
void to_json(nlohmann::json& j, const ProcedureListing& listing);

/// @brief What the registry needs from outside @c mm::node.
///
/// A procedure that calls the auto-tuning program reaches it through a callback here, because the
/// client for that program lives outside this library. The composition root builds the callbacks,
/// so this struct is where a new outside dependency of a procedure is declared, beside the others.
struct ProcedureRegistryConfig {
  /// Fits a plant model to a chirp measurement. Plant identification calls it. When no auto-tuning
  /// program runs, the composition root installs a function that says so.
  PlantModelFitFn fitPlantModel;
  /// Computes controller gains from a plant model. Controller auto-tuning calls it. When no
  /// auto-tuning program runs, the composition root installs a function that says so.
  ControllerTuningFn tuneController;
};

/// @brief Every procedure the server knows, and the four operations a client performs against it.
///
/// Built once, in the composition root, with the collaborators every procedure body shares and the
/// callbacks of @c ProcedureRegistryConfig. Entries are never added or removed after construction,
/// so a caller may hold a pointer into @c entries().
class ProcedureRegistry {
 public:
  /// @param deviceManager     Where a device is looked up. Must outlive the registry.
  /// @param procedureManager  Where a run is started, cancelled and polled. Must outlive the
  ///                          registry.
  /// @param config            The outside callbacks, copied in.
  ProcedureRegistry(DeviceManager& deviceManager, ProcedureManager& procedureManager,
                    ProcedureRegistryConfig config);

  /// @brief Every entry, in the order a client should present them.
  const std::vector<ProcedureRegistryEntry>& entries() const { return entries_; }

  /// @brief Every procedure @p devicePosition supports, each with its current or last-run
  ///        snapshot.
  ///
  /// @return The listings (empty if the device supports none), or @c kUnknownDevice.
  std::expected<std::vector<ProcedureListing>, ProcedureError> list(uint16_t devicePosition) const;

  /// @brief The current or last-run state of one procedure on one device.
  ///
  /// Never-run is not an error: it reports the descriptor's all-idle snapshot, so a client polls
  /// one shape whether or not the procedure has ever been run. @c kUnknownProcedure is reserved for
  /// a name that does not exist or that this device does not support — a genuine addressing
  /// mistake.
  std::expected<ProcedureSnapshot, ProcedureError> snapshot(uint16_t devicePosition,
                                                            std::string_view name) const;

  /// @brief Starts @p name on @p devicePosition with @p request as its parameters.
  ///
  /// @return The initial snapshot (status running, @c runCount already bumped), or why it could not
  ///         start — any of the four @c ProcedureError kinds.
  std::expected<ProcedureSnapshot, ProcedureError> start(uint16_t devicePosition,
                                                         std::string_view name,
                                                         const nlohmann::json& request) const;

  /// @brief Asks the running @p name on @p devicePosition to stop.
  ///
  /// Cancels the *run*, not the record: the retained snapshot stays behind reporting how far it
  /// got.
  ///
  /// @return Void if a run was in flight and got the stop request. @c kUnknownProcedure covers both
  ///         an unrecognised name and a recognised one with nothing running — from a client's side
  ///         both mean "there is no run here to cancel".
  std::expected<void, ProcedureError> cancel(uint16_t devicePosition, std::string_view name) const;

 private:
  /// The procedures @p devicePosition supports, as pointers into @c entries_.
  std::expected<std::vector<const ProcedureRegistryEntry*>, ProcedureError> applicableEntries(
      uint16_t devicePosition) const;

  /// One procedure by name, if the device has it. A name the registry does not hold and one this
  /// device does not support are the same answer to a client: nothing is addressable there.
  std::expected<const ProcedureRegistryEntry*, ProcedureError> applicableEntry(
      uint16_t devicePosition, std::string_view name) const;

  DeviceManager& deviceManager_;
  ProcedureManager& procedureManager_;
  std::vector<ProcedureRegistryEntry> entries_;
};

}  // namespace mm::node
