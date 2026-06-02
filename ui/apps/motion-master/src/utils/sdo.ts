// Encoding and decoding helpers for CoE SDO values: turning the raw little-endian
// byte arrays exchanged over the wire into human-readable values and back.
import { parseHexBytes } from './hex'

// CoE data types the SDO download card can encode a value into. `raw` is an
// escape hatch for entering bytes directly.
export const SDO_TYPES = [
  'uint8', 'int8', 'uint16', 'int16', 'uint32', 'int32',
  'uint64', 'int64', 'float32', 'float64', 'string', 'raw',
] as const
export type SdoType = (typeof SDO_TYPES)[number]

// Placeholder text shown in the value input for each type.
export const SDO_TYPE_HINT: Record<SdoType, string> = {
  uint8: 'e.g. 200 or 0xC8',
  int8: 'e.g. -42',
  uint16: 'e.g. 1000 or 0x03E8',
  int16: 'e.g. -1000',
  uint32: 'e.g. 100000',
  int32: 'e.g. -100000',
  uint64: 'e.g. 4294967296',
  int64: 'e.g. -4294967296',
  float32: 'e.g. 3.14',
  float64: 'e.g. 3.141592653589793',
  string: 'e.g. SOMANET',
  raw: 'hex bytes, e.g. 01 02 0A FF',
}

const INT_ENC: Partial<Record<SdoType, { size: number; signed: boolean }>> = {
  uint8: { size: 1, signed: false }, int8: { size: 1, signed: true },
  uint16: { size: 2, signed: false }, int16: { size: 2, signed: true },
  uint32: { size: 4, signed: false }, int32: { size: 4, signed: true },
  uint64: { size: 8, signed: false }, int64: { size: 8, signed: true },
}

// Accepts decimal (with optional leading '-') or 0x-prefixed hex; returns null on anything else.
function parseBigIntFlexible(s: string): bigint | null {
  if (/^-?[0-9]+$/.test(s)) return BigInt(s)
  if (/^0[xX][0-9a-fA-F]+$/.test(s)) return BigInt(s)
  if (/^-0[xX][0-9a-fA-F]+$/.test(s)) return -BigInt(s.slice(1))
  return null
}

// Encodes a human-entered value into the little-endian byte sequence for an SDO download.
export function encodeSdoValue(type: SdoType, raw: string): { bytes: number[] } | { error: string } {
  const s = raw.trim()

  const meta = INT_ENC[type]
  if (meta) {
    const n = parseBigIntFlexible(s)
    if (n === null) return { error: 'Not a valid integer' }
    const bits = BigInt(meta.size * 8)
    const min = meta.signed ? -(1n << (bits - 1n)) : 0n
    const max = meta.signed ? (1n << (bits - 1n)) - 1n : (1n << bits) - 1n
    if (n < min || n > max) return { error: `Out of range for ${type} [${min}, ${max}]` }
    const view = new DataView(new ArrayBuffer(meta.size))
    if (meta.size === 1) meta.signed ? view.setInt8(0, Number(n)) : view.setUint8(0, Number(n))
    else if (meta.size === 2) meta.signed ? view.setInt16(0, Number(n), true) : view.setUint16(0, Number(n), true)
    else if (meta.size === 4) meta.signed ? view.setInt32(0, Number(n), true) : view.setUint32(0, Number(n), true)
    else meta.signed ? view.setBigInt64(0, n, true) : view.setBigUint64(0, n, true)
    return { bytes: [...new Uint8Array(view.buffer)] }
  }

  if (type === 'float32' || type === 'float64') {
    if (s === '') return { error: 'Value is empty' }
    const f = Number(s)
    if (Number.isNaN(f)) return { error: 'Not a valid number' }
    const size = type === 'float32' ? 4 : 8
    const view = new DataView(new ArrayBuffer(size))
    if (type === 'float32') view.setFloat32(0, f, true)
    else view.setFloat64(0, f, true)
    return { bytes: [...new Uint8Array(view.buffer)] }
  }

  if (type === 'string') {
    return { bytes: [...new TextEncoder().encode(raw)] }
  }

  // raw: whitespace- or comma-separated hex bytes
  const bytes = parseHexBytes(s)
  if (bytes === null) return { error: 'Not a valid even-length hex byte string' }
  return { bytes }
}

export interface SdoInterpretation { label: string; value: string }

// Best-effort interpretation of raw SDO bytes as every fixed-width type that fits,
// plus a printable-string reading. Used to show what an uploaded value could mean.
export function interpretSdoBytes(bytes: number[]): SdoInterpretation[] {
  const view = new DataView(new Uint8Array(bytes).buffer)
  const out: SdoInterpretation[] = []

  if (bytes.length === 1) {
    out.push({ label: 'int8',  value: String(view.getInt8(0)) })
    out.push({ label: 'uint8', value: String(view.getUint8(0)) })
  } else if (bytes.length === 2) {
    out.push({ label: 'int16 LE',  value: String(view.getInt16(0, true)) })
    out.push({ label: 'uint16 LE', value: String(view.getUint16(0, true)) })
  } else if (bytes.length === 4) {
    out.push({ label: 'int32 LE',   value: String(view.getInt32(0, true)) })
    out.push({ label: 'uint32 LE',  value: String(view.getUint32(0, true)) })
    out.push({ label: 'float32 LE', value: String(view.getFloat32(0, true)) })
  } else if (bytes.length === 8) {
    out.push({ label: 'int64 LE',   value: view.getBigInt64(0, true).toString() })
    out.push({ label: 'uint64 LE',  value: view.getBigUint64(0, true).toString() })
    out.push({ label: 'float64 LE', value: String(view.getFloat64(0, true)) })
  }

  if (bytes.length > 0 && bytes.every(b => (b >= 0x20 && b <= 0x7e) || b === 0)) {
    // VISIBLE_STRING is space-padded to a fixed width and may carry a terminating
    // NUL — neither is significant, so strip trailing 0x20/0x00 before display.
    let len = bytes.length
    while (len > 0 && (bytes[len - 1] === 0x00 || bytes[len - 1] === 0x20)) len--
    const str = new TextDecoder().decode(new Uint8Array(bytes.slice(0, len).filter(b => b !== 0)))
    out.push({ label: 'string', value: `"${str}"` })
  }

  return out
}

// Decodes raw SDO bytes into a typed value, given the object's CoE data-type name.
// Falls back to the raw byte array for unknown types or short reads.
export function decodeSdoBytes(dataTypeName: string, bytes: number[]): number | string | number[] {
  const view = new DataView(new Uint8Array(bytes).buffer)
  switch (dataTypeName) {
    case 'BOOLEAN':
    case 'UNSIGNED8':
    case 'BYTE':
      if (bytes.length >= 1) return view.getUint8(0)
      break
    case 'INTEGER8':
      if (bytes.length >= 1) return view.getInt8(0)
      break
    case 'INTEGER16':
      if (bytes.length >= 2) return view.getInt16(0, true)
      break
    case 'INTEGER32':
      if (bytes.length >= 4) return view.getInt32(0, true)
      break
    case 'UNSIGNED16':
    case 'WORD':
      if (bytes.length >= 2) return view.getUint16(0, true)
      break
    case 'UNSIGNED32':
    case 'DWORD':
      if (bytes.length >= 4) return view.getUint32(0, true)
      break
    case 'REAL32':
      if (bytes.length >= 4) return view.getFloat32(0, true)
      break
    case 'REAL64':
      if (bytes.length >= 8) return view.getFloat64(0, true)
      break
    case 'INTEGER64':
      if (bytes.length >= 8) return Number(view.getBigInt64(0, true))
      break
    case 'UNSIGNED64':
      if (bytes.length >= 8) return Number(view.getBigUint64(0, true))
      break
    case 'VISIBLE_STRING':
    case 'UNICODE_STRING':
      return new TextDecoder().decode(new Uint8Array(bytes.filter(b => b !== 0)))
  }
  return bytes
}
