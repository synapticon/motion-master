import { useCallback, useState } from 'react';
import {
  fieldbusTimeMs,
  formatTimes,
  HttpError,
  serverTimeMs,
} from '@synapticon/motion-master-client';

// Shared readout of the times of one request: the round trip that the browser measured, then the
// metrics `server` and `fieldbus` that the server reports in Server-Timing, as `formatTimes` of the
// client writes them, so the Console reads the same as the debug log of the clients. Used by every
// page that shows the time of a request.

export interface RequestTimingValue {
  // The time of the fieldbus operation in ms, or null when the request ran none.
  fieldbusMs: number | null;
  // The time the server spent on the request in ms, or null when the response did not say.
  serverMs: number | null;
  // Browser-observed HTTP round-trip in ms (measured around the fetch).
  roundTripMs: number;
}

const TITLE =
  'round-trip: the time this browser measured around the HTTP request, with the upload, the download and the cross-origin and TLS overhead. server: the time the server spent on the request, which the server measured. fieldbus: the part of it on the fieldbus, the wait for the control-plane lock and every mailbox or ESC transaction. The debug log of the clients writes the same text.';

export function RequestTiming({
  timing,
  className,
}: {
  timing: RequestTimingValue | null;
  className?: string;
}) {
  if (!timing) {
    return null;
  }
  return (
    <span
      className={className ?? 'text-xs text-grey-500 font-mono whitespace-nowrap cursor-help'}
      title={TITLE}
    >
      {formatTimes(timing.roundTripMs, {
        serverMs: timing.serverMs,
        fieldbusMs: timing.fieldbusMs,
      })}
    </span>
  );
}

/** Reads the times of a request from its response. */
export function requestTiming(response: Response, startMs: number): RequestTimingValue {
  return {
    fieldbusMs: fieldbusTimeMs(response),
    serverMs: serverTimeMs(response),
    roundTripMs: performance.now() - startMs,
  };
}

// Captures the times of an API call on both success and failure — the server sends Server-Timing
// with the error response too, so a slow or failed operation still shows its cost. Pass a thunk so
// timing starts right before the request. Re-throws the error so react-query / callers still see it.
export function useRequestTiming() {
  const [timing, setTiming] = useState<RequestTimingValue | null>(null);
  const measure = useCallback(async <T extends Response>(call: () => Promise<T>): Promise<T> => {
    const start = performance.now();
    try {
      const res = await call();
      setTiming(requestTiming(res, start));
      return res;
    } catch (err) {
      if (err instanceof HttpError) {
        setTiming(requestTiming(err.response, start));
      }
      throw err;
    }
  }, []);
  const reset = useCallback(() => setTiming(null), []);
  return { timing, measure, reset };
}
