// Formats a byte count with binary prefixes — the unit these values actually are, since every size
// the API reports is a real byte count and each step divides by 1024. Labelling that "KB" would be
// wrong by 2.4% at the first step and worse after.
//
// One implementation so the same file reads identically wherever it is listed: the User Cache,
// Parameter Cache and Recorder pages all show file sizes, and previously each rounded differently
// (an 89,088-byte file read "87.0 KB" on one page and "87 KB" on another).
// Formats an epoch-microsecond timestamp as a local time of day with milliseconds.
//
// Microseconds are the unit this API uses for anything sampled per cycle, because milliseconds
// cannot tell two cycles of a 1 ms loop apart. Rendered local and to the millisecond so it lines up
// with the server log, whose prefix is also local — placing a fault against the log is the whole
// reason these timestamps are served.
export function formatEpochUs(epochUs: number): string {
  const date = new Date(epochUs / 1000)
  const time = date.toLocaleTimeString(undefined, { hour12: false })
  return `${time}.${String(date.getMilliseconds()).padStart(3, '0')}`
}

export function formatBytes(bytes: number): string {
  const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB']
  let value = bytes
  let unit = 0
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024
    unit++
  }
  // Whole bytes need no decimal; every larger unit keeps one so a small difference stays visible.
  return `${value.toFixed(unit === 0 ? 0 : 1)} ${units[unit]}`
}
