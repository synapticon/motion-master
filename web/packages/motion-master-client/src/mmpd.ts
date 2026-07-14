// Parser for the `.mmpd` process-data recorder dump (see libs/node/process_data_dump.{h,cc}). The
// format embeds the process image as a header so a run decodes fully offline — no running Motion
// Master and no live bus. All multi-byte integers are little-endian.
//
// Layout:
//   Prefix (36 bytes): magic 'MMPD', u16 formatVersion, u16 flags,
//     u64 startSequence, u64 rowCount, u32 inputBytes, u32 outputBytes, u32 deviceCount.
//   Device table (deviceCount): u16 slavePosition, u32 vendorId/productCode/revisionNumber/
//     serialNumber, str name, u32 entryCount, then per entry: u16 index, u8 subindex,
//     u8 direction (1=output/RxPDO, 0=input/TxPDO), u16 dataType, u16 bitLength, u32 bitOffset,
//     str name. (str = u16 byte length + UTF-8 bytes.)
//   Rows (rowCount, fixed stride 16 + inputBytes + outputBytes): u64 sequence, u64 timestampNs,
//     inputs[inputBytes], outputs[outputBytes].

const MAGIC = 0x4450_4d4d // 'MMPD' as a little-endian u32 ('M','M','P','D')
const SUPPORTED_FORMAT_VERSION = 1

/// One PDO-mapped object recorded in the dump header.
export interface MmpdPdoEntry {
  index: number
  subindex: number
  /// True for an RxPDO output (master→slave), false for a TxPDO input (slave→master).
  isOutput: boolean
  /// ETG.1020 data-type code, or 0 when the object dictionary was not enumerated (undecodable).
  dataType: number
  bitLength: number
  /// Absolute bit offset within its direction's image.
  bitOffset: number
  /// Object name, or empty when the OD was not enumerated.
  name: string
}

/// One device's identity and the PDO objects it contributes to the image.
export interface MmpdDevice {
  slavePosition: number
  vendorId: number
  productCode: number
  revisionNumber: number
  serialNumber: number
  name: string
  entries: MmpdPdoEntry[]
}

/// The embedded process-image header — everything needed to decode the rows offline.
export interface MmpdHeader {
  formatVersion: number
  flags: number
  /// Sequence number of the first row (oldest cycle in the span).
  startSequence: bigint
  /// Number of rows in the file.
  rowCount: number
  inputBytes: number
  outputBytes: number
  devices: MmpdDevice[]
}

/// One recorded cycle. `inputs`/`outputs` are views into the source buffer (no copy).
export interface MmpdRow {
  sequence: bigint
  timestampNs: bigint
  inputs: Uint8Array
  outputs: Uint8Array
}

const utf8 = new TextDecoder()

// Cursor over a DataView for sequential little-endian reads while parsing the header.
class Cursor {
  offset: number
  constructor(
    private readonly dv: DataView,
    private readonly u8: Uint8Array,
    offset = 0,
  ) {
    this.offset = offset
  }
  u8val(): number {
    return this.dv.getUint8(this.offset++)
  }
  u16(): number {
    const v = this.dv.getUint16(this.offset, true)
    this.offset += 2
    return v
  }
  u32(): number {
    const v = this.dv.getUint32(this.offset, true)
    this.offset += 4
    return v
  }
  u64(): bigint {
    const v = this.dv.getBigUint64(this.offset, true)
    this.offset += 8
    return v
  }
  str(): string {
    const len = this.u16()
    const s = utf8.decode(this.u8.subarray(this.offset, this.offset + len))
    this.offset += len
    return s
  }
}

/// A parsed `.mmpd` dump. The header is read eagerly; row values are decoded on demand so a
/// large file (a deep recorder ring) is not fully materialised up front.
export class MmpdFile {
  constructor(
    readonly header: MmpdHeader,
    private readonly u8: Uint8Array,
    private readonly dv: DataView,
    /// Byte offset of the first row within the buffer.
    private readonly rowsStart: number,
  ) {}

  get rowCount(): number {
    return this.header.rowCount
  }

  /// Per-row byte stride: u64 sequence + u64 timestampNs + inputs + outputs.
  get rowStride(): number {
    return 16 + this.header.inputBytes + this.header.outputBytes
  }

  /// The raw record at row `i` (views into the source buffer; do not retain past the buffer's life).
  rowAt(i: number): MmpdRow {
    const base = this.rowsStart + i * this.rowStride
    const inputsAt = base + 16
    const outputsAt = inputsAt + this.header.inputBytes
    return {
      sequence: this.dv.getBigUint64(base, true),
      timestampNs: this.dv.getBigUint64(base + 8, true),
      inputs: this.u8.subarray(inputsAt, outputsAt),
      outputs: this.u8.subarray(outputsAt, outputsAt + this.header.outputBytes),
    }
  }

  /// Epoch microseconds for every row (JS-exact in a Number until ~year 2255, matching the live
  /// monitoring protocol). Sub-microsecond nanoseconds are dropped.
  timestampsUs(): Float64Array {
    const out = new Float64Array(this.header.rowCount)
    for (let i = 0; i < out.length; i++) {
      out[i] = Number(this.dv.getBigUint64(this.rowsStart + i * this.rowStride + 8, true) / 1000n)
    }
    return out
  }

  /// Decodes one entry's value for every row. Numeric types yield numbers (64-bit ints are coerced
  /// to Number, lossy beyond 2^53); strings, undecodable (dataType 0), and unsupported types yield
  /// null — so the result is always plottable.
  decodeSeries(entry: MmpdPdoEntry): (number | null)[] {
    const out: (number | null)[] = new Array(this.header.rowCount)
    const reader = numericReader(entry.dataType)
    const byteAligned = entry.bitOffset % 8 === 0 && entry.bitLength % 8 === 0
    const regionStart = entry.isOutput ? 16 + this.header.inputBytes : 16

    if (reader && byteAligned) {
      // Fast path: read straight from the buffer with no per-row allocation.
      const valueOffset = regionStart + (entry.bitOffset >> 3)
      for (let i = 0; i < out.length; i++) {
        const v = reader(this.dv, this.rowsStart + i * this.rowStride + valueOffset)
        out[i] = typeof v === 'bigint' ? Number(v) : v
      }
      return out
    }

    // Slow path: sub-byte fields and strings — extract the bits, then decode.
    for (let i = 0; i < out.length; i++) {
      const row = this.rowAt(i)
      const bytes = extractBits(entry.isOutput ? row.outputs : row.inputs, entry.bitOffset, entry.bitLength)
      const v = decodeMmpdValue(entry.dataType, bytes)
      out[i] = typeof v === 'number' ? v : typeof v === 'bigint' ? Number(v) : null
    }
    return out
  }
}

/// Parses a `.mmpd` dump. Throws on a bad magic or an unsupported format version. A file whose row
/// region is shorter than `rowCount` implies (truncated transfer) is clamped to the rows present.
export function parseMmpd(data: ArrayBuffer | Uint8Array): MmpdFile {
  const u8 = data instanceof Uint8Array ? data : new Uint8Array(data)
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength)

  if (u8.byteLength < 36 || dv.getUint32(0, true) !== MAGIC) {
    throw new Error('Not a .mmpd file (bad magic)')
  }
  const formatVersion = dv.getUint16(4, true)
  if (formatVersion !== SUPPORTED_FORMAT_VERSION) {
    throw new Error(`Unsupported .mmpd format version ${formatVersion} (expected ${SUPPORTED_FORMAT_VERSION})`)
  }

  const header: MmpdHeader = {
    formatVersion,
    flags: dv.getUint16(6, true),
    startSequence: dv.getBigUint64(8, true),
    rowCount: Number(dv.getBigUint64(16, true)),
    inputBytes: dv.getUint32(24, true),
    outputBytes: dv.getUint32(28, true),
    devices: [],
  }
  const deviceCount = dv.getUint32(32, true)

  const cursor = new Cursor(dv, u8, 36)
  for (let d = 0; d < deviceCount; d++) {
    const device: MmpdDevice = {
      slavePosition: cursor.u16(),
      vendorId: cursor.u32(),
      productCode: cursor.u32(),
      revisionNumber: cursor.u32(),
      serialNumber: cursor.u32(),
      name: cursor.str(),
      entries: [],
    }
    const entryCount = cursor.u32()
    for (let e = 0; e < entryCount; e++) {
      device.entries.push({
        index: cursor.u16(),
        subindex: cursor.u8val(),
        isOutput: cursor.u8val() === 1,
        dataType: cursor.u16(),
        bitLength: cursor.u16(),
        bitOffset: cursor.u32(),
        name: cursor.str(),
      })
    }
    header.devices.push(device)
  }

  const rowsStart = cursor.offset
  const rowStride = 16 + header.inputBytes + header.outputBytes
  const available = Math.floor((u8.byteLength - rowsStart) / rowStride)
  if (available < header.rowCount) {
    header.rowCount = Math.max(0, available)
  }

  return new MmpdFile(header, u8, dv, rowsStart)
}

// --- value decoding ----------------------------------------------------------

// ETG.1020 data-type codes needed for decoding. See libs/comm/object_data_types.h.
const enum DataType {
  BOOLEAN = 0x0001,
  INTEGER8 = 0x0002,
  INTEGER16 = 0x0003,
  INTEGER32 = 0x0004,
  UNSIGNED8 = 0x0005,
  UNSIGNED16 = 0x0006,
  UNSIGNED32 = 0x0007,
  REAL32 = 0x0008,
  VISIBLE_STRING = 0x0009,
  UNICODE_STRING = 0x000b,
  REAL64 = 0x0011,
  INTEGER64 = 0x0015,
  UNSIGNED64 = 0x001b,
  BYTE = 0x001e,
  WORD = 0x001f,
  DWORD = 0x0020,
  BITARR8 = 0x002d,
  BITARR16 = 0x002e,
  BITARR32 = 0x002f,
}

type ScalarReader = (dv: DataView, offset: number) => number | bigint

// A byte-aligned reader for the fixed-width numeric types, or null for variable/sub-byte/string
// types that need the bit-extraction path.
function numericReader(dataType: number): ScalarReader | null {
  switch (dataType) {
    case DataType.BOOLEAN:
    case DataType.UNSIGNED8:
    case DataType.BYTE:
    case DataType.BITARR8:
      return (dv, o) => dv.getUint8(o)
    case DataType.INTEGER8:
      return (dv, o) => dv.getInt8(o)
    case DataType.INTEGER16:
      return (dv, o) => dv.getInt16(o, true)
    case DataType.UNSIGNED16:
    case DataType.WORD:
    case DataType.BITARR16:
      return (dv, o) => dv.getUint16(o, true)
    case DataType.INTEGER32:
      return (dv, o) => dv.getInt32(o, true)
    case DataType.UNSIGNED32:
    case DataType.DWORD:
    case DataType.BITARR32:
      return (dv, o) => dv.getUint32(o, true)
    case DataType.REAL32:
      return (dv, o) => dv.getFloat32(o, true)
    case DataType.REAL64:
      return (dv, o) => dv.getFloat64(o, true)
    case DataType.INTEGER64:
      return (dv, o) => dv.getBigInt64(o, true)
    case DataType.UNSIGNED64:
      return (dv, o) => dv.getBigUint64(o, true)
    default:
      return null
  }
}

/// Fetches the live recorder dump (`GET /api/process-data/dump`) and parses it. `baseUrl` is the
/// HTTP API origin (e.g. `https://host:61447`). Throws on a non-OK response (the response body's
/// `error` message when present). Pass `fetch` to inject an implementation (Node, request logging).
export async function fetchProcessDataDump(
  baseUrl: string,
  options: { fetch?: typeof fetch; signal?: AbortSignal } = {},
): Promise<MmpdFile> {
  const doFetch = options.fetch ?? fetch
  const res = await doFetch(`${baseUrl.replace(/\/+$/, '')}/api/process-data/dump`, {
    headers: { Accept: 'application/octet-stream' },
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
  return parseMmpd(await res.arrayBuffer())
}

/// Extracts `bitLength` bits starting at `bitOffset` from `src`, returning them LSB-aligned and
/// little-endian (padded to a byte boundary). Mirrors `extractBits` in libs/node/process_image.cc.
export function extractBits(src: Uint8Array, bitOffset: number, bitLength: number): Uint8Array {
  const out = new Uint8Array((bitLength + 7) >> 3)
  if (bitOffset % 8 === 0 && bitLength % 8 === 0) {
    const byteOffset = bitOffset >> 3
    for (let i = 0; i < out.length && byteOffset + i < src.length; i++) {
      out[i] = src[byteOffset + i]
    }
    return out
  }
  for (let i = 0; i < bitLength; i++) {
    const srcBit = bitOffset + i
    const srcByte = srcBit >> 3
    if (srcByte >= src.length) {
      break
    }
    const bit = (src[srcByte] >> (srcBit & 7)) & 1
    out[i >> 3] |= bit << (i & 7)
  }
  return out
}

/// Decodes LSB-aligned little-endian `bytes` (as produced by `extractBits`) for the given ETG.1020
/// `dataType`. Returns a number for fixed-width numerics, a bigint for 64-bit integers, a string
/// for VISIBLE/UNICODE strings (trailing NULs/spaces stripped), or null when the type is unknown
/// (dataType 0) or there are too few bytes. Unmapped codes fall back to an unsigned read of the
/// available bytes (covers BITn fields).
export function decodeMmpdValue(dataType: number, bytes: Uint8Array): number | bigint | string | null {
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)
  const n = bytes.byteLength
  switch (dataType) {
    case DataType.BOOLEAN:
    case DataType.UNSIGNED8:
    case DataType.BYTE:
    case DataType.BITARR8:
      return n >= 1 ? dv.getUint8(0) : null
    case DataType.INTEGER8:
      return n >= 1 ? dv.getInt8(0) : null
    case DataType.INTEGER16:
      return n >= 2 ? dv.getInt16(0, true) : null
    case DataType.UNSIGNED16:
    case DataType.WORD:
    case DataType.BITARR16:
      return n >= 2 ? dv.getUint16(0, true) : null
    case DataType.INTEGER32:
      return n >= 4 ? dv.getInt32(0, true) : null
    case DataType.UNSIGNED32:
    case DataType.DWORD:
    case DataType.BITARR32:
      return n >= 4 ? dv.getUint32(0, true) : null
    case DataType.REAL32:
      return n >= 4 ? dv.getFloat32(0, true) : null
    case DataType.REAL64:
      return n >= 8 ? dv.getFloat64(0, true) : null
    case DataType.INTEGER64:
      return n >= 8 ? dv.getBigInt64(0, true) : null
    case DataType.UNSIGNED64:
      return n >= 8 ? dv.getBigUint64(0, true) : null
    case DataType.VISIBLE_STRING:
    case DataType.UNICODE_STRING: {
      let end = n
      while (end > 0 && (bytes[end - 1] === 0x00 || bytes[end - 1] === 0x20)) {
        end--
      }
      return utf8.decode(bytes.subarray(0, end))
    }
    case 0x0000:
      return null // OD not enumerated — undecodable
    default: {
      // Unknown/sub-byte (e.g. BIT1..BIT16): read the available bytes as a little-endian unsigned.
      if (n === 0 || n > 4) {
        return null
      }
      let v = 0
      for (let i = 0; i < n; i++) {
        v |= bytes[i] << (8 * i)
      }
      return v >>> 0
    }
  }
}

/// Whether `decodeSeries` yields plottable numbers for this ETG.1020 data-type code — a
/// fixed-width integer/real or a bit field, but not a string, struct, or unenumerated type
/// (dataType 0). Useful for offering only chartable objects in a UI.
export function isPlottableDataType(dataType: number): boolean {
  return numericReader(dataType) !== null || (dataType >= 0x0030 && dataType <= 0x003f)
}
