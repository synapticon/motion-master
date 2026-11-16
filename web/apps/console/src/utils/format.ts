// Formats an epoch-microsecond timestamp as a local time of day with milliseconds.
//
// Microseconds are the unit this API uses for anything sampled per cycle, because milliseconds
// cannot tell two cycles of a 1 ms loop apart. Rendered local and to the millisecond so it lines up
// with the server log, whose prefix is also local — placing a fault against the log is the whole
// reason these timestamps are served.
export function formatEpochUs(epochUs: number): string {
  const date = new Date(epochUs / 1000);
  const time = date.toLocaleTimeString(undefined, { hour12: false });
  return `${time}.${String(date.getMilliseconds()).padStart(3, '0')}`;
}
