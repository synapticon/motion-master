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
   * The setpoint currently commanded for the active mode — target position 0x607A (PP/CSP), velocity 0x60FF (PV/CSV), or torque 0x6071 (PT/CST), widened to INTEGER32. 0 only when the active mode has no linear setpoint (NoMode / Homing).
   * @example 100000
   */
  target: number;
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
   * @example 1000
   */
  targetHz: number;
  /**
   * Cumulative average rate since the loop started (executedCycles / uptime), in hertz; 0 before the loop starts. Below targetHz means the host cannot sustain the configured period.
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
}

/** Summary of one on-disk parameter-cache file (its identity, size, and count). */
export interface ParameterCacheEntry {
  /**
   * Opaque key addressing this cache file in the download/delete endpoints ("<vendor>-<product>-<revision>"). Use it verbatim — do not construct it client-side.
   * @example "000022d2-00000201-0000000a"
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
   * Revision number.
   * @example 10
   */
  revisionNumber: number;
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
   * What the step produced, if anything — shape is per procedure. Absent when the step produced nothing. For `os-command` it is an object with `status`, `data`, and `errorCode` when the drive sent one.
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
   * The full firmware descriptor, verbatim.
   * @example "8500-04-2332"
   */
  firmwareId: string;
  /**
   * Software name.
   * @example "motion-drive"
   */
  firmwareName: string;
  /**
   * Software version, including the leading `v`.
   * @example "v5.6.10"
   */
  firmwareVersion: string;
  /**
   * Product id from the descriptor.
   * @example 8500
   */
  productId?: number;
  /**
   * Product version from the descriptor.
   * @example 4
   */
  productVersion?: number;
  /**
   * Firmware encryption key id from the descriptor.
   * @example 2332
   */
  keyId?: number;
  /**
   * Fieldbus protocol from the descriptor, decoded as hexadecimal. 1 is EtherCAT.
   * @example 1
   */
  fieldbusProtocol?: number;
}

/** One procedure paired with how its last run on this device went — an entry of `GET /api/devices/{slavePosition}/procedures`. The pairing is what lets a page render in a single request instead of one per procedure. */
export interface ProcedureListing {
  /** What a procedure *is*, independent of any run — everything needed to render a control for it. The text is served rather than held per client, because the house rule that every action carries a description and its caveats means it has to exist somewhere, and duplicating it in each client is how it goes stale. */
  descriptor: ProcedureDescriptor;
  /** The complete state of a procedure on one device — an accumulating snapshot rather than an event, which is what makes polling lossless. */
  snapshot: ProcedureSnapshot;
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
   * `encoder-register-communication` and `ic-mu-calibration-mode`: which of the drive's encoders to address. Encoder 1 is whatever 0x2110 configures and encoder 2 whatever 0x2112 does, so the ordinal picks a configured slot rather than a kind of encoder.
   * @default 1
   * @example 1
   */
  encoder?: 1 | 2;
  /**
   * `encoder-register-communication` only: false reads the register, true writes `value` into it. Either way the drive reports what the register holds afterwards.
   * @default false
   */
  write?: boolean;
  /**
   * `encoder-register-communication` only, and required for it: the register to access. The map belongs to the encoder chip rather than to the drive's firmware.
   * @min 0
   * @max 255
   * @example 117
   */
  registerAddress?: number;
  /**
   * `encoder-register-communication` only: the byte to write into the register. Ignored when reading.
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
   * `restore-default-parameters` only: which group of defaults to restore — all (0x1011:01), communication (0x1011:02), application (0x1011:03), or manufacturer (0x1011:04).
   * @default "all"
   * @example "all"
   */
  group?: "all" | "communication" | "application" | "manufacturer";
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
   * `firmware-installation`: where the device is left, as an AL state number — the same ETG.1000.6 encoding `POST /api/devices/state` takes and `alState` reports, so a client holds one way to name a state rather than two. `2` (PRE-OP) is the default and the confirmation that the install worked: the bootloader hands over to the newly written firmware on that transition, so reaching PRE-OP means the new firmware booted. No power cycle is needed for the firmware (an SII written from the package does need one — the ESC reads its EEPROM at reset). Choose `3` (BOOT) when no application will be present, after erasing one or between two installs, since a PRE-OP transition then has nothing to hand over to and the drive answers AL status `0x0014`, "No valid firmware". `4` and `8` climb through PRE-OP and re-map the whole bus on the way, briefly pausing every other device. The state the device was in beforehand is not restored.
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
   * Last-known value, decoded according to `dataType`.  Initialised to a type-appropriate zero (0 for numbers, "" for strings, [] for raw bytes) before the first read.  After a successful SDO upload, holds the decoded value; the JSON encoding follows the variant alternative — number, string, or array of byte values.
   * @example 12345
   */
  value: number | string | number[];
  /**
   * ETG.1004 unit code reported by the slave for this entry.  Absent when the slave does not populate the Unit field of the SDO Info response.  The 32-bit code decomposes into prefix, base SI unit, and exponent per ETG.1004.
   * @example 0
   */
  unit?: number;
  /** Slave-reported default value, decoded with the same logic as `value`.  Absent when the slave does not report a default. */
  defaultValue?: number | string | number[];
  /** Slave-reported minimum value, decoded with the same logic as `value`.  Absent when the slave does not report a minimum. */
  minValue?: number | string | number[];
  /** Slave-reported maximum value, decoded with the same logic as `value`.  Absent when the slave does not report a maximum. */
  maxValue?: number | string | number[];
  /**
   * Freshness of `value` relative to the device. `synced` — matches the device (last successful read or write); `pending` — set locally while offline or after a failed write, not yet confirmed on the device; `unknown` — never read, `value` is the type-appropriate default.
   * @example "synced"
   */
  syncState: "unknown" | "synced" | "pending";
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
