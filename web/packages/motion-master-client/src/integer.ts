// An optional sign, then decimal, or hex, octal or binary with a 0x, 0o or 0b prefix. A decimal
// number has no leading zero, so 010 is refused, as Python refuses it: it reads as octal in C.
const INTEGER = /^([+-]?)(0[xX][0-9a-fA-F]+|0[oO][0-7]+|0[bB][01]+|[1-9][0-9]*|0+)$/;

/**
 * Parses an integer as Python's `int(text, 0)` does, without its underscores: decimal, or hex,
 * octal or binary with a `0x`, `0o` or `0b` prefix, each with an optional sign, and with white
 * space around it. `-0x10` is -16. Returns a `bigint`, so a 64-bit value keeps every digit, or
 * null when the text is not such an integer.
 */
export function parseBigInt(text: string): bigint | null {
  const match = INTEGER.exec(text.trim());
  if (match === null) {
    return null;
  }
  const magnitude = BigInt(match[2]);
  return match[1] === '-' ? -magnitude : magnitude;
}

/**
 * Parses an integer as `parseBigInt()` does, and returns it as a number. Returns null when
 * the text is not an integer, when the integer is beyond 2^53 - 1, which a number cannot hold
 * exactly, and when it is below `min` or above `max`.
 */
export function parseInteger(
  text: string,
  options: { min?: number; max?: number } = {},
): number | null {
  const parsed = parseBigInt(text);
  if (parsed === null) {
    return null;
  }
  const value = Number(parsed);
  if (!Number.isSafeInteger(value)) {
    return null;
  }
  const { min, max } = options;
  if ((min !== undefined && value < min) || (max !== undefined && value > max)) {
    return null;
  }
  return value;
}
