// The times that the server measured for a request, from the standard Server-Timing header.
//
// Every response carries the metric `server`, the time from the complete request to the response.
// An endpoint that runs a fieldbus operation (SDO, FoE, an AL state change, ...) also carries
// `fieldbus`, the time of that operation: the wait for the control-plane lock and every mailbox or
// ESC transaction. Both are in milliseconds: `Server-Timing: server;dur=7.100, fieldbus;dur=6.200`.
// A client subtracts them from its own round trip to see where the time went. A browser records the
// header per request in its Performance API, so a page reads the times of a request that it made
// through any code; a program that holds the response reads them from there.

/** Where a time is read from: a response, or the entry of the request in the Performance API. */
type TimingSource = Response | PerformanceResourceTiming;

/**
 * Returns the time that the server spent on a request in milliseconds, the metric `server`, or
 * `null` when the server reported none.
 *
 * `source` is the response, whose `Server-Timing` header holds the time, or the entry of the
 * request in the browser's Performance API, a `PerformanceResourceTiming`.
 */
export function serverTimeMs(source: TimingSource): number | null {
  return metricMs(source, 'server');
}

/**
 * Returns the time of the fieldbus operation of a request in milliseconds, the metric `fieldbus`,
 * or `null` when the request ran none.
 *
 * `source` is the response, or the entry of the request in the browser's Performance API.
 */
export function fieldbusTimeMs(source: TimingSource): number | null {
  return metricMs(source, 'fieldbus');
}

function metricMs(source: TimingSource, name: string): number | null {
  if ('serverTiming' in source) {
    const metric = source.serverTiming.find((m) => m.name === name);
    return metric === undefined ? null : metric.duration;
  }
  const header = source.headers.get('Server-Timing');
  return header === null ? null : serverTimingDuration(header, name);
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
