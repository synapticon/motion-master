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
   * Device name for this slave position, empty if unknown
   * @example "SOMANET Node"
   */
  deviceName: string;
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
