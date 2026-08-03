#include "node/procedure_catalogue.h"

#include <algorithm>
#include <expected>
#include <format>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#include "node/somanet_procedures.h"
#include "node/synapticon.h"

namespace mm::node {

namespace {

// Every entry the server knows. Built once, never mutated: a caller may hold the reference.
std::vector<ProcedureCatalogueEntry> buildCatalogue() {
  std::vector<ProcedureCatalogueEntry> entries;

  ProcedureDescriptor osCommand;
  osCommand.name = std::string(kOsCommandProcedure);
  osCommand.title = "OS command";
  osCommand.description =
      "Issues one OS command (CANopen 0x1023 / 0x1024) with the request bytes passed through "
      "exactly as given, and reports the drive's terminal status, its reply payload and any OS "
      "error code. This is the direct route to the drive's whole OS command set: byte 0 is the "
      "command ID and bytes 1-7 are its parameters, so any command the firmware implements can be "
      "run here. Where a command also has a procedure of its own, that one names and validates its "
      "parameters and decodes its result for you; this one asks you for the bytes and hands back "
      "what the drive replied.";
  osCommand.caveats = {
      "The request bytes are not checked against any command's expected parameters — an unintended "
      "command ID or parameter is issued to the drive as written.",
      "Depending on the command issued, this can move the shaft.",
      "Some commands are refused unless the drive is already enabled in a suitable mode of "
      "operation; the drive reports those as OS error 251, \"command not allowed\".",
      "Size the timeout for the command being run. Reaching it aborts the command on the drive.",
  };
  // Unknowable here — the caller chooses the command, and some of them spin the motor — so this
  // reports the possibility rather than a false negative. requiresEnabled stays false because the
  // mechanism itself needs nothing; the commands that do are covered by the caveat above, and a
  // command with a procedure of its own sets the flag for what that command actually needs.
  osCommand.movesMotor = true;
  osCommand.steps = osCommandSteps();

  entries.push_back(ProcedureCatalogueEntry{
      .descriptor = std::move(osCommand),
      // Vendor ID, deliberately, and not createSomanetDrive: that also requires the device's object
      // dictionary to have been enumerated (its CiA402 check looks for controlword/statusword in
      // the parameter map), and enumeration is opportunistic — it happens when a device is first
      // seen at PRE-OP. Binding applicability to it would report a genuine drive as having *no*
      // procedures merely because its OD had not been read yet, which is a wrong answer rather than
      // a late one. The vendor ID comes from SII at scan time and is always known. Whether the
      // device is also a conformant CiA402 drive stays the body's business, where it fails with a
      // reason.
      .applies = [](Device& device) { return device.vendorId() == kSynapticonVendorId; },
      .makeBody = [](const nlohmann::json& request) -> std::expected<ProcedureBody, std::string> {
        auto spec = parseOsCommandRequest(request);
        if (!spec) {
          return std::unexpected(spec.error());
        }
        return [spec = *spec](Device& device, ProgressReporter& reporter, std::stop_token stop) {
          return runOsCommandProcedure(device, reporter, std::move(stop), spec);
        };
      },
  });

  ProcedureDescriptor openPhase;
  openPhase.name = std::string(kOpenPhaseDetectionProcedure);
  openPhase.title = "Open phase detection";
  openPhase.description =
      "Checks every motor terminal and FET leg for an open circuit, and names the offending one if "
      "it finds a fault. Worth running first when commissioning a motor: the measurements that "
      "follow all assume the three phases are actually connected, and each would otherwise fail in "
      "a way that points at the wrong thing. The drive is prepared and put back automatically — "
      "diagnostics mode, Operation Enabled, brake released, then everything restored as found.";
  openPhase.caveats = {
      "The brake is released while the check runs, so anything the brake was holding is free to "
      "move. On a vertical or loaded axis, support the load first.",
      "The check itself can turn the motor when nothing else holds the shaft.",
      "Releasing a pin brake turns the motor by design, to lift the load off the pin.",
      "The bus must be exchanging process data (OP state): the drive's state machine only advances "
      "while its statusword is updating, so the procedure cannot enable the drive otherwise.",
      "A detected open phase is reported as a failed run — the check completed and found a fault, "
      "which is a result rather than an error.",
  };
  openPhase.movesMotor = true;
  // Not requiresEnabled: the procedure enables the drive itself as its first step. That flag is for
  // a procedure needing the drive *already* enabled, which would make enabling the operator's job.
  openPhase.requiresEnabled = false;
  openPhase.steps = openPhaseDetectionSteps();

  entries.push_back(ProcedureCatalogueEntry{
      .descriptor = std::move(openPhase),
      .applies = [](Device& device) { return device.vendorId() == kSynapticonVendorId; },
      // Takes no parameters: the timings are properties of the command, not a caller's choice.
      .makeBody = [](const nlohmann::json&) -> std::expected<ProcedureBody, std::string> {
        return [](Device& device, ProgressReporter& reporter, std::stop_token stop) {
          return runOpenPhaseDetectionProcedure(device, reporter, std::move(stop));
        };
      },
  });

  return entries;
}

// The procedures @p devicePosition supports, as pointers into the immortal catalogue.
//
// The borrow is released before this returns, and that is deliberate rather than incidental: every
// caller below goes on to call something that borrows again (ProcedureManager::start does its own
// withDevice), and busMutex_ is shared but not recursive — a second shared acquisition can deadlock
// behind a writer that arrived between the two. So applicability is decided under the lock and
// nothing else is done while holding it.
std::expected<std::vector<const ProcedureCatalogueEntry*>, ProcedureError> applicableEntries(
    DeviceManager& deviceManager, uint16_t devicePosition) {
  using Entries = std::vector<const ProcedureCatalogueEntry*>;
  auto found = deviceManager.withDevice(devicePosition,
                                        [](Device& device) -> std::expected<Entries, std::string> {
                                          Entries entries;
                                          for (const auto& entry : procedureCatalogue()) {
                                            if (entry.applies(device)) {
                                              entries.push_back(&entry);
                                            }
                                          }
                                          return entries;
                                        });
  if (!found) {
    return std::unexpected(
        ProcedureError{.kind = ProcedureError::Kind::kUnknownDevice, .message = found.error()});
  }
  return *found;
}

// One procedure by name, if the device has it. A name the catalogue does not hold and one this
// device does not support are the same answer to a client: nothing is addressable there.
std::expected<const ProcedureCatalogueEntry*, ProcedureError> applicableEntry(
    DeviceManager& deviceManager, uint16_t devicePosition, std::string_view name) {
  auto entries = applicableEntries(deviceManager, devicePosition);
  if (!entries) {
    return std::unexpected(entries.error());
  }
  auto it = std::ranges::find_if(
      *entries, [name](const auto* entry) { return entry->descriptor.name == name; });
  if (it == entries->end()) {
    return std::unexpected(ProcedureError{
        .kind = ProcedureError::Kind::kUnknownProcedure,
        .message = std::format("device {} has no procedure '{}'", devicePosition, name)});
  }
  return *it;
}

// The retained snapshot, or the descriptor's all-idle one when the procedure has never run here.
ProcedureSnapshot snapshotOrIdle(ProcedureManager& procedureManager, uint16_t devicePosition,
                                 const ProcedureDescriptor& descriptor) {
  return procedureManager.snapshot(devicePosition, descriptor.name)
      .value_or(idleSnapshot(descriptor.steps));
}

}  // namespace

const std::vector<ProcedureCatalogueEntry>& procedureCatalogue() {
  static const std::vector<ProcedureCatalogueEntry> entries = buildCatalogue();
  return entries;
}

void to_json(nlohmann::json& j, const ProcedureListing& listing) {
  j = nlohmann::json{
      {"descriptor", listing.descriptor},
      {"snapshot", listing.snapshot},
  };
}

std::expected<std::vector<ProcedureListing>, ProcedureError> listProcedures(
    DeviceManager& deviceManager, ProcedureManager& procedureManager, uint16_t devicePosition) {
  auto entries = applicableEntries(deviceManager, devicePosition);
  if (!entries) {
    return std::unexpected(entries.error());
  }
  std::vector<ProcedureListing> listings;
  listings.reserve(entries->size());
  for (const auto* entry : *entries) {
    listings.push_back(ProcedureListing{
        .descriptor = entry->descriptor,
        .snapshot = snapshotOrIdle(procedureManager, devicePosition, entry->descriptor)});
  }
  return listings;
}

std::expected<ProcedureSnapshot, ProcedureError> procedureSnapshot(
    DeviceManager& deviceManager, ProcedureManager& procedureManager, uint16_t devicePosition,
    std::string_view name) {
  auto entry = applicableEntry(deviceManager, devicePosition, name);
  if (!entry) {
    return std::unexpected(entry.error());
  }
  return snapshotOrIdle(procedureManager, devicePosition, (*entry)->descriptor);
}

std::expected<ProcedureSnapshot, ProcedureError> startProcedure(DeviceManager& deviceManager,
                                                                ProcedureManager& procedureManager,
                                                                uint16_t devicePosition,
                                                                std::string_view name,
                                                                const nlohmann::json& request) {
  auto entry = applicableEntry(deviceManager, devicePosition, name);
  if (!entry) {
    return std::unexpected(entry.error());
  }
  auto body = (*entry)->makeBody(request);
  if (!body) {
    return std::unexpected(
        ProcedureError{.kind = ProcedureError::Kind::kInvalidRequest, .message = body.error()});
  }
  const auto& descriptor = (*entry)->descriptor;
  if (auto started = procedureManager.start(devicePosition, descriptor.name, descriptor.steps,
                                            std::move(*body));
      !started) {
    return std::unexpected(started.error());
  }
  return snapshotOrIdle(procedureManager, devicePosition, descriptor);
}

std::expected<void, ProcedureError> cancelProcedure(DeviceManager& deviceManager,
                                                    ProcedureManager& procedureManager,
                                                    uint16_t devicePosition,
                                                    std::string_view name) {
  auto entry = applicableEntry(deviceManager, devicePosition, name);
  if (!entry) {
    return std::unexpected(entry.error());
  }
  if (!procedureManager.cancel(devicePosition, (*entry)->descriptor.name)) {
    return std::unexpected(ProcedureError{
        .kind = ProcedureError::Kind::kUnknownProcedure,
        .message = std::format("device {} has no run of '{}' to cancel", devicePosition, name)});
  }
  return {};
}

}  // namespace mm::node
