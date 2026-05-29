// Parse a string of hex bytes ("0A FF", "0x0AFF", "0a,ff") into a byte array.
// Returns null when the input is empty or not a valid even-length hex string.
export function parseHexBytes(input: string): number[] | null {
  const cleaned = input.replace(/0x/gi, '').replace(/[\s,]+/g, '')
  if (cleaned.length === 0 || cleaned.length % 2 !== 0) return null
  if (!/^[0-9a-fA-F]+$/.test(cleaned)) return null
  const bytes: number[] = []
  for (let i = 0; i < cleaned.length; i += 2) {
    bytes.push(parseInt(cleaned.slice(i, i + 2), 16))
  }
  return bytes
}
