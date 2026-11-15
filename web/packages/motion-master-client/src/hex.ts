// Format a number as an uppercase hex string, zero-padded to `width` digits. With `prefix`
// (the default) it carries the conventional "0x"; pass false for bare digits — e.g. a subindex
// rendered after a colon ("0x6040:00"). The display inverse of parseHexBytes.
export function formatHex(value: number, width = 4, prefix = true): string {
  return `${prefix ? '0x' : ''}${value.toString(16).toUpperCase().padStart(width, '0')}`;
}

/**
 * Formats bytes as hex in capitals, two digits each, such as `0A FF`. The display inverse of
 * parseHexBytes. Python writes the same with `data.hex(" ").upper()`.
 */
export function formatHexBytes(bytes: Uint8Array | readonly number[], separator = ' '): string {
  return Array.from(bytes, (b) => b.toString(16).toUpperCase().padStart(2, '0')).join(separator);
}

// Parse a string of hex bytes ("0A FF", "0x0AFF", "0a,ff") into a byte array.
// Returns null when the input is empty or not a valid even-length hex string.
export function parseHexBytes(input: string): number[] | null {
  const cleaned = input.replace(/0x/gi, '').replace(/[\s,]+/g, '');
  if (cleaned.length === 0 || cleaned.length % 2 !== 0) {
    return null;
  }
  if (!/^[0-9a-fA-F]+$/.test(cleaned)) {
    return null;
  }
  const bytes: number[] = [];
  for (let i = 0; i < cleaned.length; i += 2) {
    bytes.push(parseInt(cleaned.slice(i, i + 2), 16));
  }
  return bytes;
}
