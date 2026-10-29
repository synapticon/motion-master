/* eslint-disable */
/* tslint:disable */
// @ts-nocheck
/*
 * ---------------------------------------------------------------
 * ## THIS FILE WAS GENERATED VIA SWAGGER-TYPESCRIPT-API        ##
 * ##                                                           ##
 * ## AUTHOR: acacode                                           ##
 * ## SOURCE: https://github.com/acacode/swagger-typescript-api ##
 * ---------------------------------------------------------------
 */

type UtilRequiredKeys<T, K extends keyof T> = Omit<T, K> & Required<Pick<T, K>>;

/** A CiA402 drive's control snapshot — its state machine, control words, and mode. */
export interface Cia402Status {
  /**
   * Decoded CiA402 state-machine state (from the statusword, 0x6041).
   * @example "OperationEnabled"
   */
  state:
    | "NotReadyToSwitchOn"
    | "SwitchOnDisabled"
    | "ReadyToSwitchOn"
    | "SwitchedOn"
    | "OperationEnabled"
    | "QuickStopActive"
    | "FaultReactionActive"
    | "Fault";
  /**
   * Raw statusword (0x6041), 0–65535.
   * @example 4663
   */
  statusword: number;
  /**
   * Last-commanded controlword (0x6040), 0–65535.
   * @example 15
   */
  controlword: number;
  /**
   * Active operation mode (display object 0x6061), as an INTEGER8 value.
   * @example 9
   */
  modeOfOperation: number;
  /**
   * Human-readable name of the active operation mode.
   * @example "CyclicSyncVelocity"
   */
  modeName: string;
  /**
   * Target position (0x607A), INTEGER32, in the drive's own position units. The drive acts on it in the position modes (PP, CSP) only, but it is reported in every mode. Read from the process image while the device exchanges, else over SDO. `null` when the drive has no such object or the read failed.
   * @example 100000
   */
  targetPosition: number | null;
  /**
   * Target velocity (0x60FF), INTEGER32, in the drive's own velocity units. The drive acts on it in the velocity modes (PV, CSV) only, but it is reported in every mode. Read from the process image while the device exchanges, else over SDO. `null` when the drive has no such object or the read failed.
   * @example 250000
   */
  targetVelocity: number | null;
  /**
   * Target torque (0x6071), INTEGER16 widened to INTEGER32, in per mille of rated torque. The drive acts on it in the torque modes (PT, CST) only, but it is reported in every mode. Read from the process image while the device exchanges, else over SDO. `null` when the drive has no such object or the read failed.
   * @example -100
   */
  targetTorque: number | null;
  /**
   * Position actual value (0x6064), INTEGER32, in the drive's own position units. Read from the process image while the device exchanges, else over SDO. `null` when the drive has no such object or the read failed.
   * @example 99874
   */
  actualPosition: number | null;
  /**
   * Velocity actual value (0x606C), INTEGER32, in the drive's own velocity units. Read from the process image while the device exchanges, else over SDO. `null` when the drive has no such object or the read failed.
   * @example 249000
   */
  actualVelocity: number | null;
  /**
   * Torque actual value (0x6077), INTEGER16 widened to INTEGER32, in per mille of rated torque. Read from the process image while the device exchanges, else over SDO. `null` when the drive has no such object or the read failed.
   * @example -150
   */
  actualTorque: number | null;
  /**
   * The drive's own short description of its last fault, read from the vendor object that holds one. A SOMANET drive keeps it in 0x203F:01 and reports an empty string while there is no fault. `null` when the drive has no such object or the read failed.
   * @example "CyclicHb"
   */
  errorDescription: string | null;
}

/** Every operation mode a device has, with what its "Supported drive modes" field (0x6502) says about each. */
export interface OperationModes {
  /**
   * Object 0x6502 verbatim, as an UNSIGNED32.
   * @example 197549
   */
  supportedDriveModes: number;
  /**
   * The set bits of 0x6502 from bit 16 upwards, reported as bit numbers and nothing more. The profile leaves their meaning entirely to the vendor, so nothing here can decode them — a SOMANET drive sets bits 16 and 17 and publishes no definition for either.
   * @example [16,17]
   */
  manufacturerBits: number[];
  /** Every mode, ascending by value — manufacturer modes first, then standard. */
  modes: OperationMode[];
}

/** One operation mode of one device. */
export interface OperationMode {
  /**
   * The value written to 0x6060 / read from 0x6061. Negative for a vendor mode.
   * @example -2
   */
  value: number;
  /**
   * PascalCase identifier, matching the modeName of GET /api/devices/{n}/cia402.
   * @example "Diagnostics"
   */
  name: string;
  /**
   * The profile's or the vendor's own wording, for a user-facing list.
   * @example "Diagnostics mode"
   */
  label: string;
  /**
   * Whether CiA402 or the vendor defines this mode.
   * @example "manufacturer"
   */
  kind: "standard" | "manufacturer";
  /**
   * The bit of 0x6502 that advertises this mode. Null for every manufacturer mode, and for NoMode, which is always legal and so has no bit.
   * @example 7
   */
  bit: number | null;
  /**
   * The profile's short form (pp, vl, pv, tq, hm, ip, csp, csv, cst, cstca).
   * @example "csp"
   */
  abbreviation: string | null;
  /**
   * Whether the drive advertises this mode. Null for every manufacturer mode — 0x6502 has no bit a master could read for one — which means "the drive does not say", not "no".
   * @example true
   */
  supported: boolean | null;
  /**
   * Present and true only for a mode the vendor's firmware marks deprecated.
   * @example true
   */
  deprecated?: boolean;
}

/** Point-in-time health of the real-time game loop. Times are nanoseconds; rates are hertz. All fields are diagnostic (relaxed reads), not synchronised. */
export interface GameLoopHealth {
  /**
   * Configured target cycle period, in microseconds
   * @format int64
   * @example 1000
   */
  periodUs: number;
  /**
   * Target cycle rate (1e6 / periodUs), in hertz
   * @format double
   * @example 1000
   */
  targetHz: number;
  /**
   * Cumulative average rate since the loop started (executedCycles / uptime), in hertz; 0 before the loop starts. Below targetHz means the host cannot sustain the configured period.
   * @format double
   * @example 999.56
   */
  achievedHz: number;
  /**
   * Loop iterations actually executed since start
   * @format int64
   * @example 2023
   */
  executedCycles: number;
  /**
   * Cycles skipped to catch up after overruns/stalls since start. A rising value means the loop is not meeting its period.
   * @format int64
   * @example 0
   */
  skippedCycles: number;
  /**
   * Task-execution time of the most recent cycle, in nanoseconds
   * @format int64
   * @example 104
   */
  lastExecNs: number;
  /**
   * Worst task-execution time since start, in nanoseconds
   * @format int64
   * @example 4009
   */
  maxExecNs: number;
  /**
   * Mean task-execution time since start, in nanoseconds
   * @format int64
   * @example 338
   */
  avgExecNs: number;
  /**
   * Whether SCHED_FIFO real-time priority was acquired (false on Windows, which never attempts it, and on Linux/macOS when the privilege is missing).
   * @example false
   */
  schedFifo: boolean;
  /**
   * Whether mlockall succeeded to pin memory (Linux only; false elsewhere or when the privilege is missing).
   * @example false
   */
  memLocked: boolean;
  /**
   * Core the real-time thread was configured to pin to (gameLoop.cpuAffinity), or -1 when unpinned. Pointing this at an isolcpus core is what gives the RT thread a core to itself; only that thread is pinned, so the HTTP, WebSocket and monitoring threads stay on the remaining cores.
   * @example -1
   */
  cpuAffinity: number;
  /**
   * Whether the pin took. Always false when cpuAffinity is -1 (nothing was asked for); false while cpuAffinity is >= 0 means the request failed and the thread is running unpinned.
   * @example false
   */
  cpuPinned: boolean;
  /**
   * Server wall-clock timestamp (epoch microseconds) when sampled
   * @format int64
   * @example 1783968687738680
   */
  timestampUs: number;
}

/** The user cache's location on disk plus every file it holds. */
export interface UserCacheListing {
  /**
   * Absolute path of the cache directory on the server's filesystem. Platform-dependent (and overridable via the `userCache.directory` config key), so it is reported rather than assumed — show it to the user so they can find the files themselves.
   * @example "/home/user/.cache/motion-master"
   */
  root: string;
  files: UserCacheFile[];
}

/** One file in the user cache. */
export interface UserCacheFile {
  /**
   * Path relative to the cache root, always with `/` separators. Use it verbatim in the download/upload/delete endpoints.
   * @example "configs/machine-a.json"
   */
  path: string;
  /**
   * File size in bytes.
   * @example 20480
   */
  size: number;
  /**
   * Last-write time, in milliseconds since the Unix epoch.
   * @example 1783968687738
   */
  modifiedMs: number;
  /**
   * Whether `DELETE` will accept this path. False for a file the running server holds open — today only its own log file, `logs/motion-master.log`. Deleting such a file cannot do what it appears to (Windows refuses; Linux and macOS unlink it while the server keeps writing to it, reclaiming nothing), so the request is refused with `409 Conflict` and a client should not offer the action. Rotated copies are closed and remain deletable, which is how the space is reclaimed.
   * @example true
   */
  deletable: boolean;
}

/** Summary of one on-disk parameter-cache file (its identity, size, and count). */
export interface ParameterCacheEntry {
  /**
   * Opaque key addressing this cache file in the download/delete endpoints ("<vendor>-<product>-<version>", the version reduced to URL-safe characters). Use it verbatim — do not construct it client-side.
   * @example "000022d2-00000201-v5.6.6"
   */
  id: string;
  /**
   * EtherCAT vendor ID (0x22D2 = Synapticon).
   * @example 8914
   */
  vendorId: number;
  /**
   * Product code.
   * @example 513
   */
  productCode: number;
  /**
   * The device's firmware version (0x100A) the file was written for, in full.
   * @example "v5.6.6"
   */
  firmwareVersion: string;
  /**
   * Number of cached parameter definitions in the file.
   * @example 312
   */
  parameterCount: number;
  /**
   * File size on disk, in bytes.
   * @example 48213
   */
  sizeBytes: number;
}

/** A device's process-data (sync-manager) watchdog configuration, decoded from the watchdog divider (0x0400) and process-data watchdog time (0x0420) ESC registers. */
export interface ProcessDataWatchdog {
  /**
   * 1-based position of the device on the fieldbus.
   * @example 1
   */
  slavePosition: number;
  /**
   * False when the time register (ticks) is 0, i.e. the watchdog is disabled.
   * @example true
   */
  enabled: boolean;
  /**
   * Live status register (0x0440 bit 0): true = the watchdog is counting, false = expired or disabled. Meaningful only when enabled; it counts down only while process data flows.
   * @example true
   */
  running: boolean;
  /**
   * Configured timeout in nanoseconds: ticks × 40 ns × (divider + 2).
   * @example 100000000
   */
  timeoutNs: number;
  /**
   * The same timeout in milliseconds (convenience for display/editing).
   * @format double
   * @example 100
   */
  timeoutMs: number;
  /**
   * Raw watchdog divider register (0x0400); shared with the PDI watchdog.
   * @example 2498
   */
  divider: number;
  /**
   * Raw process-data watchdog time register (0x0420), in watchdog ticks.
   * @example 1000
   */
  ticks: number;
}

/** A monitoring's configuration plus its per-parameter source classification. */
export interface Monitoring {
  /** @example "left-leg" */
  topic: string;
  /**
   * Present only when a label was supplied at creation.
   * @example "Left Leg"
   */
  name?: string;
  /**
   * Flush cadence in milliseconds (5–2000).
   * @example 20
   */
  interval: number;
  /** The sampled objects, in the positional order of each WebSocket sample row. */
  parameters: {
    /** @example 1 */
    devicePosition: number;
    /** @example 24676 */
    index: number;
    /** @example 0 */
    subindex: number;
    /**
     * How the value is sourced — `pdo` (decoded from the live process image) or `sdo` (polled in the background and read from cache).
     * @example "pdo"
     */
    source: "pdo" | "sdo";
  }[];
}

/** One mapping entry to write. Packs to the 32-bit CoE word index<<16 | subindex<<8 | bitLength. */
export interface PdoMappingRequestEntry {
  /**
   * CoE object index; 0 marks an alignment-padding gap (no bound object)
   * @min 0
   * @max 65535
   * @example 24640
   */
  index: number;
  /**
   * CoE object subindex
   * @min 0
   * @max 255
   * @example 0
   */
  subindex: number;
  /**
   * Width of the entry in bits
   * @min 0
   * @max 255
   * @example 16
   */
  bitLength: number;
}

/** One PDO mapping object to write and its ordered entries. */
export interface PdoMappingRequestObject {
  /**
   * Mapping-object index (0x16xx RxPDO for outputs, 0x1Axx TxPDO for inputs)
   * @min 0
   * @max 65535
   * @example 5632
   */
  pdoIndex: number;
  entries: PdoMappingRequestEntry[];
}

/** A device's desired PDO configuration to write. `outputs` are RxPDO objects (master→slave, assigned to 0x1C12); `inputs` are TxPDO objects (slave→master, assigned to 0x1C13). Vector order is the sync-manager assignment order. Both keys are required; an empty array clears that direction. */
export interface PdoMappingRequest {
  outputs: PdoMappingRequestObject[];
  inputs: PdoMappingRequestObject[];
}

/** One entry of a mapping object, as read back from the device. */
export interface PdoMappingEntry {
  /**
   * CoE object index; 0 marks an alignment-padding gap
   * @example 24640
   */
  index: number;
  /**
   * CoE object subindex
   * @example 0
   */
  subindex: number;
  /**
   * Width of the entry in bits
   * @example 16
   */
  bitLength: number;
  /**
   * Bit offset from the start of this direction's window (derived by the device)
   * @example 0
   */
  bitOffset: number;
}

/** One mapping object as read back, with its ordered entries (offsets included). */
export interface PdoMappingObject {
  /**
   * Mapping-object index (0x16xx RxPDO for outputs, 0x1Axx TxPDO for inputs)
   * @example 5632
   */
  pdoIndex: number;
  entries: PdoMappingEntry[];
}

/** A device's PDO mapping grouped by mapping object — the read/write-back shape. `outputs` are RxPDO objects (0x1C12), `inputs` are TxPDO objects (0x1C13), each in sync-manager assignment order with its entries and derived bit offsets. */
export interface PdoMapping {
  /** RxPDO objects (master→slave), in assignment order */
  outputs: PdoMappingObject[];
  /** TxPDO objects (slave→master), in assignment order */
  inputs: PdoMappingObject[];
}

export interface ProcessImageObject {
  /**
   * 1-based bus position of the owning device
   * @example 1
   */
  slavePosition: number;
  /**
   * CoE object index
   * @example 24640
   */
  index: number;
  /**
   * CoE object subindex
   * @example 0
   */
  subindex: number;
  /**
   * Object name resolved from the device's parameter map; empty when the object dictionary has not been enumerated for that device
   * @example "Target position"
   */
  name: string;
  /**
   * Absolute bit offset within the direction's image
   * @example 32
   */
  bitOffset: number;
  /**
   * Width of the value in bits
   * @example 32
   */
  bitLength: number;
}

/** One step of a procedure. `id` is stable across runs, so a client keys its label and its value formatting off it. A step never reports cancellation: cancelling stops the procedure, leaving the step it was on `running` and the rest `idle`, which records how far it got. */
export interface ProgressStep {
  /**
   * Stable identifier of the step within its procedure.
   * @example "command"
   */
  id: string;
  /** @example "succeeded" */
  status: "idle" | "running" | "succeeded" | "failed";
  /**
   * What the step produced, if anything — shape is per procedure. Absent when the step produced nothing. For `os-command` it is an object with `status`, `data`, `errorCode` when the drive sent one, and `fsBuffer` when the request asked to read bulk data. For `read-object-dictionary` it is an object with `byteCount` and `values`, one entry per object with its `index`, `subindex` and `value`. For `ic-mu-encoder-status` the `read-registers` step holds an array of `IcMuEncoderStatus`, and for `ic-mu-encoder-configuration` the `verify` step holds one. For `ic-mu-calibration` the `validate` step holds an `IcMuCalibrationPlan`, each `iteration-<n>` step an `IcMuCalibrationIteration`, and the `validation` step an `IcMuCalibrationValidation`. For `ic-mu-key-registers` the `read-key-registers` step holds an array of `IcMuKeyRegister`, for `ic-mu-eeprom-export` the `read-eeprom` step an array of `IcMuEepromByte`, and for `ic-mu-i2c-register` the `i2c-transfer` step an `IcMuI2cTransfer`.
   * @example {"status":1,"data":[1,2,3,4,5,6]}
   */
  value?: any;
  /** Why the step failed. Absent unless it did. */
  error?: string;
}

/** The complete state of a procedure on one device — an accumulating snapshot rather than an event, which is what makes polling lossless. */
export interface ProcedureSnapshot {
  /**
   * The one field a polling loop checks. `cancelled` is distinct from `failed` on purpose: "I stopped it" and "the drive could not do it" are different outcomes.
   * @example "succeeded"
   */
  status: "idle" | "running" | "succeeded" | "failed" | "cancelled";
  /**
   * How many runs have been accepted on this device since the last scan or reset; a rejected start does not count. Doubles as a generation counter — with no run id, a changed `runCount` is how a poller can tell one run from the next.
   * @example 3
   */
  runCount: number;
  /**
   * Epoch milliseconds the run began. Absent when never run.
   * @format int64
   * @example 1735821000123
   */
  startedAt?: number;
  /**
   * Epoch milliseconds the run ended. Absent while running.
   * @format int64
   * @example 1735821042456
   */
  finishedAt?: number;
  /** The procedure's ordered steps, always the full template. */
  steps: ProgressStep[];
  /** Why the run failed when no step captured it — a failure belonging to no step, such as the device not being the kind the procedure needs. Absent otherwise. */
  error?: string;
}

/** One file stored on a device, as its file list reports it. */
export interface DeviceFile {
  /**
   * Filename as FoE addresses it.
   * @example "hr_data0.bin"
   */
  name: string;
  /**
   * Size the device reported. Absent when it reported the name without one — the size is what the listing claims, not what a read returned.
   * @example 8032
   */
  byteCount?: number;
}

/** One high resolution data recording, read back from a drive's files and decoded. */
export interface HrdRecording {
  /**
   * Which layout the files were decoded as — echoes the request.
   * @example "encoder-raw"
   */
  data: "encoder-raw" | "system-identification";
  /** The files it was read from, in order. A recording that came back shorter than expected can be told from one that was never made by what is listed here. */
  files: DeviceFile[];
  /**
   * Total bytes read across those files.
   * @example 20000
   */
  byteCount: number;
  /**
   * Bytes past the last whole sample. The drive allocates its files in fixed-size blocks, so this is the last block's padding rather than data that failed to decode.
   * @example 0
   */
  trailingBytes: number;
  /**
   * How many samples were decoded.
   * @example 5000
   */
  sampleCount: number;
  /**
   * What each value of a row is, in order. For `encoder-raw`: `raw` (the 32-bit word the encoder reported), `masterCount` (its low 14 bits) and `noniusCount` (the next 14) — the raw word is kept because the firmware specification describes only bits 0-27 and says nothing about the top four. For `system-identification`: `velocityRpm` (converted out of the file's Q15 fixed point) and `torquePermil` (per mille of rated torque).
   * @example ["raw","masterCount","noniusCount"]
   */
  columns: string[];
  /**
   * One row per sample, positional in the order `columns` gives — a full recording is up to ten thousand rows, so the names travel once beside them rather than on every row.
   * @example [[4661,4661,0],[4662,4662,0]]
   */
  samples: number[][];
}

/** One encoder configuration record, 0x2110 for encoder 1 or 0x2112 for encoder 2. */
export interface EncoderConfiguration {
  /**
   * The encoder ordinal, which the controller fields and the OS commands refer to; 1 is 0x2110, 2 is 0x2112.
   * @example 1
   */
  encoderOrdinal: 1 | 2;
  /**
   * The port the encoder is wired to (:01), 0 for none.
   * @example 1
   */
  sensorPort: number;
  /**
   * The kind of encoder (:02). `unknown` for a value this version does not name.
   * @example "biss"
   */
  type:
    | "none"
    | "hallSensor"
    | "incremental"
    | "nikonAFormat"
    | "biss"
    | "internal"
    | "ssi"
    | "sinCosModule"
    | "unknown";
  /**
   * Counts per revolution (:03). One revolution of this encoder in position counts.
   * @example 262144
   */
  resolution: number;
  /**
   * Whether the counting direction is inverted (:05).
   * @example false
   */
  inverted: boolean;
  /**
   * Bits of the turn counter in the encoder's frame (:0A).
   * @example 0
   */
  multiturnBits: number;
  /**
   * Bits of the position within one turn in the encoder's frame (:0C).
   * @example 18
   */
  singleturnBits: number;
}

/** Both encoder records of a SOMANET drive and which of them each controller uses. The three controller fields hold an ordinal of the `encoders` list, or `null` when the controller names no encoder. */
export interface Encoders {
  encoders: EncoderConfiguration[];
  /**
   * The encoder position control uses (0x2012:09).
   * @example 1
   */
  position: 1 | 2 | null;
  /**
   * The encoder velocity control uses (0x2011:05).
   * @example 1
   */
  velocity: 1 | 2 | null;
  /**
   * The encoder commutation uses (0x2010:0C).
   * @example 1
   */
  commutation: 1 | 2 | null;
}

/** A drive's brake configuration and current state — the parts of object 0x2004 that decide what commanding the brake will actually do. Returned by every brake endpoint, so the outcome of a release or engage arrives in the same shape as a plain read. */
export interface BrakeState {
  /**
   * Brake state (0x2004:07). A brake is spring-engaged, so `engaged` is also its powered-off state.
   * @example "engaged"
   */
  status: "notConfigured" | "engaged" | "disengaged";
  /**
   * How the brake is driven (0x2004:04). `manualOutputVoltage` means the firmware does not drive it — the brake is a raw output voltage (0x2004:10) instead.
   * @example "clutch"
   */
  releaseStrategy: "manualOutputVoltage" | "clutch" | "pin";
  /**
   * How long the pull voltage is applied to release the brake (0x2004:03). The firmware blocks motion, and motion-related OS commands, until this expires.
   * @example 120
   */
  pullTimeMs: number;
  /**
   * Voltage that disengages the brake, in millivolts (0x2004:01).
   * @example 24000
   */
  pullVoltageMv: number;
  /**
   * Lower voltage that keeps it disengaged once released, in millivolts (0x2004:02).
   * @example 12000
   */
  holdVoltageMv: number;
  /**
   * Whether commanding the brake does anything at all — false for `manualOutputVoltage`. Derived from the release strategy and served so a client need not encode that rule.
   * @example true
   */
  softwareControllable: boolean;
  /**
   * Whether releasing this brake turns the motor. True only for a pin brake, whose release raises current progressively to lift the load off the pin.
   * @example false
   */
  releaseMovesShaft: boolean;
}

/** What a procedure *is*, independent of any run — everything needed to render a control for it. The text is served rather than held per client, because the house rule that every action carries a description and its caveats means it has to exist somewhere, and duplicating it in each client is how it goes stale. */
export interface ProcedureDescriptor {
  /**
   * Identifier: the `procedureName` path segment, and the key its snapshot is retained under.
   * @example "os-command"
   */
  name: string;
  /**
   * Short human-readable name.
   * @example "OS command"
   */
  title: string;
  /** What the procedure does. */
  description: string;
  /** What a user must know before running it. May be empty. */
  caveats: string[];
  /**
   * True if running it can move the shaft.
   * @example true
   */
  movesMotor: boolean;
  /**
   * True if the drive must be enabled before it will run.
   * @example false
   */
  requiresEnabled: boolean;
  /** What the request body may carry, in the order a client should present it. Empty for a procedure that takes none, which is most of them — a procedure's timings are properties of the command it issues rather than a caller's choice. */
  parameters: ProcedureParameter[];
  /**
   * The ordered step ids the procedure reports against — bare ids, because a template's per-step status is always idle and says nothing. Live status for the same ids is in the snapshot's `steps`.
   * @example ["command"]
   */
  steps: string[];
}

/** One choice of an `enum` parameter. */
export interface ParameterOption {
  /**
   * What the request body carries when this option is chosen.
   * @example 1
   */
  value: any;
  /**
   * What the user picks.
   * @example "Encoder 1"
   */
  title: string;
}

/** One named parameter a procedure accepts — enough to render a field for it and to know what to put in. Descriptive rather than authoritative: the server validates every request whatever a client sends, and this is what lets a client build a sensible form and catch a mistake before the request goes out. The three type-specific fields are absent where they do not apply. */
export interface ProcedureParameter {
  /**
   * The key in the request body.
   * @example "registerAddress"
   */
  name: string;
  /**
   * Short label.
   * @example "Register address"
   */
  title: string;
  /** What it does and what a sensible value is. */
  description: string;
  /**
   * Which kind of control to render. `file` carries a whole file as a base64 string, so a client renders a file picker and encodes what it reads; `stringArray` is an editable list of free-text values.
   * @example "integer"
   */
  type:
    | "integer"
    | "boolean"
    | "enum"
    | "byteArray"
    | "string"
    | "stringArray"
    | "file";
  /**
   * Whether a request must carry it — true exactly when there is no `defaultValue`. Reported rather than left to be derived, so a client need not know the rule.
   * @example true
   */
  required: boolean;
  /**
   * What an omitting request gets. Absent when the parameter is required.
   * @example 1
   */
  defaultValue?: any;
  /**
   * `integer` only: smallest accepted value.
   * @example 0
   */
  minValue?: number;
  /**
   * `integer` only: largest accepted value.
   * @example 255
   */
  maxValue?: number;
  /**
   * `byteArray` only: the exact number of bytes.
   * @example 8
   */
  length?: number;
  /** `enum` only: the values that may be chosen. */
  options?: ParameterOption[];
}

/** A SOMANET firmware package filename broken into its fields. The four decoded-descriptor properties are absent — not null — when the full firmware descriptor is not the numeric kind, which the naming specification explicitly allows. */
export interface FirmwarePackageName {
  /**
   * The format description; always `package` for a firmware bundle.
   * @example "package"
   */
  description: string;
  /**
   * Human-readable hardware name.
   * @example "SOMANET-Circulo-7"
   */
  hardwareName: string;
  /**
   * The hardware this package is built for, verbatim — the string a compatibility check compares.
   * @example "8500-04-2332"
   */
  fullFirmwareDescriptor: string;
  /**
   * Firmware or software name.
   * @example "motion-drive"
   */
  softwareName: string;
  /**
   * The firmware or software version, including the leading `v`. Not to be confused with `firmwareVersion`, which is the *hardware* revision inside the descriptor.
   * @example "v5.6.10"
   */
  softwareVersion: string;
  /**
   * Firmware id from the descriptor — the hardware product.
   * @example "8500"
   */
  firmwareId?: string;
  /**
   * Hardware revision from the descriptor. A string, so a leading zero survives: `04` read as a number and printed back would be `4`, and `8500-4` matches no package.
   * @example "04"
   */
  firmwareVersion?: string;
  /**
   * Firmware encryption key id from the descriptor. A string because real packages carry non-numeric ones, e.g. `A`.
   * @example "2332"
   */
  keyId?: string;
  /**
   * Fieldbus protocol character from the descriptor, hexadecimal. `1` is EtherCAT.
   * @example "1"
   */
  fieldbusProtocol?: string;
  /**
   * `firmwareId` and `firmwareVersion` joined — the descriptor without the key or fieldbus. Useful for saying *how* a package missed: a matching build descriptor with a different key means right hardware, wrong encryption key.
   * @example "8500-04"
   */
  buildDescriptor?: string;
}

/** One item making up a device or assembly. Carries no function beyond recording what has a serial number, so every property can legitimately be empty. */
export interface HardwareComponent {
  /** @example "Com EtherCAT" */
  name: string;
  /** @example "A.2" */
  version: string;
  /** @example "1000-02-0005739-1815" */
  serialNumber: string;
}

/** A device or an assembly. The two are one shape — the specification lists their fields in a single table and they differ only in that `keyId` and `macAddress` are device-only. */
export interface HardwareProduct {
  /**
   * Human-readable description of the hardware.
   * @example "SOMANET Servo Node 400 EtherCAT"
   */
  name: string;
  /**
   * Names the product's picture. Empty when the file carries none.
   * @example "9501-xx"
   */
  imageId: string;
  /**
   * The `firmwareId` — identifies the hardware product.
   * @example "9501"
   */
  id: string;
  /**
   * The `firmwareVersion` — revision of that product.
   * @example "01"
   */
  version: string;
  /**
   * Firmware encryption key burned to OTP memory. Device only, and empty on hardware whose firmware predates encryption.
   * @example "4622"
   */
  keyId: string;
  /** @example "9501-01-040595-4023" */
  serialNumber: string;
  /**
   * MAC address of the Ethernet port. Device only.
   * @example "01-23-45-67-89-AB"
   */
  macAddress: string;
  components: HardwareComponent[];
  /**
   * `id` and `version` joined by a dash.
   * @example "9501-01"
   */
  buildDescriptor: string;
}

/**
 * A device's `.hardware_description` file — what the hardware says it is, and the source of the descriptor that decides which firmware belongs on it.
 *
 * `device` is never absent, and that is structural rather than a simplification: the file lives *on* the device, so a file without one cannot exist. `assembly` is present only when the device was packaged into a product — an ACTILINK is an assembly with a SOMANET Node device inside it — and when it is, it is what firmware is matched against first.
 */
export interface HardwareDescription {
  /**
   * Format version of the file itself, semver.
   * @example "1.2.0"
   */
  fileVersion: string;
  /** A device or an assembly. The two are one shape — the specification lists their fields in a single table and they differ only in that `keyId` and `macAddress` are device-only. */
  device: HardwareProduct;
  /** A device or an assembly. The two are one shape — the specification lists their fields in a single table and they differ only in that `keyId` and `macAddress` are device-only. */
  assembly?: HardwareProduct;
}

/** One entry of the Integro runtime-variant option catalogue. Nothing on the server acts on `socVariables` or `incompatibleOptionIds` — the drive enforces them — but they are what make a decoded file legible. */
export interface IntegroVariantOption {
  /**
   * The option code as it appears in the file.
   * @example 1
   */
  id: number;
  /**
   * What the option selects.
   * @example "Fieldbus Protocol"
   */
  category: string;
  /**
   * Which choice within that category.
   * @example "EtherCAT"
   */
  meaning: string;
  /**
   * The drive's own configuration values this option sets. Empty for an option the fieldbus firmware acts on rather than the SoC.
   * @example ["adc_phase_current_sensor_gain"]
   */
  socVariables: string[];
  /**
   * Options that cannot be selected alongside this one — the other members of a category that selects exactly one choice. Empty for an independent flag such as "disable DI1".
   * @example [0,2,3,39]
   */
  incompatibleOptionIds: number[];
  /**
   * Segments of the part number this option corresponds to. Empty for an option not distinguished in the part number.
   * @example ["EC"]
   */
  mpnSegmentCodes: string[];
}

/** One option code from a decoded file, expanded into its catalogue entry. Everything but `id` is absent for a code the catalogue does not name, so "we do not know what this is" is missing properties rather than the string `unknown`. */
export type IntegroVariantFileOption = UtilRequiredKeys<
  IntegroVariantOption,
  "id"
>;

/**
 * A plant model as plant identification stored it on the drive: the transfer function from torque in Nm to velocity in rad/s, coefficients highest order first and normalised so that the numerator's last coefficient is 1, and the chirp it was identified with. The last two denominator coefficients are the effective inertia and the effective viscous friction of the reduced model, which is how auto-tuning reads them.
 * @example {"numerator":[1],"denominator":[0.000123,0.0456],"inertia":0.000123,"friction":0.0456,"chirp":{"startFrequencyHz":1,"endFrequencyHz":100,"amplitudePermil":300,"durationMs":3000,"signalType":"logarithmic"}}
 */
export interface IdentifiedPlantModel {
  numerator: number[];
  denominator: number[];
  /**
   * The denominator coefficient before the last; null when there are fewer than two
   * @format double
   */
  inertia: number | null;
  /**
   * The last denominator coefficient; null when the denominator is empty
   * @format double
   */
  friction: number | null;
  /** What excited the plant. A file written without a chirp row reads back with zeros and the logarithmic type. */
  chirp: {
    /** @format double */
    startFrequencyHz: number;
    /** @format double */
    endFrequencyHz: number;
    /** Peak excitation, in per mille of rated torque */
    amplitudePermil: number;
    /** How long the sweep took */
    durationMs: number;
    signalType: "logarithmic" | "linear";
  };
}

/**
 * A decoded `.variant` file: which features a SOMANET Integro was licensed with.
 *
 * Only Integro drives carry one. Its layout comes from the firmware that reads it rather than from a specification — there is none — and the one thing on it that matters beyond inspection is `fieldbusProtocol`, which the hardware description deliberately does not carry and a full firmware descriptor ends with.
 */
export interface IntegroVariant {
  /**
   * Format version; 1 in every file seen so far.
   * @example 1
   */
  fileVersion: number;
  /** Lowercase hex. Opaque — verification needs the private key and happens on the drive — and reported only so a caller can see whether one is present. */
  signature: string;
  /**
   * Lowercase hex netX chip id this file was issued for.
   * @example "4e64646c70b286a000000000"
   */
  chipId: string;
  /** @example "9004-02-0000424-2445" */
  serialNumber: string;
  /** @example "40:49:8A:01:21:D6" */
  macAddress: string;
  /**
   * Who the device was licensed to.
   * @example 1
   */
  customerId: number;
  /**
   * Raw mode value. `passive` (0x51) is the failure state as much as a mode: firmware falls back to it when the file is missing, its signature does not verify, or its chip id belongs to another device.
   * @example 87
   */
  operationMode: number;
  /** @example "live" */
  operationModeName: "passive" | "trial" | "production" | "live" | "unknown";
  /**
   * The fieldbus the device is licensed for, for the tail of a full firmware descriptor. **EtherCAT wins when several are selected** — real files do carry more than one. Absent when the file selects no fieldbus at all.
   * @example 1
   */
  fieldbusProtocol?: number;
  /** The option codes, in the order the file lists them — which is not sorted, and is the only record of how the file was written. */
  options: IntegroVariantFileOption[];
}

/** The full firmware descriptors one device accepts firmware under. Two rather than one because the specification says both are compatible and lists both for the user to choose from: the assembly's firmware "should have been customized for the assembly", so it is the one to prefer, while the device package is the generic build for the hardware inside it. */
export interface FullFirmwareDescriptors {
  /**
   * The device's own descriptor. Always present.
   * @example "9010-02-2423-1"
   */
  device: string;
  /**
   * The assembly's build descriptor with the **device's** key id — assemblies have no key of their own. Absent when the hardware description carries no assembly.
   * @example "6000-01-2423-1"
   */
  assembly?: string;
}

/**
 * Whether one firmware package belongs on one device.
 *
 * **The whole descriptor string is compared, never its decoded parts.** That is what makes the check correct for descriptors outside the numeric convention, and it is how the specification frames the operation — find compatible firmware by searching for the `fullFirmwareDescriptor` string.
 */
export interface FirmwareCompatibility {
  /** @example true */
  compatible: boolean;
  /**
   * Which of the device's descriptors matched. `device` is a match that still has something to say: it is the generic build rather than the one customised for the assembly.
   * @example "assembly"
   */
  match: "none" | "assembly" | "device";
  /** A SOMANET firmware package filename broken into its fields. The four decoded-descriptor properties are absent — not null — when the full firmware descriptor is not the numeric kind, which the naming specification explicitly allows. */
  packageName: FirmwarePackageName;
  /** The full firmware descriptors one device accepts firmware under. Two rather than one because the specification says both are compatible and lists both for the user to choose from: the assembly's firmware "should have been customized for the assembly", so it is the one to prefer, while the device package is the generic build for the hardware inside it. */
  deviceDescriptors: FullFirmwareDescriptors;
  /**
   * One sentence naming the descriptors involved, ready to show a user. Populated for a match as well as a mismatch.
   * @example "This package is built for 6000-01-2423-1, which is this device's assembly."
   */
  explanation: string;
}

/** One procedure paired with how its last run on this device went — an entry of `GET /api/devices/{slavePosition}/procedures`. The pairing is what lets a page render in a single request instead of one per procedure. */
export interface ProcedureListing {
  /** What a procedure *is*, independent of any run — everything needed to render a control for it. The text is served rather than held per client, because the house rule that every action carries a description and its caveats means it has to exist somewhere, and duplicating it in each client is how it goes stale. */
  descriptor: ProcedureDescriptor;
  /** The complete state of a procedure on one device — an accumulating snapshot rather than an event, which is what makes polling lossless. */
  snapshot: ProcedureSnapshot;
}

export interface IcHausRegister {
  /**
   * The address, or the first of a printed range.
   * @example 15
   */
  address: number;
  /**
   * The last address of the range; equal to address for a single-address row.
   * @example 15
   */
  lastAddress: number;
  /**
   * The fields rendered as the datasheet prints the row. "RESERVED" for a reserved row.
   * @example "SPO_MT(3:0) | MPC(3:0)"
   */
  layout: string;
  /** The named pieces of this register, most significant bit first. Empty when reserved. */
  fields: {
    /**
     * The datasheet's own name for the field.
     * @example "SPO_MT"
     */
    name: string;
    /**
     * The field's own bit slice as the register map prints it, empty where it prints a bare name. Not the position within the byte.
     * @example "3:0"
     */
    bits: string;
    /**
     * The datasheet's one-line description of the field.
     * @example "Offset external multiturn"
     */
    description: string;
  }[];
  /** @example false */
  reserved: boolean;
  /**
   * Reachable over SPI only, so the register communication service cannot touch it.
   * @example false
   */
  spiOnly: boolean;
}

/** Whether one iC-MU encoder holds the output configuration that its encoder configuration object asks for. */
export interface IcMuEncoderStatus {
  /** @example 1 */
  encoderOrdinal: 1 | 2;
  /**
   * 0x21x0:01. 3 to 5 is an external encoder.
   * @example 1
   */
  sensorPort: number;
  /**
   * 0x21x0:0C.
   * @example 19
   */
  singleturnBits: number;
  /**
   * 0x21x0:0A, 0 where the object has none.
   * @example 0
   */
  multiturnBits: number;
  /**
   * The pole pairs of the ring, null for a ring the rules do not know.
   * @example 32
   */
  masterPolePairs: number | null;
  /**
   * iC-MU register 0x74.
   * @example 21
   */
  hardRev: number;
  /**
   * The part register 0x74 names, null for a code no iC-MU uses.
   * @example "iC-MU150"
   */
  part: string | null;
  /**
   * The low nibble of register 0x74.
   * @example 5
   */
  revision: number;
  /**
   * iC-MU register 0x11.
   * @example 5
   */
  outputRegister: number;
  /**
   * The value of register 0x11 the rules give, null where they do not apply.
   * @example 5
   */
  expectedOutputRegister: number | null;
  /**
   * not-configured when register 0x11 holds 0xA5, configured when it holds the expected value, invalid otherwise. null without an expected value, and for another chip on an external port.
   * @example "configured"
   */
  state: "not-configured" | "invalid" | "configured" | null;
  /** Why there is no expected value. */
  reason: string | null;
  /** @example "singleturn" */
  path: "singleturn" | "multiturn";
  /** The battery modes a configuration request can name besides default. Empty on the singleturn path and on a ring that offers no mode. */
  batteryModes: {
    /** @example "18-microamperes" */
    name: string;
    /** @example 18 */
    maxCurrentMicroamps: number;
    /** @example 56 */
    maxAccelerationKrpmPerSecond: number;
  }[];
  /**
   * The encoder status word, 0x2111:04 for encoder 1 and 0x2113:04 for encoder 2, read with an SDO upload. It is a snapshot, not a live view of the chips. The drive writes it after a BiSS error bit and at start-up: iC-MU STATUS1 in bits 0-7, the iC-PVL status in bits 8-15 and iC-MU STATUS0 in bits 16-23, each with only the bits the drive watches. 0 means that nothing was latched since the drive started. The drive sets the word to 0 again only when a change of the encoder configuration object, 0x2110 or 0x2112, restarts the encoder; `ic-mu-encoder-configuration` does not clear it. The chips themselves are not read, because a read of STATUS1 clears the iC-MU errors.
   * @format int64
   * @example 4096
   */
  encoderStatus: number;
  /** One entry for each set bit of encoderStatus, lowest bit first. */
  encoderStatusBits: IcMuStatusBit[];
}

/** One key register of an iC-MU, read from its RAM: the step value of `ic-mu-key-registers` is a list of 33 of them, in the order of the restore file. */
export interface IcMuKeyRegister {
  /** @example 82 */
  ramAddress: number;
  /**
   * Where the EEPROM keeps the register. RAM 0x00-0x18 is EEPROM 0x00-0x18, and RAM 0x52-0x59 is EEPROM 0x19-0x20.
   * @example 25
   */
  eepromAddress: number;
  /** @example 47 */
  value: number;
}

/** One byte of the EEPROM of an iC-MU encoder: the step value of `ic-mu-eeprom-export` is a list of 77 of them, 0x00-0x4C. 0x00-0x3F is the configuration of the iC-MU and 0x40-0x4C that of the iC-PVL. */
export interface IcMuEepromByte {
  /** @example 64 */
  address: number;
  /** @example 40 */
  value: number;
}

/** The step value of `ic-mu-i2c-register`. */
export interface IcMuI2CTransfer {
  /** @example "ic-pvl" */
  target: "eeprom" | "ic-pvl" | "ic-pvl-status";
  /**
   * The I²C device id with the R/W bit, as written to I2C_DEVID (0x5E).
   * @example 195
   */
  deviceId: number;
  /** @example false */
  write: boolean;
  /** @example 16 */
  registerAddress: number;
  /**
   * The byte read or written.
   * @example 240
   */
  value: number;
}

/** One set bit of the encoder status word. A set bit that the drive masks out has no name, title, description or remedy. */
export interface IcMuStatusBit {
  /**
   * The bit in the encoder status word.
   * @example 12
   */
  bit: number;
  /**
   * The chip the bit comes from, null for bits 24-31.
   * @example "iC-PVL"
   */
  chip: "iC-MU" | "iC-PVL" | null;
  /**
   * The status register of the chip, null for bits 24-31.
   * @example "STATUS"
   */
  register: "STATUS0" | "STATUS1" | "STATUS" | null;
  /**
   * The bit in that register.
   * @example 4
   */
  registerBit: number;
  /**
   * The datasheet name.
   * @example "BAT_ERR"
   */
  name: string | null;
  /** @example "Battery error" */
  title: string | null;
  /** @example "The battery voltage is too low (iC-PVL datasheet, Elec. Char. No. 404). The position is not valid." */
  description: string | null;
  /**
   * What to do, null where there is nothing to do.
   * @example "Change the battery, then reset the fault of the drive."
   */
  remedy: string | null;
}

/** What an iC-MU calibration decided before the motor moved. */
export interface IcMuCalibrationPlan {
  /** @example 2 */
  encoderOrdinal: 1 | 2;
  /** @example 2 */
  sensorPort: number;
  /** @example "outer" */
  ringPosition: "inner" | "outer";
  /** @example "iC-MU200" */
  part: string;
  /** @example 32 */
  masterPolePairs: number;
  /** @example "motor" */
  shaft: "motor" | "driving";
  /** False when the start values of another ring are used. */
  startValuesMeasured: boolean;
  /**
   * On the driving shaft.
   * @example 0.75
   */
  velocityRpm: number;
  /**
   * On the driving shaft.
   * @example 8.33
   */
  accelerationRpmPerSecond: number;
  /**
   * The time at constant velocity.
   * @example 6000
   */
  recordingMs: number;
  /** Whether the software position limits bound the motion. */
  limitedRange: boolean;
  warnings: string[];
}

/** The error that remains on one track after a recording, in LSB of its registers. */
export interface IcMuTrackResidual {
  sineOffset: number;
  cosineOffset: number;
  gain: number;
  phase: number;
  phaseRange: 0 | 1;
  /** Whether the track passed the test of this iteration. */
  passed: boolean;
}

/** A curve over the ring: at most 720 points of [ring angle in degrees, value]. The value is in the unit of phaseRangeLimit. */
export type IcMuCalibrationCurve = number[][];

/** What one calibration iteration recorded and wrote. */
export interface IcMuCalibrationIteration {
  /** @example 1 */
  iteration: number;
  direction: 1 | -1;
  /** @example 0.75 */
  velocityRpm: number;
  /**
   * The samples analysed.
   * @example 6000
   */
  samples: number;
  /**
   * Turns of the ring the recording covered.
   * @example 8.9
   */
  recordedRevolutions: number;
  /** Present when the recording covered less than one turn of the ring. */
  warning?: string;
  /** The permissible nonius phase error. */
  phaseRangeLimit: number;
  /** The tolerance of the pass test */
  tolerance: 1 | 3 | 5;
  /** The error that remains on one track after a recording, in LSB of its registers. */
  master: IcMuTrackResidual;
  /** The error that remains on one track after a recording, in LSB of its registers. */
  nonius: IcMuTrackResidual;
  /** A curve over the ring: at most 720 points of [ring angle in degrees, value]. The value is in the unit of phaseRangeLimit. */
  phaseError: IcMuCalibrationCurve;
}

/** The validation pass of an iC-MU calibration: the nonius phase margins the encoder now holds. It passes when both are at least 10 %. */
export interface IcMuCalibrationValidation {
  /** @example 3 */
  iteration: number;
  direction: 1 | -1;
  /** @example 0.75 */
  velocityRpm: number;
  /** @example 6000 */
  samples: number;
  /** @example 8.9 */
  recordedRevolutions: number;
  warning?: string;
  phaseRangeLimit: number;
  /** A curve over the ring: at most 720 points of [ring angle in degrees, value]. The value is in the unit of phaseRangeLimit. */
  phaseMargin: IcMuCalibrationCurve;
  /** @example 78.5 */
  lowerMarginPercent: number;
  /** @example 75 */
  upperMarginPercent: number;
  /**
   * The smaller of the two margins.
   * @example 75
   */
  marginPercent: number;
  /** robust from 50 %, small-displacements from 30 %, avoid-displacements from 10 %, and too-high below 10 %, where the validation fails. */
  marginBand:
    | "robust"
    | "small-displacements"
    | "avoid-displacements"
    | "too-high";
  /** What the band means for the user. */
  marginMessage: string;
  passed: boolean;
  /** Present after a full calibration of the commutation encoder: the commutation offset must be measured again. */
  commutationWarning?: string;
}

/** A decoded iC-MU restore file, `circulo_integrated_encoder_<sensor port>_config.bin`. The drive writes these pairs into the encoder when the iC-MU reports a corrupted EEPROM. */
export interface IcMuRestoreFile {
  /** One entry for each pair, in the order of the file. */
  entries: {
    /**
     * The iC-MU RAM address the drive writes the value into.
     * @min 0
     * @max 255
     * @example 12
     */
    address: number;
    /**
     * @min 0
     * @max 255
     * @example 108
     */
    value: number;
    /** The iC-MU register at the address, as `/api/meta/ic-haus-registers` lists it, or null for an address the register map does not list. */
    register: IcHausRegister | null;
  }[];
}

/** A procedure's parameters. Which fields apply is per procedure, and each descriptor reports its own in `parameters` — so this schema is open, and a procedure that takes none may be started with no body at all. The properties below are the union of every procedure's, each naming the procedure it belongs to. */
export interface ProcedureRequest {
  /**
   * `os-command` only, and required for it: the eight request bytes. Byte 0 is the OS command ID; bytes 1-7 are that command's parameters.
   * @maxItems 8
   * @minItems 8
   * @example [8,0,0,0,0,0,0,0]
   */
  command?: number[];
  /**
   * Ceiling on the whole command. Not a liveness check — the drive fails a command no service acknowledges within 5 s on its own — so size it for the command being run. Hitting it aborts the command on the drive.
   * @min 1
   * @max 600000
   * @default 1000
   * @example 30000
   */
  timeoutMs?: number;
  /**
   * `os-command` only: how often the server reads the drive's response object while waiting.
   * @min 1
   * @max 1000
   * @default 10
   */
  pollIntervalMs?: number;
  /**
   * `os-command` only: whether the command moves bulk data through the drive's fs-buffer, and which way. Most commands move none. A write runs while the command runs, because the command reads its input as it arrives, and it needs firmware 5.2 or newer. A read runs after the command succeeds, because the command completes when its whole output is in the fs-buffer. No read runs after a failure. An output larger than the 2024-byte fs-buffer cannot be read: the command waits for the master to empty it and runs into its timeout. Setting a direction for a command that moves no bulk data makes the transfer wait for an answer the drive never sends.
   * @default "none"
   * @example "read"
   */
  fsBuffer?: "none" | "read" | "write";
  /** `os-command` only, and required when `fsBuffer` is `write`: the bytes to send, base64-encoded into this JSON string. Rejected for the other two directions rather than ignored, so a request that names one and forgets the other is told rather than running a command that moves nothing. (Deliberately not `format: byte`, for the reason spelled out on `packageContent`.) */
  fsBufferData?: string;
  /**
   * `encoder-register-communication`, `ic-mu-calibration-mode`, `ignore-biss-status-bits`, `ic-mu-encoder-configuration`, `ic-mu-calibration`, `ic-mu-key-registers`, `ic-mu-eeprom-export` and `ic-mu-i2c-register`: the encoder ordinal of the OS commands, which picks the drive's encoder to address. Encoder 1 is the encoder configured in 0x2110 and encoder 2 the one configured in 0x2112, so the ordinal picks a configuration object rather than a kind of encoder. Required for every `ic-mu-` procedure that takes it, because a default would address an encoder that nobody named.
   * @default 1
   * @example 1
   */
  encoderOrdinal?: 1 | 2;
  /**
   * `encoder-register-communication` and `ic-mu-i2c-register`: false reads, true writes `value`. `encoder-register-communication` reports what the register holds afterwards; `ic-mu-i2c-register` reports the byte it wrote.
   * @default false
   */
  write?: boolean;
  /**
   * `encoder-register-communication` and `ic-mu-i2c-register`, and required for both: the register to access. For `encoder-register-communication` the map belongs to the encoder chip rather than to the drive's firmware. For `ic-mu-i2c-register` it is an address on `target`: an EEPROM address, or an iC-PVL register.
   * @min 0
   * @max 255
   * @example 117
   */
  registerAddress?: number;
  /**
   * `ic-mu-i2c-register` only, and required for it: what the I²C transfer of the iC-MU addresses. `eeprom` is the EEPROM that holds the configuration of the iC-MU and the iC-PVL, `ic-pvl` every iC-PVL register, and `ic-pvl-status` only the status and command registers of the iC-PVL, where the command register is 0x01. The server picks the I²C device id from `target` and `write`.
   * @example "eeprom"
   */
  target?: "eeprom" | "ic-pvl" | "ic-pvl-status";
  /**
   * `encoder-register-communication` and `ic-mu-i2c-register`: the byte to write. Ignored when reading. `ic-mu-i2c-register` requires it for a write.
   * @min 0
   * @max 255
   * @default 0
   * @example 7
   */
  value?: number;
  /**
   * `hrd-streaming` only, and required for it: which signal to record. Also decides how the recording decodes when it is read back from `/api/devices/{slavePosition}/hrd`.
   * @example "encoder-raw"
   */
  data?: "encoder-raw" | "system-identification";
  /**
   * `hrd-streaming` only, and required for it: how long to record. One sample is written every millisecond into at most five 8032-byte files, so the real ceiling depends on the sample size — 10000 ms for `encoder-raw`, but only 6000 ms for `system-identification`. The drive applies both limits; the narrower one is checked here too, so an over-long request is rejected before a run starts rather than failing on its first step.
   * @min 1
   * @max 10000
   * @example 5000
   */
  durationMs?: number;
  /**
   * `ic-mu-calibration-mode` only, and required for it: how the BiSS service should clock the encoder. `standard` is normal operation; `configuration` keeps the encoder clocked but uses only the register-communication bits, so position stops updating and no CRC fault is raised; `raw` clocks an encoder already configured for raw output and averages that data into 0x2704. There is no default — the mode is the instruction.
   * @example "configuration"
   */
  mode?: "configuration" | "raw" | "standard";
  /**
   * `ic-mu-encoder-configuration` only, and only on the multiturn path: the current the iC-PVL draws from the battery while the drive has no power. A higher current lets it count revolutions at a higher acceleration. `default` takes iC-PVL code 0x5D for an encoder on the motor shaft and 0x68 for one on the driving shaft, and refuses an encoder that serves no controller. A ring of 16 master pole pairs takes only `default`. The `ic-mu-encoder-status` result lists the modes of each encoder with the acceleration each one allows.
   * @default "default"
   * @example "18-microamperes"
   */
  batteryMode?:
    | "default"
    | "6-microamperes"
    | "10-microamperes"
    | "18-microamperes"
    | "36-microamperes"
    | "72-microamperes";
  /**
   * `ic-mu-encoder-configuration` only: writes the configured registers to the restore file of the sensor port, `circulo_integrated_encoder_<sensor port>_config.bin`. The drive writes that file back into the encoder when the iC-MU reports a corrupted EEPROM. False keeps the file there now. The drive keeps a restore file only for sensor ports 1 and 2.
   * @default true
   */
  writeRestoreFile?: boolean;
  /**
   * `ic-mu-calibration` only: where the ring of an external encoder on sensor port 3 to 5 sits. Required there, and refused for port 1, which is always inner, and port 2, which is always outer. The calibration starts from the values measured for that position.
   * @example "inner"
   */
  ringPosition?: "inner" | "outer";
  /**
   * `ic-mu-calibration` only: runs one validation pass, which reports the nonius phase margins of the calibration the encoder holds, and writes nothing to the encoder.
   * @default false
   */
  measurementOnly?: boolean;
  /**
   * `ic-mu-calibration` only: after a full calibration, shows the NON_CTR error of the iC-MU in the BiSS error bit, register 0x0C = 0x2C.
   * @default false
   */
  activateHealthMonitoring?: boolean;
  /**
   * `ic-mu-calibration` only: the velocity of the encoder's ring in rpm, on whichever shaft the encoder sits. Without it a motor-shaft encoder runs at the velocity measured for its ring and a driving-shaft encoder at 15 rpm. The 0.95 × 0x6080 cap still applies, and a narrow range between the software position limits replaces it with the velocity that fits.
   * @min 1
   * @max 1000
   * @example 90
   */
  ringVelocity?: number;
  /**
   * `restore-default-parameters` only: which group of defaults to restore — all (0x1011:01), communication (0x1011:02), application (0x1011:03), or manufacturer (0x1011:04).
   * @default "all"
   * @example "all"
   */
  group?: "all" | "communication" | "application" | "manufacturer";
  /**
   * `device-locate` only: how long the LEDs blink, in seconds. They stop at the end, and when the run is cancelled. Only a device whose transport can blink the LEDs offers `device-locate`, which today is a SPoE drive.
   * @min 1
   * @max 3600
   * @default 10
   * @example 10
   */
  duration?: number;
  /** `firmware-installation`: the .zip firmware package, base64-encoded into this JSON string. One of this or a `packageFilename` naming an already-cached package must be present; this wins when both are given. To avoid base64 entirely, `PUT` the raw bytes to `/api/user-cache/firmwares/<name>.zip` first and then send only `packageFilename` — the firmware cache is that directory. (Deliberately not `format: byte`: that is the correct OpenAPI spelling for base64, but generators map it to a binary type — `Blob` in TypeScript — which is not what goes on the wire here and would serialise to `{}`.) */
  packageContent?: string;
  /**
   * `firmware-installation`: the package's filename. Optional — it names the package for caching and identifies a cached one to re-install without uploading it again. A name outside the SOMANET convention still installs, it is just not cached.
   * @example "package_SOMANET-Circulo-7_8500-04-2332_motion-drive_v5.6.10.zip"
   */
  packageFilename?: string;
  /**
   * `firmware-installation`: entries inside the package that are not written to the device. An explicit list replaces the defaults wholesale rather than adding to them, so `[]` means "write everything the package holds". Any entry can be named, including the SII and the firmware binaries.
   * @default ["SOMANET_CiA_402.xml.zip","stack_image.svg.zip"]
   */
  skipFiles?: string[];
  /**
   * `firmware-installation`: keep a copy of the package on the server so it can be re-installed without uploading it again. Only possible when a filename is given and it follows the SOMANET package naming convention.
   * @default true
   */
  cachePackage?: boolean;
  /**
   * `firmware-installation`: where the device is left, as an AL state number — the same ETG.1000.6 encoding `POST /api/devices/state` takes and `alState` reports, so a client holds one way to name a state rather than two. `2` (PRE-OP) is the default and the confirmation that the install worked: the bootloader hands over to the newly written firmware on that transition, so reaching PRE-OP means the new firmware booted. No power cycle is needed for the firmware (an SII written from the package does need one — the ESC reads its EEPROM at reset). Choose `3` (BOOT) when no application will be present, after erasing one or between two installs, since a PRE-OP transition then has nothing to hand over to and the drive answers AL status `0x0014`, "No valid firmware". `4` and `8` climb through PRE-OP and re-map the whole bus on the way, briefly pausing every other device. The state the device was in beforehand is not restored. Over SPoE the drive restarts into the new firmware and comes up in PRE-OP, so `3` cannot keep it in BOOT.
   * @default 2
   * @example 2
   */
  finalState?: 1 | 2 | 3 | 4 | 8;
  [key: string]: any;
}

export interface OutputStageResult {
  /**
   * 1-based bus position of the owning device (echoes the request)
   * @example 1
   */
  slavePosition: number;
  /**
   * CoE object index (echoes the request)
   * @example 24698
   */
  index: number;
  /**
   * CoE object subindex (echoes the request)
   * @example 0
   */
  subindex: number;
  /**
   * True when the value landed in the published output image and will be sent on the next exchange cycle
   * @example true
   */
  staged: boolean;
  /**
   * Empty on success; otherwise why the object was not cyclically staged (unknown device, coercion failure, object not output-mapped, or the bus not exchanging)
   * @example ""
   */
  error: string;
}

export interface SyncManagerConfig {
  /**
   * Sync Manager number (0–7)
   * @example 2
   */
  index: number;
  /**
   * Physical ESC memory start address the SM guards
   * @example 4352
   */
  physicalStart: number;
  /**
   * Length of the guarded window in bytes
   * @example 12
   */
  length: number;
  /**
   * Raw SM control/flags register (buffer mode, direction, watchdog, enable) — decoded by the client
   * @example 100100
   */
  flags: number;
  /**
   * 0 unused, 1 MbxOut, 2 MbxIn, 3 Outputs, 4 Inputs
   * @example 3
   */
  type: number;
}

export interface FmmuConfig {
  /**
   * FMMU number (0–3)
   * @example 0
   */
  index: number;
  /**
   * Logical (bus-wide) start address
   * @example 0
   */
  logicalStart: number;
  /**
   * Mapped length in bytes
   * @example 12
   */
  length: number;
  /**
   * Start bit within the first logical byte
   * @example 0
   */
  logicalStartBit: number;
  /**
   * End bit within the last logical byte
   * @example 7
   */
  logicalEndBit: number;
  /**
   * Physical ESC start address (ties the FMMU to a Sync Manager)
   * @example 4352
   */
  physicalStart: number;
  /**
   * Start bit within the first physical byte
   * @example 0
   */
  physicalStartBit: number;
  /**
   * ESC FMMU type: 1 read (inputs/TxPDO), 2 write (outputs/RxPDO)
   * @example 2
   */
  type: number;
  /**
   * Whether the FMMU is active
   * @example true
   */
  active: boolean;
}

export interface SlaveConfig {
  /**
   * 1-based bus position
   * @example 1
   */
  slavePosition: number;
  /**
   * SII device name for this slave position, empty if unknown. For SOMANET drives this is the generic group name "SOMANET"; use productName to distinguish products.
   * @example "SOMANET"
   */
  deviceName: string;
  /**
   * Canonical product name, empty if unknown. For a recognised SOMANET product this distinguishes the product (e.g. "SOMANET Circulo"); otherwise it falls back to deviceName.
   * @example "SOMANET Node"
   */
  productName: string;
  /**
   * Vendor ID from EEPROM (0 if the slave is unknown)
   * @format int64
   * @example 8914
   */
  vendorId: number;
  /**
   * Product code from EEPROM
   * @format int64
   * @example 769
   */
  productCode: number;
  /**
   * Revision number from EEPROM
   * @format int64
   * @example 285212675
   */
  revisionNumber: number;
  /**
   * Serial number from EEPROM
   * @format int64
   * @example 0
   */
  serialNumber: number;
  /**
   * Station (configured) address assigned during scan
   * @example 4097
   */
  configuredAddress: number;
  /**
   * Configured station alias from EEPROM
   * @example 0
   */
  aliasAddress: number;
  /**
   * Mapped output (master→slave) bits
   * @example 96
   */
  outputBits: number;
  /**
   * Mapped input (slave→master) bits
   * @example 128
   */
  inputBits: number;
  mailbox: {
    /**
     * Write (master→slave) mailbox length in bytes; 0 if none
     * @example 128
     */
    writeLength: number;
    /**
     * Write mailbox physical ESC offset
     * @example 4096
     */
    writeOffset: number;
    /**
     * Read (slave→master) mailbox length in bytes
     * @example 128
     */
    readLength: number;
    /**
     * Read mailbox physical ESC offset
     * @example 4224
     */
    readOffset: number;
    /**
     * Supported-protocol bits (0x01 AoE, 0x02 EoE, 0x04 CoE, 0x08 FoE, 0x10 SoE, 0x20 VoE) — decoded by the client
     * @example 14
     */
    protocols: number;
    /**
     * CoE detail bits, advertised in EEPROM — decoded by the client (0x01 SDO, 0x02 SDO-Info, 0x04 PDO-Assign, 0x08 PDO-Config, 0x10 Upload, 0x20 Complete-Access). A hint, not a guarantee.
     * @example 15
     */
    coeDetails: number;
    /**
     * FoE detail byte (bit 0 = enabled).
     * @example 1
     */
    foeDetails: number;
    /**
     * EoE detail byte (bit 0 = enabled).
     * @example 0
     */
    eoeDetails: number;
    /**
     * SoE detail byte / channel count.
     * @example 0
     */
    soeDetails: number;
  };
  dc: {
    /**
     * Slave has distributed-clock hardware
     * @example true
     */
    capable: boolean;
    /**
     * SYNC0 generation enabled (false for SM-synchronous bring-up)
     * @example false
     */
    active: boolean;
    /**
     * Measured propagation delay in nanoseconds
     * @example 300
     */
    propagationDelay: number;
    /**
     * DC cycle time in nanoseconds
     * @example 0
     */
    cycleTime: number;
    /**
     * Shift from the cycle-modulus boundary in nanoseconds
     * @example 0
     */
    shift: number;
  };
  /** Configured Sync Managers, by index */
  syncManagers: SyncManagerConfig[];
  /** Configured FMMUs, by index */
  fmmus: FmmuConfig[];
}

export interface PortDiagnostics {
  /**
   * Physical link detected on this port (DL Status link bit)
   * @example true
   */
  linkUp: boolean;
  /**
   * Loop closed on this port (no downstream slave, or port disabled)
   * @example false
   */
  loopClosed: boolean;
  /**
   * Stable communication established on this port
   * @example true
   */
  communication: boolean;
  /**
   * Invalid-frame counter (0x0300+): frames with a bad FCS/structure. 8-bit, saturates at 255; watch for a rising delta rather than the absolute value
   * @example 0
   */
  invalidFrame: number;
  /**
   * Physical-layer RX error counter (0x0301+): RX_ER asserted by the PHY
   * @example 0
   */
  rxError: number;
  /**
   * Forwarded RX error counter (0x0308+): errors first flagged by an upstream ESC — pinpoints the segment where corruption began
   * @example 0
   */
  forwardedError: number;
  /**
   * Lost-link counter (0x0310+): link-down events on this port
   * @example 0
   */
  lostLink: number;
}

export interface DeviceDiagnostics {
  /**
   * 1-based position on the fieldbus
   * @example 1
   */
  slavePosition: number;
  /**
   * Device name for this slave position, empty if unknown
   * @example "SOMANET Node"
   */
  deviceName: string;
  /** Per-port link state and error counters (ports 0–3, in order) */
  ports: PortDiagnostics[];
  /**
   * ECAT processing-unit error counter (0x030C): datagrams reaching the unit malformed
   * @example 0
   */
  processingUnitError: number;
  /**
   * PDI error counter (0x030D): problems on the slave-local process-data interface
   * @example 0
   */
  pdiError: number;
  /**
   * Process-data (SM) watchdog expirations (0x0442): the slave stopped seeing fresh outputs
   * @example 0
   */
  processDataWatchdog: number;
  /**
   * PDI watchdog expirations (0x0443)
   * @example 0
   */
  pdiWatchdog: number;
}

export interface DcSyncStatus {
  /**
   * 1-based position on the fieldbus
   * @example 1
   */
  slavePosition: number;
  /**
   * Device name for this slave position, empty if unknown
   * @example "SOMANET Node"
   */
  deviceName: string;
  /**
   * Slave has distributed-clock hardware
   * @example true
   */
  dcCapable: boolean;
  /**
   * This slave is the DC reference clock (the first DC-capable slave) — it defines bus time, so its own systemTimeDifference is zero
   * @example false
   */
  referenceClock: boolean;
  /**
   * System-time delay / propagation delay (0x0928), nanoseconds
   * @example 300
   */
  propagationDelay: number;
  /**
   * Signed deviation of the local system time from the reference clock (0x092C), in nanoseconds. Positive = local clock ahead of the reference, negative = behind; zero on the reference clock. Meaningful only while exchanging in SAFE-OP/OP; converges toward zero as the drift-compensation loop settles
   * @example 0
   */
  systemTimeDifference: number;
}

/** The counts for one kind of frame. The bands are cumulative. */
export interface CaptureKindSummary {
  /**
   * Frames the master sent. A frame sent again counts once.
   * @example 60021
   */
  sent: number;
  /**
   * Frames sent again with the same index and header while the first copy had no answer yet. A register datagram goes out again every 2 ms, so a late one shows here too.
   * @example 12
   */
  resent: number;
  /**
   * Sent frames that came back
   * @example 60018
   */
  returned: number;
  /**
   * Sent frames with no return before the capture ended
   * @example 3
   */
  lost: number;
  /**
   * Returns later than 2 ms after the first send
   * @example 1210
   */
  over2Ms: number;
  /**
   * Returns later than 20 ms. Counted in over2Ms as well.
   * @example 1198
   */
  over20Ms: number;
  /**
   * Returns later than 100 ms. Counted in the two above as well.
   * @example 1195
   */
  over100Ms: number;
  /**
   * The latest return, in microseconds after its first send
   * @format int64
   * @example 221748
   */
  latestUs: number;
}

/** How late the frames of a capture came back. See `POST /api/captures`. */
export interface CaptureSummary {
  /**
   * Every record in the capture
   * @example 120050
   */
  frames: number;
  /**
   * Records that are not an EtherCAT command frame
   * @example 0
   */
  notEtherCat: number;
  /**
   * Frames from the slaves with no send to match, such as the second answer to a frame sent twice
   * @example 8
   */
  unmatchedReturns: number;
  /**
   * Timestamp of the first record, epoch microseconds
   * @format int64
   * @example 1761143405000123
   */
  firstUs: number;
  /**
   * Timestamp of the last record, epoch microseconds
   * @format int64
   * @example 1761143435000456
   */
  lastUs: number;
  /** The counts for one kind of frame. The bands are cumulative. */
  control: CaptureKindSummary;
  /** The counts for one kind of frame. The bands are cumulative. */
  cyclic: CaptureKindSummary;
}

export interface AlStatusCode {
  /**
   * AL Status Code value (ETG.1000.6 §6.4.1)
   * @example 20
   */
  code: number;
  /**
   * Short human-readable name
   * @example "No valid firmware"
   */
  name: string;
  /**
   * Full description of the error condition
   * @example "No valid firmware is present — the slave needs firmware flashed"
   */
  description: string;
  /**
   * True if a slave reporting this code cannot reach the requested EtherCAT state by retrying. The server abandons the slave immediately during a state transition instead of waiting for timeout; the master must change something (re-init, reflash, power cycle) before another transition attempt can succeed.
   * @example true
   */
  terminal: boolean;
}

export interface EscRegister {
  /**
   * Register address in the ESC address space (decimal)
   * @example 272
   */
  address: number;
  /**
   * Register width in bytes (1, 2, 4, or 8)
   * @example 2
   */
  length: number;
  /**
   * Short snake_case identifier
   * @example "dl_status"
   */
  name: string;
  /**
   * Human-readable description from the Beckhoff ESC datasheet
   * @example "DL status: EEPROM load ok, link detected, communication established per port"
   */
  description: string;
}

export interface FoeErrorCode {
  /**
   * 32-bit FoE error code as sent in the FoE ERROR packet (ETG.1000.6 §5.5)
   * @example 32770
   */
  code: number;
  /**
   * Short human-readable name
   * @example "File not found"
   */
  name: string;
  /**
   * Full description of the error condition
   * @example "The requested file does not exist on the slave"
   */
  description: string;
}

export interface SdoAbortCode {
  /**
   * 32-bit CoE SDO abort code (ETG.1000.6 §5.6.2.7.2, Table 41)
   * @example 100794368
   */
  code: number;
  /**
   * Human-readable meaning of the abort
   * @example "The object does not exist in the object dictionary"
   */
  description: string;
}

export interface MailboxErrorCode {
  /**
   * 16-bit mailbox error Detail code (ETG.1000.4, Table 30)
   * @example 1
   */
  code: number;
  /**
   * Symbolic name
   * @example "MBXERR_SYNTAX"
   */
  name: string;
  /**
   * Human-readable meaning of the error
   * @example "Syntax of the 6-octet mailbox header is wrong"
   */
  description: string;
}

export interface ObjectDataTypeInfo {
  /**
   * ETG.1020 data type code
   * @example 7
   */
  code: number;
  /**
   * Symbolic name of the data type
   * @example "UNSIGNED32"
   */
  name: string;
  /**
   * Declared bit width of one element; 0 for variable-length types
   * @example 32
   */
  bitSize: number;
}

/**
 * The value of one object dictionary entry, encoded by its `dataType` (ETG.1020). BOOLEAN (0x0001), INTEGER8 (0x0002), INTEGER16 (0x0003), INTEGER32 (0x0004), INTEGER64 (0x0015), UNSIGNED8 (0x0005), UNSIGNED16 (0x0006), UNSIGNED32 (0x0007), BYTE (0x001E), WORD (0x001F) and DWORD (0x0020) are a JSON integer that fits `int64`. UNSIGNED64 (0x001B) is a JSON integer that fits `uint64`, and a value above 9223372036854775807 matches only that alternative. REAL32 (0x0008) and REAL64 (0x0011) are a JSON number, which the server writes as a double, so a REAL32 shows its exact binary value. VISIBLE STRING (0x0009) and UNICODE STRING (0x000B) are a string. Every other data type, OCTET STRING and DOMAIN included, is an array of byte values. Every integer is also a valid number, so use `dataType` to choose the alternative, the way the server does. On a write, the server converts the value to the declared type of the parameter.
 * @example 12345
 */
export type ParameterValue = number | string | number[];

export interface DeviceParameter {
  /**
   * CoE object index
   * @example 24640
   */
  index: number;
  /**
   * CoE object subindex
   * @example 0
   */
  subindex: number;
  /**
   * Textual description from the slave's SDO Info "Get Entry Description"
   * @example "Position actual value"
   */
  name: string;
  /**
   * ETG.1000.6 §5 object code (VAR=7, ARRAY=8, RECORD=9)
   * @example 7
   */
  objectCode: number;
  /**
   * ETG.1020 data type code (e.g. 7 = UNSIGNED32)
   * @example 4
   */
  dataType: number;
  /**
   * Symbolic name of the data type (resolved server-side from `dataType`)
   * @example "INTEGER32"
   */
  dataTypeName: string;
  /**
   * Bit length of the entry's value
   * @example 32
   */
  bitLength: number;
  /**
   * ObjAccess bitfield (read/write per AL state)
   * @example 7
   */
  access: number;
  /**
   * Where this parameter's definition came from. `objectDictionary` — enumerated over the CoE object dictionary (SDO-Info). `sii` — derived from the SII EEPROM PDO categories, for a mailbox-less slave (an EtherCAT coupler / simple I/O terminal) that has no CoE object dictionary; such a parameter has no live SDO access, so its value comes only from the process image.
   * @example "objectDictionary"
   */
  origin: "objectDictionary" | "sii";
  /**
   * Last-known value, decoded according to `dataType`.  Initialised to a type-appropriate zero (0 for numbers, "" for strings, [] for raw bytes) before the first read.  After a successful SDO upload, holds the decoded value. `ParameterValue` says which JSON type each `dataType` uses.
   * @example 12345
   */
  value: ParameterValue;
  /**
   * ETG.1004 unit code of this entry. The code comes from the SDO Info response of the slave. When the slave reports none, it comes from the device's ESI in the `esis` directory of the user cache. That file is named `esi-<vendor>-<version>.xml`, where `<version>` is the device's firmware version in 0x100A, matched exactly. Absent when neither source gives a unit. The 32-bit code decomposes into prefix, base SI unit, and exponent per ETG.1004.
   * @example 11814656
   */
  unit?: number;
  /**
   * The symbol of `unit`, for example `rpm` or `mV`. It comes from the ESI with the unit, or is rendered from the ETG.1004 code when the slave reported the unit. Absent when there is no unit or it has no symbol.
   * @example "rpm"
   */
  unitSymbol?: string;
  /** The default value, decoded with the same logic as `value`. It comes from the SDO Info response of the slave. When the slave reports none, it comes from the device's ESI (see `esiFile`). Absent when neither source gives one. */
  defaultValue?: ParameterValue;
  /** The minimum value, decoded with the same logic as `value`. It comes from the SDO Info response of the slave. When the slave reports none, it comes from the device's ESI (see `esiFile`). Absent when neither source gives one. */
  minValue?: ParameterValue;
  /** The maximum value, decoded with the same logic as `value`. It comes from the SDO Info response of the slave. When the slave reports none, it comes from the device's ESI (see `esiFile`). Absent when neither source gives one. */
  maxValue?: ParameterValue;
  /** What the entry does, from the device's ESI, as HTML. A record or array is described once, on subindex 0. Absent when the ESI gives no description. SOMANET ESIs carry one for most objects. */
  description?: string;
  /** The named values of an enumerated entry, from the device's ESI. Absent when the entry is not an enumeration. */
  options?: {
    /**
     * @format int64
     * @example 1
     */
    value: number;
    /** @example "Enabled" */
    label: string;
  }[];
  /**
   * The file in the `esis` directory of the user cache that described this entry. Absent when the device has no ESI, or its ESI does not describe this entry. On a device whose other entries have one, an absent `esiFile` means the ESI and the firmware disagree.
   * @example "esi-000022d2-v5.6.11.xml"
   */
  esiFile?: string;
  /**
   * Freshness of `value` relative to the device. `synced` — matches the device (last successful read or write); `pending` — set locally while offline or after a failed write, not yet confirmed on the device; `stale` — came from an earlier read or write, and the latest read of the object failed; `unknown` — never read, `value` is the type-appropriate default. A failed read turns `synced` into `stale` and keeps the value. It leaves `unknown` and `pending` unchanged. The next successful read sets `synced` again.
   * @example "synced"
   */
  syncState: "unknown" | "synced" | "pending" | "stale";
}

/** Parsed SII (EEPROM) image of a slave — the fixed header plus the decoded category section.  String-index fields are 1-based references into `category.strings`. */
export interface SlaveInformationInterface {
  /** Fixed 128-byte header (identity and mailbox configuration) */
  info: {
    pdiControl?: number;
    pdiConfiguration?: number;
    syncImpulseLen?: number;
    pdiConfiguration2?: number;
    configuredStationAlias?: number;
    checksum?: number;
    /** @example 8914 */
    vendorId?: number;
    /** @example 769 */
    productCode?: number;
    revisionNumber?: number;
    serialNumber?: number;
    bootstrapReceiveMailboxOffset?: number;
    bootstrapReceiveMailboxSize?: number;
    bootstrapSendMailboxOffset?: number;
    bootstrapSendMailboxSize?: number;
    standardReceiveMailboxOffset?: number;
    standardReceiveMailboxSize?: number;
    standardSendMailboxOffset?: number;
    standardSendMailboxSize?: number;
    /**
     * Bitfield: 0x01 AoE, 0x02 EoE, 0x04 CoE, 0x08 FoE, 0x10 SoE, 0x20 VoE
     * @example 12
     */
    mailboxProtocol?: number;
    /** Raw EEPROM-size word (ETG: size in KiBit − 1) */
    size?: number;
    version?: number;
  };
  /** Decoded SII category section */
  category: {
    /**
     * STRINGS table (category 10), referenced by 1-based index
     * @example ["SOMANET","SOMANET Circulo CiA402 Drive"]
     */
    strings?: string[];
    /** GENERAL device information (category 30) */
    general?: {
      groupIdx?: number;
      imgIdx?: number;
      orderIdx?: number;
      nameIdx?: number;
      coeDetails?: number;
      foeDetails?: number;
      eoeDetails?: number;
      soeChannels?: number;
      ds402Channels?: number;
      sysmanClass?: number;
      flags?: number;
      currentOnEBus?: number;
      physicalPort?: number;
      physicalMemoryAddress?: number;
    };
    /** FMMU defaults (category 40), one raw 16-bit word per entry */
    fmmus?: number[];
    /** Sync-Manager defaults (category 41) */
    syncManagers?: {
      physicalStartAddress?: number;
      length?: number;
      controlRegister?: number;
      statusRegister?: number;
      enableSyncManager?: number;
      /** 1 MbxOut, 2 MbxIn, 3 Outputs, 4 Inputs */
      syncManagerType?: number;
    }[];
    /** Default RxPDOs (category 51) */
    rxPdos?: SiiPdo[];
    /** Default TxPDOs (category 50) */
    txPdos?: SiiPdo[];
    /** Distributed-clock settings (category 60) */
    distributedClocks?: {
      cycleTime0?: number;
      shiftTime0?: number;
      shiftTime1?: number;
      sync1CycleFactor?: number;
      assignActivate?: number;
      sync0CycleFactor?: number;
      nameIdx?: number;
      descIdx?: number;
    }[];
  };
}

/** One default PDO from the SII (RxPDO category 51 or TxPDO category 50) */
export interface SiiPdo {
  pdoIndex?: number;
  /** Declared entry count (may exceed `entries.length` if the payload was truncated) */
  nEntry?: number;
  syncM?: number;
  synchronization?: number;
  nameIdx?: number;
  flags?: number;
  entries?: {
    entryIndex?: number;
    subindex?: number;
    entryNameIdx?: number;
    dataType?: number;
    bitLen?: number;
    flags?: number;
  }[];
}

/** One addressable `(index, subindex)` of a device's object dictionary, assembled from the ESI. `defaultData`/`minData`/`maxData` are **raw little-endian bytes** as uppercase hexBinary, the same spelling the ESI itself uses — the first byte is the least significant, so `92010200` is `0x00020192`. Decode them with `dataType`; a byte-wise comparison of `minData` against `maxData` is wrong when `isSigned`. */
export interface EsiEntry {
  /** Effective CoE index */
  index: number;
  subindex: number;
  /** Index as written in the source dictionary; omitted when unrelocated */
  rawIndex?: number;
  /** The parent object's name — identical across all its subindices */
  objectName: string;
  /** This subindex's own name */
  entryName: string;
  /** The ESI's display-name override, else `entryName` */
  displayName: string;
  objectCode: "VAR" | "ARRAY" | "RECORD";
  /** The CoE object code (ETG.1000.6 §5) — 7, 8 or 9 */
  objectCodeValue?: number;
  /** Highest subindex; omitted for a VAR */
  numberOfEntries?: number;
  /**
   * ESI/IEC 61131-3 type name, e.g. `UDINT`, `STRING(50)`
   * @example "UDINT"
   */
  dataTypeName: string;
  /**
   * ETG.1020 code, matching `DeviceParameter.dataType`; 0 if unresolved
   * @example 7
   */
  dataType: number;
  /**
   * The C++ type this entry's value maps to, resolved from `dataType` **and** `bitSize`. The width is not decoration: a vendor's `ARRAY [0..24] OF BYTE` resolves to the ETG.1020 code for BYTE, so trusting the code alone would call a 25-byte object a `uint8_t` and every read of it would return its first byte. Where the declared width contradicts the code, this is `std::vector<uint8_t>`.
   * @example "int32_t"
   */
  cxxType: string;
  isSigned: boolean;
  bitSize: number;
  /** Offset within the object's complete-access image */
  bitOffset: number;
  /** Uppercase hexBinary */
  defaultData?: string;
  minData?: string;
  maxData?: string;
  /** Packed ETG.1004 notation value */
  unit?: number;
  /** @example "mV" */
  unitSymbol?: string;
  access: {
    mode: "ro" | "rw" | "wo";
    readRestrictions?:
      | "PreOP"
      | "PreOP_SafeOP"
      | "PreOP_OP"
      | "SafeOP"
      | "SafeOP_OP"
      | "OP";
    writeRestrictions?:
      | "PreOP"
      | "PreOP_SafeOP"
      | "PreOP_OP"
      | "SafeOP"
      | "SafeOP_OP"
      | "OP";
  };
  /** Mandatory, optional or conditional */
  category: "m" | "o" | "c";
  /** Empty when not mappable; `t` TxPDO, `r` RxPDO */
  pdoMapping: "" | "t" | "r" | "tr";
  safetyMapping?: "si" | "so" | "sio" | "sp";
  sdoAccess: "SubIndexAccess" | "CompleteAccess";
  backup: boolean;
  setting: boolean;
  /** SoE IDN */
  attribute?: number;
  /** ETG.1000.6 ObjAccess bitfield synthesised from the flags — bits 0-2 read in PreOP/SafeOP/OP, 3-5 write, 6 RxPDO-mappable, 7 TxPDO-mappable, 8 backup, 9 setting. Matches `DeviceParameter.access`. */
  objAccess: number;
  /** This row's description (HTML), decoded from `properties`. On subindex 0 it is the object's; on a RECORD member it is that member's; an ARRAY element has none. */
  description?: string;
  /** Enum labels, from `<EnumInfo>` and/or an `options` property */
  options?: {
    label: string;
    value: number;
  }[];
  /** Raw `<Property>` annotations as written, for whatever this row describes. This is the ESI's generic extension mechanism; `description` and `options` above are conveniences decoded from the two conventional names, so a vendor using other names still has its data here. **Subindex 0 carries the object's own annotation** (stored once, not repeated onto every subindex); other rows carry only what that entry itself declares. */
  properties?: EsiProperties;
  /** Which dictionary this entry came from. The module's *name* is not repeated here — resolve `moduleIdent` against the top-level `modules` list. */
  source: {
    kind: "device" | "module";
    moduleIdent?: number;
    /** Zero-based slot number */
    slot?: number;
    /** Slot offset applied to `rawIndex` */
    indexOffset?: number;
  };
}

/** Raw `<Property>` annotations as written, for whatever this row describes. This is the ESI's generic extension mechanism; `description` and `options` above are conveniences decoded from the two conventional names, so a vendor using other names still has its data here. **Subindex 0 carries the object's own annotation** (stored once, not repeated onto every subindex); other rows carry only what that entry itself declares. */
export type EsiProperties = {
  name: string;
  value: string;
}[];

export interface EsiDeviceSummary {
  /** Zero-based position in the file */
  ordinal: number;
  /** @example "SOMANET Node" */
  type: string;
  name: string;
  productCode?: number;
  revisionNo?: number;
  /** @example 402 */
  profileNo?: number;
  groupType: string;
  /** Objects in the device's own dictionary, before any module merge */
  objectCount: number;
  rxPdoCount?: number;
  txPdoCount?: number;
  /** Every `ModuleIdent` the device's slots reference, in slot order */
  moduleIdents: number[];
  /** The parsed `<Slots>` block */
  slots?: object;
  /** This device's flat, slot-merged object dictionary */
  entries?: EsiEntry[];
  /** Problems found while assembling *this device* — a value whose byte length disagrees with its declared width, an unresolvable type, a module collision. Distinct from the top-level `warnings`, which are about the document; a different `modules` selection would produce a different list here. */
  warnings?: string[];
}

/** A parsed ESI. Every device carries its own assembled `entries` table. */
export interface EsiParseResult {
  /** The `EtherCATInfo/@Version` attribute */
  version?: string;
  vendor: {
    /** @example 8914 */
    id: number;
    name: string;
    url?: string;
  };
  devices: EsiDeviceSummary[];
  modules: {
    moduleIdent: number;
    /** @example "CiA402 Dictionary" */
    type: string;
    name: string;
    objectCount?: number;
  }[];
  /** Recoverable problems in the document itself — a skipped malformed object, an unparsable value. A file with warnings still parses. Per-device assembly problems are reported on the device instead. */
  warnings?: string[];
}

/** What Motion Master knows about the auto-tuning process, as a startup snapshot. */
export interface AutoTuningStatus {
  /**
   * The `autoTuning.enabled` setting from the configuration file.
   * @example true
   */
  enabled: boolean;
  /**
   * The executable Motion Master looked for, as an absolute path.
   * @example "/opt/motion-master/auto-tuning"
   */
  binaryPath: string;
  /**
   * Whether a file exists at that path.
   * @example true
   */
  installed: boolean;
  /**
   * Whether the process started and answered its health endpoint. Not a liveness check: nothing polls the process after startup.
   * @example true
   */
  started: boolean;
  /**
   * The version the process reported, empty unless it started. A build older than that program's version reporting starts and reports nothing, which is why an empty string here does not mean it failed.
   * @example "3.1.1"
   */
  version: string;
  /**
   * The loopback port the process was told to serve on.
   * @example 63528
   */
  port: number;
  /**
   * Why it is not running. Empty when it is.
   * @example ""
   */
  error: string;
}

/** The auto-tuning program's error shape. Returned with 200 when a function rejected its inputs, and with 404 or 500 for an unknown function or an unhandled exception. */
export interface AutoTuningError {
  error: {
    /** @example "A linear model can't be fit in the recorded data." */
    message: string;
  };
}

/**
 * Transfer function coefficients, highest order first.
 * @example {"numerator":[1],"denominator":[0.00015,0.000206]}
 */
export interface PlantModel {
  numerator: number[];
  denominator: number[];
}

/** One controller's gains. */
export interface Pid {
  /** @format double */
  kp?: number;
  /** @format double */
  ki?: number;
  /** @format double */
  kd?: number;
}

/** Controller gains. A function that tunes only the velocity loop leaves the position gains at zero. */
export interface Gains {
  /** One controller's gains. */
  position?: Pid;
  /** One controller's gains. */
  velocity?: Pid;
}

/**
 * Bode diagram data, keyed by column name, every array the same length. This comes back in the response body instead of a written CSV, which is why no auto-tuning call needs a shared filesystem. A non-finite value is null.
 * @example {"Frequency_Hz":[0.5,1],"Plant_Mag_dB":[65.77570877159437,59.79191013147556],"Plant_Phase_deg":[-66.38766900151838,-76.55997369096534]}
 */
export type Bode = Record<string, (number | null)[]>;

/** Drive configuration, as read from the drive. */
export interface DriveConfig {
  vel_lp_type?: number;
  /** @format double */
  vel_lp_fc?: number;
  pos_lp_type?: number;
  /** @format double */
  pos_lp_fc?: number;
  notch_en?: number;
  /** @format double */
  notch_fw?: number;
  /** @format double */
  notch_fc?: number;
  /** @format double */
  notch_dp?: number;
  /** @format double */
  vel_ff_gain?: number;
  /** @format double */
  vel_ff_fc?: number;
  /**
   * Winding inductance.
   * @format double
   */
  L?: number;
  /**
   * Winding resistance.
   * @format double
   */
  R?: number;
  /**
   * DC link voltage.
   * @format double
   */
  vdc?: number;
  /** @format double */
  current_ratio?: number;
  /** @format double */
  tc_kp?: number;
  /** @format double */
  tc_ki?: number;
  switch_F?: number;
  /** @format double */
  tc_st?: number;
  /** @format double */
  tc_damping?: number;
  software_version?: string;
  /** @format double */
  rated_torque?: number;
  /** @format double */
  max_vel_noise?: number;
}

/** The `data` schema depends on the value of `run`. Each variant pairs one `run` value with the inputs that function reads. `run: exit` is deliberately absent: it shuts the process down, and this endpoint refuses it with 400. */
export type AutoTuningRunRequest =
  | AutoTuneVelocityController
  | AutoTunePositionController
  | FullAutoTuneVelocityController
  | FullAutoTunePositionController
  | AutoTuneNotch
  | AutoTuneFeedbackFilters
  | ComputePositionControllerGains
  | IdentifyPlantModel
  | GeneratePlantBode;

export interface AutoTuneVelocityController {
  run: "auto_tune_velocity_controller";
  data: {
    /** Transfer function coefficients, highest order first. */
    plant_model: PlantModel;
    /**
     * Damping ratio.
     * @format double
     */
    zeta: number;
    /**
     * Demanded bandwidth in Hz.
     * @format double
     */
    demanded_bw: number;
  };
}

export interface AutoTunePositionController {
  run: "auto_tune_position_controller";
  data: {
    /** Transfer function coefficients, highest order first. */
    plant_model: PlantModel;
    /** @example "PI-P" */
    controller_type: string;
    /** @format double */
    zeta: number;
    /**
     * Demanded settling time in seconds.
     * @format double
     */
    demanded_st: number;
    /**
     * Optional. Absent means the standard tuning rule.
     * @format double
     */
    highFrictionGain?: number;
  };
}

export interface FullAutoTuneVelocityController {
  run: "full_auto_tune_velocity_controller";
  data: {
    /** Transfer function coefficients, highest order first. */
    plant_model: PlantModel;
    /** Drive configuration, as read from the drive. */
    drive_config: DriveConfig;
  };
}

export interface FullAutoTunePositionController {
  run: "full_auto_tune_position_controller";
  data: {
    /** Transfer function coefficients, highest order first. */
    plant_model: PlantModel;
    /** @example "PI-P" */
    controller_type: string;
    /** Drive configuration, as read from the drive. */
    drive_config: DriveConfig;
    highFriction: 0 | 1;
  };
}

export interface AutoTuneNotch {
  run: "auto_tune_notch";
  data: {
    /** Transfer function coefficients, highest order first. */
    plant_model: PlantModel;
  };
}

export interface AutoTuneFeedbackFilters {
  run: "auto_tune_feedback_filters";
  data: {
    /** @format double */
    sigma_v: number;
    /** @format double */
    sigma_p: number;
    /** @format double */
    resVel: number;
    /** @format double */
    resPos: number;
    /** @format double */
    gearRatio: number;
  };
}

export interface ComputePositionControllerGains {
  run: "compute_position_controller_gains";
  data: {
    /** @format double */
    pk: number;
    /** @format double */
    pt: number;
    /** @format double */
    omega: number;
    /** @format double */
    alpha: number;
    /** @format double */
    zeta: number;
    /** @example "PI-P" */
    controller_type: string;
  };
}

/** Identifies a plant model from chirp measurements. The measurements are uploaded in `data.csv`; `data.filepath` names a file on the auto-tuning process's own machine instead, and is a fallback that Motion Master has no use for. */
export interface IdentifyPlantModel {
  run: "identify_plant_model" | "identify_plant_model_inline";
  data: {
    /** Contents of the measurement CSV. Three columns, no header: time, torque in Nm, velocity in rad/s, sampled at 1000 Hz. */
    csv?: string;
    /** Path to that CSV instead of its contents. */
    filepath?: string;
    /**
     * Chirp start frequency in Hz.
     * @format double
     */
    f0: number;
    /**
     * Chirp end frequency in Hz.
     * @format double
     */
    f1: number;
  };
}

/** Computes the bode data of a plant model and returns it inline. */
export interface GeneratePlantBode {
  run: "generate_plant_bode_file" | "generate_plant_bode";
  data: {
    /** Transfer function coefficients, highest order first. */
    plant_model: PlantModel;
  };
}

/** Which variant comes back depends on the request's `run`. Every result carries `duration`, the function's execution time in seconds. A function that rejected its inputs answers with `AutoTuningError` and status 200. */
export type AutoTuningRunResponse =
  | GainsResult
  | FullVelocityResult
  | FullPositionResult
  | NotchResult
  | FeedbackFiltersResult
  | IdentifyPlantModelResult
  | BodeResult
  | AutoTuningError;

export interface GainsResult {
  /** Controller gains. A function that tunes only the velocity loop leaves the position gains at zero. */
  gains?: Gains;
  /** @format double */
  duration?: number;
}

export interface FullVelocityResult {
  /** Controller gains. A function that tunes only the velocity loop leaves the position gains at zero. */
  gains?: Gains;
  /** @format double */
  demanded_bw?: number;
  /** @format double */
  demanded_zeta?: number;
  /** Stability margins and closed-loop metrics. */
  state?: {
    /** @format double */
    gm?: number;
    /** @format double */
    pm?: number;
    /** @format double */
    msens?: number;
    /** @format double */
    act_bw?: number;
    /** @format double */
    vel_cl?: number;
    /** @format double */
    noise_resp?: number;
    /** @format double */
    max_KS?: number;
  };
  /** @format double */
  duration?: number;
}

export interface FullPositionResult {
  /** Controller gains. A function that tunes only the velocity loop leaves the position gains at zero. */
  gains?: Gains;
  /** @format double */
  demanded_st?: number;
  /** @format double */
  demanded_zeta?: number;
  /** @format double */
  highFrictionGain?: number;
  /** Stability margins and closed-loop metrics, position and velocity loop. */
  state?: {
    /** @format double */
    gm_p?: number;
    /** @format double */
    pm_p?: number;
    /** @format double */
    msens_p?: number;
    /** @format double */
    gm_v?: number;
    /** @format double */
    pm_v?: number;
    /** @format double */
    msens_v?: number;
    /** @format double */
    noise_resp?: number;
    /** @format double */
    max_KS?: number;
    /** @format double */
    bw_p?: number;
    /** @format double */
    bw_v?: number;
    /** @format double */
    act_st?: number;
    /** @format double */
    pos_cl?: number;
  };
  /** @format double */
  duration?: number;
}

export interface NotchResult {
  notch_en?: boolean;
  /** @format double */
  notch_fc?: number | null;
  /** @format double */
  notch_fw?: number | null;
  /** @format double */
  notch_dp?: number | null;
  /** @format double */
  duration?: number;
}

export interface FeedbackFiltersResult {
  /**
   * Velocity feedback filter cut-off in Hz.
   * @format double
   */
  fcv?: number;
  /**
   * Position feedback filter cut-off in Hz.
   * @format double
   */
  fcp?: number;
  /** @format double */
  duration?: number;
}

export interface IdentifyPlantModelResult {
  numerators?: number[];
  denominators?: number[];
  /** Bode diagram data, keyed by column name, every array the same length. This comes back in the response body instead of a written CSV, which is why no auto-tuning call needs a shared filesystem. A non-finite value is null. */
  bode?: Bode;
  /** @format double */
  duration?: number;
}

export interface BodeResult {
  /** Bode diagram data, keyed by column name, every array the same length. This comes back in the response body instead of a written CSV, which is why no auto-tuning call needs a shared filesystem. A non-finite value is null. */
  bode?: Bode;
  /** @format double */
  duration?: number;
}
