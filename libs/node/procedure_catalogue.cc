#include "node/procedure_catalogue.h"

#include <algorithm>
#include <expected>
#include <format>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "node/profile_procedures.h"
#include "node/somanet_procedures.h"
#include "node/synapticon.h"

namespace mm::node {

namespace {

// Every entry the server knows. Built once, never mutated: a caller may hold the reference.
std::vector<ProcedureCatalogueEntry> buildCatalogue() {
  std::vector<ProcedureCatalogueEntry> entries;

  // The generic CiA301 procedures come first, and their applicability is the widest in the table:
  // every CoE device carries the non-volatile storage objects, whatever profile it implements, so
  // these are offered on a third-party slave as readily as on a drive. CoE support comes from the
  // slave's EEPROM capability bits and is known as soon as the device is, which is what an
  // `applies` predicate is allowed to consult.
  ProcedureDescriptor storeParameters;
  storeParameters.name = std::string(kStoreParametersProcedure);
  storeParameters.title = "Store parameters";
  storeParameters.description =
      "Persists the device's current parameter values to non-volatile memory — the CoE 0x1010 "
      "\"store parameters\" object — so the changes you have made survive a power cycle. The "
      "\"save\" signature is written and the device is then polled until it reports the save "
      "complete, which usually takes about a second while it writes to flash.";
  storeParameters.caveats = {
      "It stores what the device currently holds, which includes anything written since the last "
      "store — there is no selecting what to keep.",
      "Cancelling stops the wait, not the store: the command has already been written, so the "
      "device may complete it anyway.",
      "The mailbox must be active, so the device has to be in PRE-OP or above.",
  };
  storeParameters.movesMotor = false;
  storeParameters.requiresEnabled = false;
  storeParameters.steps = storeParametersSteps();

  entries.push_back(ProcedureCatalogueEntry{
      .descriptor = std::move(storeParameters),
      .applies = [](Device& device) { return device.supportsCoe(); },
      .makeBody = [](const nlohmann::json&) -> std::expected<ProcedureBody, std::string> {
        return [](Device& device, ProgressReporter& reporter, std::stop_token stop) {
          return runStoreParametersProcedure(device, reporter, std::move(stop));
        };
      },
  });

  ProcedureDescriptor restoreDefaults;
  restoreDefaults.name = std::string(kRestoreDefaultParametersProcedure);
  restoreDefaults.title = "Restore default parameters";
  restoreDefaults.description =
      "Restores the device's default parameters — the CoE 0x1011 \"restore default parameters\" "
      "object — for the selected group. The \"load\" signature is written and the device is then "
      "polled until it reports the restore complete.";
  restoreDefaults.caveats = {
      "Destructive: the selected group's live values are overwritten with the device's defaults, "
      "so anything you have not stored is gone. Run Store parameters first if you want to keep it.",
      "The defaults are restored into volatile memory. They become permanent only if a store "
      "follows, and some devices apply them only after a reset or power cycle.",
      "Cancelling stops the wait, not the restore: the command has already been written, so the "
      "device may complete it anyway.",
      "The mailbox must be active, so the device has to be in PRE-OP or above.",
  };
  restoreDefaults.movesMotor = false;
  restoreDefaults.requiresEnabled = false;
  restoreDefaults.parameters = restoreDefaultParametersParameters();
  restoreDefaults.steps = restoreDefaultParametersSteps();

  entries.push_back(ProcedureCatalogueEntry{
      .descriptor = std::move(restoreDefaults),
      .applies = [](Device& device) { return device.supportsCoe(); },
      .makeBody = [](const nlohmann::json& request) -> std::expected<ProcedureBody, std::string> {
        auto spec = parseRestoreDefaultParametersRequest(request);
        if (!spec) {
          return std::unexpected(spec.error());
        }
        return [spec = *spec](Device& device, ProgressReporter& reporter, std::stop_token stop) {
          return runRestoreDefaultParametersProcedure(device, reporter, std::move(stop), spec);
        };
      },
  });

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
  osCommand.parameters = osCommandParameters();
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

  ProcedureDescriptor encoderRegister;
  encoderRegister.name = std::string(kEncoderRegisterProcedure);
  encoderRegister.title = "Encoder register communication";
  encoderRegister.description =
      "Reads or writes one register of an encoder, through the encoder's own register "
      "communication service. Only BiSS implements that today, so this addresses a BiSS encoder — "
      "the internal encoder of a Circulo, say — and the register map is the encoder chip's rather "
      "than the drive's: what a register means comes from its own documentation. Unlike the motor "
      "measurements this prepares nothing and moves nothing, so it can be run on a drive that is "
      "exchanging process data without disturbing it. A write is answered the same way a read is, "
      "with what the register holds afterwards.";
  encoderRegister.caveats = {
      "A write reconfigures the encoder, and nothing here checks what a value means — a wrong one "
      "can leave an encoder unable to report position. Read the encoder chip's own register "
      "documentation first.",
      "The command works only on a configured BiSS encoder. Any other encoder, or an ordinal whose "
      "slot is not configured, is refused by the drive as OS error 251 (\"command not allowed\").",
      "The iC-MU soft reset (0x07 into register 0x75) restarts the chip and is acknowledged "
      "without a value, so that one access reports no reading.",
      "The mailbox must be active, so the device has to be in PRE-OP or above — but it need not be "
      "enabled, in any particular mode of operation, or exchanging process data.",
  };
  encoderRegister.movesMotor = false;
  encoderRegister.requiresEnabled = false;
  encoderRegister.parameters = encoderRegisterParameters();
  encoderRegister.steps = encoderRegisterSteps();

  entries.push_back(ProcedureCatalogueEntry{
      .descriptor = std::move(encoderRegister),
      .applies = [](Device& device) { return device.vendorId() == kSynapticonVendorId; },
      .makeBody = [](const nlohmann::json& request) -> std::expected<ProcedureBody, std::string> {
        auto spec = parseEncoderRegisterRequest(request);
        if (!spec) {
          return std::unexpected(spec.error());
        }
        return [spec = *spec](Device& device, ProgressReporter& reporter, std::stop_token stop) {
          return runEncoderRegisterProcedure(device, reporter, std::move(stop), spec);
        };
      },
  });

  ProcedureDescriptor icMuCalibrationMode;
  icMuCalibrationMode.name = std::string(kIcMuCalibrationModeProcedure);
  icMuCalibrationMode.title = "iC-MU calibration mode";
  icMuCalibrationMode.description =
      "Sets how the BiSS service clocks an iC-MU encoder — the chip behind a Circulo's internal "
      "encoder. Standard is normal operation. Configuration keeps the encoder clocked but uses "
      "only the register-communication bits, so position stops updating and no CRC fault is "
      "raised, which is what makes it possible to change the encoder's configuration registers "
      "with Encoder register communication. Raw clocks an encoder already configured for raw "
      "output and averages that data into 0x2704. Calibrating an encoder means moving between "
      "these modes, not setting one switch.";
  icMuCalibrationMode.caveats = {
      "There is no restore: the encoder stays in the mode this sets until another run puts it back "
      "to standard. Leaving one in configuration mode leaves the drive without a position update.",
      "In configuration mode the encoder's position is not updated and the BiSS CRC error is "
      "suppressed, so the drive will not report a problem it would normally fault on.",
      "Entering configuration mode saves the current position, and entering raw mode counts from "
      "that saved position because raw data is relative — so do not move the motor while in "
      "configuration mode if raw mode is to follow.",
      "The command works only on a configured Circulo internal encoder. Anything else is refused "
      "by the drive as OS error 251 (\"command not allowed\").",
      "The mailbox must be active, so the device has to be in PRE-OP or above.",
  };
  icMuCalibrationMode.movesMotor = false;
  icMuCalibrationMode.requiresEnabled = false;
  icMuCalibrationMode.parameters = icMuCalibrationModeParameters();
  icMuCalibrationMode.steps = icMuCalibrationModeSteps();

  entries.push_back(ProcedureCatalogueEntry{
      .descriptor = std::move(icMuCalibrationMode),
      .applies = [](Device& device) { return device.vendorId() == kSynapticonVendorId; },
      .makeBody = [](const nlohmann::json& request) -> std::expected<ProcedureBody, std::string> {
        auto spec = parseIcMuCalibrationModeRequest(request);
        if (!spec) {
          return std::unexpected(spec.error());
        }
        return [spec = *spec](Device& device, ProgressReporter& reporter, std::stop_token stop) {
          return runIcMuCalibrationModeProcedure(device, reporter, std::move(stop), spec);
        };
      },
  });

  ProcedureDescriptor hrdStreaming;
  hrdStreaming.name = std::string(kHrdStreamingProcedure);
  hrdStreaming.title = "HRD streaming";
  hrdStreaming.description =
      "Records one signal into the drive's high resolution data files. One sample is written every "
      "millisecond — the drive's control loop runs faster, up to once every 250 µs, but the "
      "recording is decimated to 1 kHz whatever the loop period is. "
      "Encoder raw data captures the position word an iC-MU encoder reports; system identification "
      "data captures the velocity and torque actual values. Arming the recording deletes the "
      "previous one, and recording occupies the whole requested duration. The recording stays on "
      "the drive — read it back from the device's HRD endpoint, which needs the same data "
      "selection to decode it.";
  hrdStreaming.caveats = {
      "Arming a recording deletes every high resolution data file already on the drive, so the "
      "previous "
      "recording is gone the moment this starts — read one back before recording the next.",
      "Encoder raw data records zeros unless the encoder was put into raw mode first with iC-MU "
      "calibration mode. The drive streams whatever the encoder is currently clocked for and "
      "reports no problem.",
      "System identification data records an unexcited drive unless a system identification run "
      "was configured and started first.",
      "The duration limit depends on the data: 10000 ms for encoder raw, but only 6000 ms for "
      "system identification, since the recording has to fit five 8032-byte files.",
      "Cancelling stops the recording on the drive and discards whatever it had buffered but not "
      "yet written — up to about 250 samples. What reached the files stays, so the recording is a "
      "short one rather than none.",
      "The mailbox must be active, so the device has to be in PRE-OP or above.",
  };
  hrdStreaming.movesMotor = false;
  hrdStreaming.requiresEnabled = false;
  hrdStreaming.parameters = hrdStreamingParameters();
  hrdStreaming.steps = hrdStreamingSteps();

  entries.push_back(ProcedureCatalogueEntry{
      .descriptor = std::move(hrdStreaming),
      .applies = [](Device& device) { return device.vendorId() == kSynapticonVendorId; },
      .makeBody = [](const nlohmann::json& request) -> std::expected<ProcedureBody, std::string> {
        auto spec = parseHrdStreamingRequest(request);
        if (!spec) {
          return std::unexpected(spec.error());
        }
        return [spec = *spec](Device& device, ProgressReporter& reporter, std::stop_token stop) {
          return runHrdStreamingProcedure(device, reporter, std::move(stop), spec);
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

  ProcedureDescriptor skippedCycles;
  skippedCycles.name = std::string(kSkippedCyclesProcedure);
  skippedCycles.title = "Skipped cycles counter";
  skippedCycles.description =
      "Reads how many cycles one of the drive's two control loops has failed to start on time "
      "since it began running. The firmware counts a cycle as skipped when it starts late enough "
      "to miss its slot, and adds the whole backlog when several are missed at once — so this is "
      "missed cycles, not missed deadlines. Nothing here changes the drive: no operation mode, no "
      "state, no brake, no motion.";
  skippedCycles.caveats = {
      "The counter is cumulative since the loop started and nothing resets it, so a single reading "
      "means little. Read it, wait, read it again: a large but unchanging number is a startup "
      "transient, a small one that keeps climbing is a drive still missing cycles now.",
      "The two loops are counted separately and a reading from one says nothing about the other.",
      "Cycles skipped while a controller is enabled also raise a CtrlCyEx warning in the drive's "
      "error report; ones skipped while it is disabled raise nothing, so the counter can climb "
      "with no warning anywhere.",
      "This counts the drive's own loop, not Motion Master's. The master's skipped cycles are on "
      "the Server → Game Loop page, and the two are independent.",
  };
  skippedCycles.parameters = skippedCyclesParameters();
  skippedCycles.movesMotor = false;
  skippedCycles.requiresEnabled = false;
  skippedCycles.steps = skippedCyclesSteps();

  entries.push_back(ProcedureCatalogueEntry{
      .descriptor = std::move(skippedCycles),
      .applies = [](Device& device) { return device.vendorId() == kSynapticonVendorId; },
      .makeBody = [](const nlohmann::json& body) -> std::expected<ProcedureBody, std::string> {
        auto request = parseSkippedCyclesRequest(body);
        if (!request) {
          return std::unexpected(request.error());
        }
        return
            [request = *request](Device& device, ProgressReporter& reporter, std::stop_token stop) {
              return runSkippedCyclesProcedure(device, reporter, std::move(stop), request);
            };
      },
  });

  ProcedureDescriptor torqueConstant;
  torqueConstant.name = std::string(kTorqueConstantMeasurementProcedure);
  torqueConstant.title = "Torque constant measurement";
  torqueConstant.description =
      "Measures how much torque the motor produces per ampere of effective (RMS) current, in "
      "mNm/A_rms, and reports it. The drive has no torque sensor, so it measures the same constant "
      "from the other side: it spins the motor up over about ten seconds, holds it at speed, and "
      "works the constant out from the voltage the motor generates. The drive is put into "
      "diagnostics mode, enabled, its brake released, and restored to exactly the state it was "
      "found in afterwards.";
  torqueConstant.caveats = {
      "Measure and store pole pairs, phase resistance and phase inductance first. The drive "
      "subtracts the winding impedance to find the back-EMF, and it takes that impedance and the "
      "pole pair count from 0x2003:01, :03 and :04 — not from anything it measures here. Stale "
      "values there give a wrong constant with no indication that anything went wrong; badly wrong "
      "ones give a negative result.",
      "The value is reported, not stored — nothing in the drive's configuration changes. Object "
      "0x2003:02 is where a torque constant belongs if you want to keep it, but it is stored in "
      "µNm/A_rms while this reports mNm/A_rms: multiply by 1000 before writing it.",
      "The rotor turns continuously for the whole run, not by a step or a fraction of a turn, and "
      "this is the longest-moving of these procedures. The shaft must be free and whatever it "
      "drives safe to keep moving.",
      "The brake is released, which this command requires — so anything it was holding is free to "
      "move. Support the load first.",
      "Releasing a pin brake turns the motor by design, to lift the load off the pin.",
      "The bus must be exchanging process data (OP state): the drive's state machine only advances "
      "while its statusword is updating, so the procedure cannot enable the drive otherwise.",
  };
  torqueConstant.movesMotor = true;
  torqueConstant.requiresEnabled = false;
  torqueConstant.steps = torqueConstantMeasurementSteps();

  entries.push_back(ProcedureCatalogueEntry{
      .descriptor = std::move(torqueConstant),
      .applies = [](Device& device) { return device.vendorId() == kSynapticonVendorId; },
      .makeBody = [](const nlohmann::json&) -> std::expected<ProcedureBody, std::string> {
        return [](Device& device, ProgressReporter& reporter, std::stop_token stop) {
          return runTorqueConstantMeasurementProcedure(device, reporter, std::move(stop));
        };
      },
  });

  ProcedureDescriptor firmware;
  firmware.name = std::string(kFirmwareInstallationProcedure);
  firmware.title = "Firmware installation";
  firmware.description =
      "Installs a SOMANET firmware package. The device is taken to BOOT, where its bootloader "
      "accepts the package's application and communication binaries over FoE and its SII image "
      "into the EEPROM, and is then returned to the state you choose. PRE-OP is the default and is "
      "the confirmation that it worked: the bootloader hands over to the newly written firmware on "
      "that transition, so reaching PRE-OP means the new firmware booted and answered. The "
      "descriptive extras a package carries — the ESI and the stack image — are skipped by default "
      "and can be un-skipped by editing the list.";
  firmware.caveats = {
      "The device stops exchanging process data for the whole installation, so anything it was "
      "driving is uncontrolled from the moment it leaves OP. Other devices on the bus keep "
      "running, but the bus is briefly re-mapped when this one rejoins.",
      "Cancelling does not undo anything, and between two files it leaves the device part-flashed. "
      "A transfer already under way finishes regardless — cancellation is noticed between files.",
      "If the package writes an SII, that part does need a power cycle: the ESC reads its EEPROM "
      "at reset, unlike the firmware, which loads on the transition out of BOOT.",
      "Choose BOOT as the final state when no application will be present — after erasing one, or "
      "between two installs — because a PRE-OP transition then has nothing to hand over to and the "
      "drive answers AL status 0x0014, \"No valid firmware\".",
      "Nothing here checks that the package matches the device. A package built for other hardware "
      "is written as readily as the right one.",
  };
  firmware.movesMotor = false;
  firmware.requiresEnabled = false;
  firmware.parameters = firmwareInstallationParameters();
  firmware.steps = firmwareInstallationSteps();

  entries.push_back(ProcedureCatalogueEntry{
      .descriptor = std::move(firmware),
      .applies = [](Device& device) { return device.vendorId() == kSynapticonVendorId; },
      .makeBody = [](const nlohmann::json& request) -> std::expected<ProcedureWork, std::string> {
        auto spec = parseFirmwareInstallationRequest(request);
        if (!spec) {
          return std::unexpected(spec.error());
        }
        // The only BusProcedureBody in the table: this one changes AL state, so it is handed the
        // manager and borrows per step rather than being given a device for the whole run.
        return BusProcedureBody{
            [spec = std::move(*spec)](DeviceManager& deviceManager, uint16_t devicePosition,
                                      ProgressReporter& reporter, std::stop_token stop) {
              return runFirmwareInstallationProcedure(deviceManager, devicePosition, reporter,
                                                      std::move(stop), spec);
            }};
      },
  });

  // Served in name order, whatever order the table is written in. The authoring order above is
  // grouped by profile so the commentary can explain each group's applicability, and that is worth
  // keeping — but it is a poor order to *read* a list in, and it would shift under every insertion,
  // so a procedure would move in the sidebar because of where a row happened to be added. Sorting
  // here rather than in listProcedures does it once for the process instead of once per poll, and
  // means every consumer of the catalogue sees the same order: applicableEntries preserves it, so
  // the list endpoint and the Console's sidebar are ordered without either of them sorting.
  //
  // By name rather than title because the name is the stable identifier — it is what the URL and
  // the API carry, so the order cannot drift when a title is reworded. The two happen to agree for
  // every entry, each name being its title's slug.
  std::ranges::sort(entries, {}, [](const ProcedureCatalogueEntry& entry) {
    return std::string_view(entry.descriptor.name);
  });

  return entries;
}

// The procedures @p devicePosition supports, as pointers into the immortal catalogue.
//
// The borrow is released before this returns, and that is deliberate rather than incidental: every
// caller below goes on to call something that borrows again (ProcedureManager::start does its own
// withDevice), and deviceSetMutex_ is shared but not recursive — a second shared acquisition can
// deadlock behind a writer that arrived between the two. So applicability is decided under the lock
// and nothing else is done while holding it.
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
  // One visit picks the ProcedureManager::start overload matching the shape the entry produced.
  // The two body types differ in arity, so neither converts to the other and the overload is exact.
  auto started = std::visit(
      [&](auto&& work) {
        return procedureManager.start(devicePosition, descriptor.name, descriptor.steps,
                                      std::forward<decltype(work)>(work));
      },
      std::move(*body));
  if (!started) {
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
