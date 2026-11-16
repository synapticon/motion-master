// The server-measured wire time of a request.
//
// An endpoint that performs a fieldbus transaction (SDO, FoE, ...) reports the on-device cost —
// control-plane lock acquire plus the mailbox/ESC round-trip — in the standard Server-Timing
// header, as the metric `wire` in milliseconds: `Server-Timing: wire;dur=1.500`. A client subtracts
// it from its own HTTP round-trip to separate the device cost from the browser and the transport.
// A browser records the header per request in its Performance API, so a page reads the time of a
// request that it made through any code; a program that holds the response reads it from there.

/** The name of the metric in `Server-Timing` that carries the wire time. */
const WIRE_METRIC = 'wire';

/**
 * Returns the wire time of a request in milliseconds, or `null` when the server reported none.
 *
 * `source` is the response, whose `Server-Timing` header holds the time, or the entry of the
 * request in the browser's Performance API, a `PerformanceResourceTiming`.
 */
export function wireTimeMs(source: Response | PerformanceResourceTiming): number | null {
  if ('serverTiming' in source) {
    const metric = source.serverTiming.find((m) => m.name === WIRE_METRIC);
    return metric === undefined ? null : metric.duration;
  }
  const header = source.headers.get('Server-Timing');
  return header === null ? null : serverTimingDuration(header, WIRE_METRIC);
}

/** Returns the `dur` of one metric of a `Server-Timing` value, or `null` when it has none. */
function serverTimingDuration(header: string, name: string): number | null {
  for (const entry of header.split(',')) {
    const [metric, ...parameters] = entry.split(';').map((part) => part.trim());
    if (metric !== name) {
      continue;
    }
    for (const parameter of parameters) {
      const [key, value] = parameter.split('=').map((part) => part.trim());
      if (key === 'dur') {
        const duration = Number(value);
        return Number.isFinite(duration) ? duration : null;
      }
    }
    return null;
  }
  return null;
}
