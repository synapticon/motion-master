// Formats a duration and a byte count for a person to read. The Python client formats both the
// same way, digit for digit: it rounds half up, as toFixed() and Math.round() do.

/**
 * Formats a duration in milliseconds, such as `4.2 ms`, `310 ms` or `2.75 s`. Below 10 ms it keeps
 * one decimal, so that a fast transaction does not show as `0 ms`. From 1000 ms it gives seconds
 * with two decimals.
 */
export function formatDuration(ms: number): string {
  if (ms < 10) {
    return `${ms.toFixed(1)} ms`;
  }
  if (ms < 1000) {
    return `${Math.round(ms)} ms`;
  }
  return `${(ms / 1000).toFixed(2)} s`;
}

/**
 * Formats a byte count with binary prefixes, such as `512 B` or `87.0 KiB`. Each step divides by
 * 1024, so the prefixes are KiB, MiB, GiB and TiB, not KB. Bytes have no decimal, and every larger
 * unit has one.
 */
export function formatBytes(bytes: number): string {
  const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB'];
  let value = bytes;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit++;
  }
  return `${value.toFixed(unit === 0 ? 0 : 1)} ${units[unit]}`;
}
