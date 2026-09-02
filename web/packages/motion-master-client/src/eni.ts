// EtherCAT Network Information (ENI) export.
//
// The generated client types `GET /api/eni` as a `File`, which is what swagger-typescript-api makes
// of a binary response. An ENI is text, and what a caller wants is the document plus the warning
// count that came with it, so this wraps the call.

/// The result of an ENI export: the document, and how many parts the server could not read.
export interface EniExport {
  /// The ENI document, conforming to ENI Schema 1.7.
  xml: string
  /// Parts that were asked for and not answered, from `X-Eni-Warnings`. Each one costs an optional
  /// element, never the document, so a non-zero count still describes a bus a master can bring up.
  /// The server logs what each one was.
  warnings: number
}

/// Exports the bus as an ENI document (`GET /api/eni`). `baseUrl` is the HTTP API origin (e.g.
/// `https://host:61447`). Throws on a non-OK response, with the response body's `error` message
/// when there is one — a bus that has not reached SAFE-OP or OP answers 409, because before that
/// there is no mapping to describe. Pass `fetch` to inject an implementation (Node, request
/// logging).
export async function fetchEni(
  baseUrl: string,
  options: { fetch?: typeof fetch; signal?: AbortSignal } = {},
): Promise<EniExport> {
  const doFetch = options.fetch ?? fetch
  const res = await doFetch(`${baseUrl.replace(/\/+$/, '')}/api/eni`, {
    headers: { Accept: 'application/xml' },
    signal: options.signal,
  })
  if (!res.ok) {
    let message = `HTTP ${res.status}`
    try {
      const body = (await res.json()) as { error?: string }
      if (body?.error) {
        message = body.error
      }
    } catch {
      // Non-JSON error body — keep the status message.
    }
    throw new Error(message)
  }
  const warnings = Number.parseInt(res.headers.get('X-Eni-Warnings') ?? '0', 10)
  return {
    xml: await res.text(),
    warnings: Number.isFinite(warnings) ? warnings : 0,
  }
}

// --- Parsed documents -------------------------------------------------------------------------
//
// The shapes `POST /api/eni/parse` returns. The generated client types `network` as a bare object,
// because the swagger schema says only that it is one — describing every element there would
// duplicate the C++ model in YAML. These are that description, written once, where a renderer can
// use them.

/// A byte payload as the document writes it, with its length alongside.
export interface EniBytes {
  hex: string
  bytes: number
}

/// What an init command's address selects, and what its payload means. Present only where the
/// address matches a register the server knows.
export interface EniRegisterAnnotation {
  /// Catalogue name, e.g. `sm2`, `fmmu0`, `al_control`.
  name: string
  description: string
  /// Which channel or entity, for a register block that repeats.
  instance?: number
  /// Fields of a sync-manager or FMMU block. Absent for any other register.
  decoded?: Record<string, number | boolean | string>
  /// The AL state a write to AL Control asks for.
  requestsState?: string
  /// The AL state a read of AL Status waits for.
  waitsForState?: string
}

/// One EtherCAT datagram, whether the master sends it at a transition or every cycle.
///
/// The two differ only in when they are sent and in three fields: an init command names the
/// `transitions` it runs at, while a cyclic command names the AL `states` it is sent in and the
/// offsets its data occupies in the process image.
export interface EniInitCmd {
  transitions?: string[]
  /// AL states a cyclic command is sent in. Absent on an init command.
  states?: string[]
  /// Where a cyclic command's read data lands in the input image, in bytes.
  inputOffs?: number
  /// Where a cyclic command's written data comes from in the output image, in bytes.
  outputOffs?: number
  cmd: number
  /// The datagram's mnemonic, e.g. `FPWR`.
  cmdName: string
  beforeSlave?: boolean
  comment?: string
  requires?: string
  adp?: number
  ado?: number
  addr?: number
  data?: EniBytes
  dataLength?: number
  cnt?: number
  retries?: number
  timeoutMs?: number
  validate?: { data: EniBytes; dataMask?: EniBytes; timeoutMs: number }
  register?: EniRegisterAnnotation
}

/// One CoE transfer, which lives under a device's mailbox rather than beside its datagrams.
export interface EniCoeCmd {
  transitions: string[]
  timeoutMs: number
  ccs: number
  ccsName: 'download' | 'upload'
  index: number
  subindex: number
  comment?: string
  data?: EniBytes
  disabled?: boolean
}

/// One object mapped into a PDO. An `index` of zero is padding: it occupies `bitLen` bits and
/// addresses nothing, so it carries neither a name nor a type.
export interface EniPdoEntry {
  index: number
  subindex: number
  bitLen: number
  name?: string
  dataType?: string
  comment?: string
}

/// One process-data object. This is the only place in an ENI where a mapped value's object address
/// lives: the process image says where a value sits and what it is called, and a PDO says which CoE
/// object it is.
export interface EniPdo {
  index: number
  name: string
  /// The Sync Manager carrying it. Set means the PDO is part of the process image.
  syncManager?: number
  fixed?: boolean
  mandatory?: boolean
  entries: EniPdoEntry[]
}

export interface EniSyncManager {
  index: number
  type: string
  startAddress: number
  controlByte: number
  enable: boolean
  minSize?: number
  maxSize?: number
  defaultSize?: number
  watchdog?: number
}

export interface EniMailboxWindow {
  start: number
  length: number
  pollTime?: number
  statusBitAddr?: number
}

export interface EniSlave {
  info: {
    name: string
    physAddr: number
    autoIncAddr: number
    physics: string
    vendorId: number
    productCode: number
    revisionNo: number
    serialNo: number
  }
  processData?: {
    /// The window in the master's *output* image, named from the master's side.
    send?: { bitStart: number; bitLength: number }
    /// The window in the master's *input* image.
    recv?: { bitStart: number; bitLength: number }
    syncManagers: EniSyncManager[]
    /// Master-to-device PDOs, so the outputs.
    rxPdos?: EniPdo[]
    /// Device-to-master PDOs, so the inputs.
    txPdos?: EniPdo[]
  }
  mailbox?: {
    send: EniMailboxWindow
    recv: EniMailboxWindow
    bootstrap?: { send: EniMailboxWindow; recv: EniMailboxWindow }
    protocols: string[]
    coeInitCmds: EniCoeCmd[]
  }
  initCmds: EniInitCmd[]
  previousPorts?: { port: string; selected: boolean; physAddr?: number; deviceId?: number }[]
  dc?: {
    potentialReferenceClock?: boolean
    referenceClock?: boolean
    cycleTime0Ns?: number
    /// Not the SYNC1 cycle time: ETG.2100 defines it as SYNC1 cycle − SYNC0 cycle + SYNC0 shift.
    cycleTime1Ns?: number
    shiftTimeNs?: number
  }
}

export interface EniVariable {
  name: string
  bitSize: number
  bitOffs: number
  comment?: string
  dataType?: string
}

export interface EniNetwork {
  master: {
    name: string
    destination: string
    source: string
    etherType?: number
    mailboxStates?: { startAddr: number; count: number }
    eoe?: { maxPorts: number; maxFrames: number; maxMacs: number }
    initCmds: EniInitCmd[]
  }
  slaves: EniSlave[]
  cyclic?: {
    comment?: string
    taskId?: string
    cycleTimeUs?: number
    priority?: number
    frames: { comment?: string; cmds: EniInitCmd[] }[]
  }
  processImage?: {
    inputs?: { byteSize: number; variables: EniVariable[] }
    outputs?: { byteSize: number; variables: EniVariable[] }
  }
}

/// What `POST /api/eni/parse` returns.
export interface EniParseResult {
  network: EniNetwork
  /// One line per element seen and not modelled, or per value that would not decode.
  warnings: string[]
  summary: { devices: number; datagrams: number; coeTransfers: number }
}

/// Parses an ENI document (`POST /api/eni/parse`). `baseUrl` is the HTTP API origin. The document
/// need not have come from this server — reading one another tool wrote is the point. Throws on a
/// non-OK response, with the server's `error` message when there is one.
export async function parseEni(
  baseUrl: string,
  xml: string,
  options: { fetch?: typeof fetch; signal?: AbortSignal } = {},
): Promise<EniParseResult> {
  const doFetch = options.fetch ?? fetch
  const res = await doFetch(`${baseUrl.replace(/\/+$/, '')}/api/eni/parse`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/xml' },
    body: xml,
    signal: options.signal,
  })
  if (!res.ok) {
    let message = `HTTP ${res.status}`
    try {
      const body = (await res.json()) as { error?: string }
      if (body?.error) {
        message = body.error
      }
    } catch {
      // Non-JSON error body — keep the status message.
    }
    throw new Error(message)
  }
  return (await res.json()) as EniParseResult
}
