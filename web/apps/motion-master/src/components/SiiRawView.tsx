import HexViewer from './HexViewer'

// Length of the meaningful SII content: everything up to and including the END category marker
// (0xFFFF). Walks the category list from the 128-byte header exactly as the parser does, so the
// dump stops where the data does — everything after is unprogrammed flash (0xFF). Falls back to the
// full buffer if no END marker is found (a malformed or unterminated image).
const SII_HEADER_BYTES = 128
export function siiContentLength(bytes: Uint8Array): number {
  if (bytes.length < SII_HEADER_BYTES) {
    return bytes.length
  }
  let off = SII_HEADER_BYTES
  while (off + 4 <= bytes.length) {
    const type = bytes[off] | (bytes[off + 1] << 8)
    if (type === 0xffff) {
      return off + 2 // include the 2-byte END marker itself
    }
    const wordSize = bytes[off + 2] | (bytes[off + 3] << 8)
    off += 4 + wordSize * 2
  }
  return bytes.length
}

// Hex dump of a raw SII image, trimmed at the END marker so trailing unprogrammed flash (0xFF) is
// hidden. Shared by the device SII page and the Tools SII page.
export default function SiiRawView({ bytes }: { bytes: Uint8Array }) {
  const contentLen = siiContentLength(bytes)
  const hidden = bytes.length - contentLen
  return (
    <div className="space-y-2">
      <p className="text-xs text-grey-500">
        {bytes.length} bytes total; {contentLen} bytes of SII content
        {hidden > 0 && ` (${hidden} unprogrammed 0xFF bytes hidden)`}.
      </p>
      <HexViewer bytes={bytes.subarray(0, contentLen)} />
    </div>
  )
}
