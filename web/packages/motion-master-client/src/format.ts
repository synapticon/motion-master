// Formats a duration and a byte count for a person to read. The Python client formats both the
// same way, digit for digit: it rounds half up, as toFixed() and Math.round() do.

/**
 * Formats a duration in milliseconds with three decimals, such as `0.087 ms`, `874.010 ms` or
 * `2.745 s`: milliseconds below one second, and seconds from one second. Three decimals of a
 * millisecond are the microseconds that the server measures, so a fast transaction keeps its
 * digits. The Console, the debug log of both clients and the examples format every time with it.
 */
export function formatDuration(ms: number): string {
  const rounded = Number(ms.toFixed(3));
  if (rounded < 1000) {
    return `${rounded.toFixed(3)} ms`;
  }
  return `${(ms / 1000).toFixed(3)} s`;
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

/**
 * Formats the times of one request, such as `round-trip 3.557 ms, server 1.416 ms, fieldbus
 * 1.356 ms`: the round trip that the client measured, then the metrics `server` and `fieldbus` that
 * the server reported in `Server-Timing`. A time that is `null` or missing is left out. The Console
 * and the debug log of both clients write the times of a request with it.
 */
export function formatTimes(
  roundTripMs: number,
  options: { serverMs?: number | null; fieldbusMs?: number | null } = {},
): string {
  const parts = [`round-trip ${formatDuration(roundTripMs)}`];
  if (options.serverMs !== undefined && options.serverMs !== null) {
    parts.push(`server ${formatDuration(options.serverMs)}`);
  }
  if (options.fieldbusMs !== undefined && options.fieldbusMs !== null) {
    parts.push(`fieldbus ${formatDuration(options.fieldbusMs)}`);
  }
  return parts.join(', ');
}
