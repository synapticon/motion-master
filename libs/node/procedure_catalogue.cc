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

  ProcedureDescriptor commissioning;
  commissioning.name = std::string(kOffsetDetectionProcedure);
  commissioning.title = "Offset detection";
  commissioning.description =
      "Runs every measurement a motor needs, in the order they depend on each other, in one "
      "prepared "
      "session: open phase detection, phase resistance, phase inductance, pole pair detection, "
      "motor phase order detection, and commutation offset measurement. Running it as one "
      "procedure "
      "is what makes the order impossible to get wrong, and each step reports its own result, so "
      "a run that stops half way still shows everything it established. The drive is put into "
      "diagnostics mode and enabled once, the brake is released once, and everything is restored "
      "afterwards.";
  commissioning.caveats = {
      "This turns the rotor: pole pair detection and motor phase order detection both have to, and "
      "the offset measurement may. The shaft must be free, and whatever it drives must be safe to "
      "move through several separate motions.",
      "The brake is released partway through — as late as the sequence allows, since the first "
      "three "
      "measurements do not need it — and stays released until the end. Anything it was holding is "
      "free to move for that whole stretch: on a vertical or loaded axis, support the load first.",
      "Releasing a pin brake turns the motor by design, to lift the load off the pin.",
      "The measured resistance, inductance and pole pair count are reported but not stored — the "
      "drive does not write them, and this does not either. Objects 0x2003:03, :04 and :01 are "
      "where "
      "they belong. The phase order and the commutation offset the firmware does store itself, and "
      "the restore does not undo those.",
      "A failing step stops the run rather than being skipped, because every step depends on the "
      "ones "
      "before it. An open phase stops it immediately.",
      "The bus must be exchanging process data (OP state): the drive's state machine only advances "
      "while its statusword is updating, so the procedure cannot enable the drive otherwise.",
  };
  commissioning.movesMotor = true;
  commissioning.requiresEnabled = false;
  commissioning.steps = offsetDetectionSteps();

  entries.push_back(ProcedureCatalogueEntry{
      .descriptor = std::move(commissioning),
      .applies = [](Device& device) { return device.vendorId() == kSynapticonVendorId; },
      .makeBody = [](const nlohmann::json&) -> std::expected<ProcedureBody, std::string> {
        return [](Device& device, ProgressReporter& reporter, std::stop_token stop) {
          return runOffsetDetectionProcedure(device, reporter, std::move(stop));
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
      "diagnostics mode, Operation Enabled, then the mode restored as found.";
  openPhase.caveats = {
      "The brake is left exactly as found — this command does not require it released, and an "
      "engaged brake simply keeps the shaft still while the check runs.",
      "The check can turn the motor when nothing holds the shaft: no brake, or one already "
      "disengaged.",
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

  ProcedureDescriptor polePair;
  polePair.name = std::string(kPolePairDetectionProcedure);
  polePair.title = "Pole pair detection";
  polePair.description =
      "Counts the connected motor's pole pairs, by turning the rotor and watching what it takes to "
      "do so. Part of commissioning an absolute-encoder axis, where it is run after open phase "
      "detection and before motor phase order detection and commutation offset measurement. The "
      "drive is put into diagnostics mode, enabled, its brake released, and all of that restored "
      "afterwards.";
  polePair.caveats = {
      "This command turns the rotor — it has to, in order to count poles. The shaft must be free "
      "to "
      "move, and whatever it drives must be safe to move with it.",
      "The brake is released while it runs, because this command requires that. Anything the brake "
      "was holding is free to move: on a vertical or loaded axis, support the load first.",
      "Releasing a pin brake turns the motor by design, to lift the load off the pin.",
      "The count is reported, not stored — nothing in the drive's configuration changes. Object "
      "0x2003:01 is where a pole pair count belongs if you want to keep it.",
      "The bus must be exchanging process data (OP state): the drive's state machine only advances "
      "while its statusword is updating, so the procedure cannot enable the drive otherwise.",
      "A drive that cannot raise the motor phase currents far enough reports the run as failed — a "
      "limited DC-link voltage or a high motor phase impedance can both cause that.",
  };
  polePair.movesMotor = true;
  polePair.requiresEnabled = false;
  polePair.steps = polePairDetectionSteps();

  entries.push_back(ProcedureCatalogueEntry{
      .descriptor = std::move(polePair),
      .applies = [](Device& device) { return device.vendorId() == kSynapticonVendorId; },
      .makeBody = [](const nlohmann::json&) -> std::expected<ProcedureBody, std::string> {
        return [](Device& device, ProgressReporter& reporter, std::stop_token stop) {
          return runPolePairDetectionProcedure(device, reporter, std::move(stop));
        };
      },
  });

  ProcedureDescriptor motorPhaseOrder;
  motorPhaseOrder.name = std::string(kMotorPhaseOrderDetectionProcedure);
  motorPhaseOrder.title = "Motor phase order detection";
  motorPhaseOrder.description =
      "Works out whether the motor's phases are wired normally or inverted — whether the sensor "
      "angle and the rotor angle move in the same direction — and stores the answer in the drive "
      "(0x2003:05). Unlike the other detections this one reconfigures the drive, which is the "
      "point "
      "of running it: commutation offset measurement requires that it has been done, so it is the "
      "step immediately before it, and it has to be repeated after every power-on on an axis with "
      "an "
      "incremental encoder. The drive is put into diagnostics mode, enabled, its brake released, "
      "and "
      "all of that restored afterwards.";
  motorPhaseOrder.caveats = {
      "This command turns the rotor. The shaft must be free to move, and whatever it drives must "
      "be "
      "safe to move with it.",
      "The brake is released while it runs, because this command requires that. Anything the brake "
      "was holding is free to move: on a vertical or loaded axis, support the load first.",
      "Releasing a pin brake turns the motor by design, to lift the load off the pin.",
      "A successful run changes the drive's configuration, and the restore does not undo it — the "
      "new phase order is the result, not a side effect.",
      "The bus must be exchanging process data (OP state): the drive's state machine only advances "
      "while its statusword is updating, so the procedure cannot enable the drive otherwise.",
  };
  motorPhaseOrder.movesMotor = true;
  motorPhaseOrder.requiresEnabled = false;
  motorPhaseOrder.steps = motorPhaseOrderDetectionSteps();

  entries.push_back(ProcedureCatalogueEntry{
      .descriptor = std::move(motorPhaseOrder),
      .applies = [](Device& device) { return device.vendorId() == kSynapticonVendorId; },
      .makeBody = [](const nlohmann::json&) -> std::expected<ProcedureBody, std::string> {
        return [](Device& device, ProgressReporter& reporter, std::stop_token stop) {
          return runMotorPhaseOrderDetectionProcedure(device, reporter, std::move(stop));
        };
      },
  });

  ProcedureDescriptor commutationOffset;
  commutationOffset.name = std::string(kCommutationOffsetDetectionProcedure);
  commutationOffset.title = "Commutation offset detection";
  commutationOffset.description =
      "Detects the motor phase order and then measures the commutation angle offset, storing both "
      "in "
      "the drive (0x2003:05, and 0x2001 marked valid in 0x2009:01). This is what commissions an "
      "axis, "
      "and the two commands are one unit rather than two you sequence yourself: an offset measured "
      "against an unknown phase order is wrong, and the drive does not check that the phase order "
      "was "
      "established. It is also exactly the sequence an axis with an incremental encoder repeats "
      "after "
      "every power-on. On a new absolute-encoder axis, run open phase detection and pole pair "
      "detection first. How the offset is measured is configured on the drive rather than chosen "
      "here "
      "— the method in 0x2009:03 decides whether that step turns the rotor and which way the "
      "brake goes — and the method that ran is reported with the result.";
  commutationOffset.caveats = {
      "This turns the rotor whatever the method is set to: the stationary offset method does not "
      "turn "
      "it, but phase order detection always does. The shaft must be free, and whatever it drives "
      "must "
      "be safe to move.",
      "The brake is released for phase order detection, which requires that unconditionally — "
      "so "
      "anything it was holding is free to move, even under the stationary method. Support the load "
      "first. It is engaged again before a stationary measurement, which cannot hold the load "
      "itself.",
      "Releasing a pin brake turns the motor by design, to lift the load off the pin.",
      "The rotating methods (0x2009:03 = 0 or 1) measure with the brake released; method 1 "
      "additionally needs the gains in 0x2009:04-06 tuned. The stationary method (2) is less "
      "precise.",
      "A successful run changes the drive's configuration twice over, and the restore does not "
      "undo "
      "either — the phase order and the offset are the result, not side effects.",
      "The bus must be exchanging process data (OP state): the drive's state machine only advances "
      "while its statusword is updating, so the procedure cannot enable the drive otherwise.",
  };
  commutationOffset.movesMotor = true;
  commutationOffset.requiresEnabled = false;
  commutationOffset.steps = commutationOffsetDetectionSteps();

  entries.push_back(ProcedureCatalogueEntry{
      .descriptor = std::move(commutationOffset),
      .applies = [](Device& device) { return device.vendorId() == kSynapticonVendorId; },
      .makeBody = [](const nlohmann::json&) -> std::expected<ProcedureBody, std::string> {
        return [](Device& device, ProgressReporter& reporter, std::stop_token stop) {
          return runCommutationOffsetDetectionProcedure(device, reporter, std::move(stop));
        };
      },
  });

  ProcedureDescriptor phaseResistance;
  phaseResistance.name = std::string(kPhaseResistanceMeasurementProcedure);
  phaseResistance.title = "Phase resistance measurement";
  phaseResistance.description =
      "Measures the resistance of one motor phase, in milliohms, and reports it. The drive is put "
      "into diagnostics mode, enabled, and restored to exactly the state it was found in "
      "afterwards. Measured at the drive's own terminals, so what comes back is the winding plus "
      "whatever is in series with it — your cabling and connectors included.";
  phaseResistance.caveats = {
      "The value is reported, not stored — nothing in the drive's configuration changes. Object "
      "0x2003:03 is where a phase resistance belongs if you want to keep it.",
      "The brake is left exactly as found — this command does not require it released, and an "
      "engaged brake steadying the shaft is the better state to measure in.",
      "The shaft can still turn if nothing holds it: no brake, or one already disengaged.",
      "The bus must be exchanging process data (OP state): the drive's state machine only advances "
      "while its statusword is updating, so the procedure cannot enable the drive otherwise.",
      "A drive that cannot raise the current amplitude far enough reports the run as failed rather "
      "than returning a low reading.",
  };
  phaseResistance.movesMotor = true;
  phaseResistance.requiresEnabled = false;
  phaseResistance.steps = phaseResistanceMeasurementSteps();

  entries.push_back(ProcedureCatalogueEntry{
      .descriptor = std::move(phaseResistance),
      .applies = [](Device& device) { return device.vendorId() == kSynapticonVendorId; },
      .makeBody = [](const nlohmann::json&) -> std::expected<ProcedureBody, std::string> {
        return [](Device& device, ProgressReporter& reporter, std::stop_token stop) {
          return runPhaseResistanceMeasurementProcedure(device, reporter, std::move(stop));
        };
      },
  });

  ProcedureDescriptor phaseInductance;
  phaseInductance.name = std::string(kPhaseInductanceMeasurementProcedure);
  phaseInductance.title = "Phase inductance measurement";
  phaseInductance.description =
      "Measures the inductance of one motor phase, in microhenries, and reports it. The companion "
      "of phase resistance measurement: same preconditions, same preparation, and the brake "
      "handled "
      "the same way — only the quantity differs. The drive is put into diagnostics mode, enabled, "
      "and restored to exactly the state it was found in afterwards.";
  phaseInductance.caveats = {
      "The value is reported, not stored — nothing in the drive's configuration changes. Object "
      "0x2003:04 is where a phase inductance belongs if you want to keep it.",
      "The brake is left exactly as found — this command does not require it released, and an "
      "engaged brake steadying the shaft is the better state to measure in.",
      "The shaft can still turn if nothing holds it: no brake, or one already disengaged.",
      "The bus must be exchanging process data (OP state): the drive's state machine only advances "
      "while its statusword is updating, so the procedure cannot enable the drive otherwise.",
      "A drive that cannot raise the current amplitude far enough reports the run as failed rather "
      "than returning a low reading.",
  };
  phaseInductance.movesMotor = true;
  phaseInductance.requiresEnabled = false;
  phaseInductance.steps = phaseInductanceMeasurementSteps();

  entries.push_back(ProcedureCatalogueEntry{
      .descriptor = std::move(phaseInductance),
      .applies = [](Device& device) { return device.vendorId() == kSynapticonVendorId; },
      .makeBody = [](const nlohmann::json&) -> std::expected<ProcedureBody, std::string> {
        return [](Device& device, ProgressReporter& reporter, std::stop_token stop) {
          return runPhaseInductanceMeasurementProcedure(device, reporter, std::move(stop));
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
