import { useCallback, useState } from 'react'
import { wireTimeMs } from '@synapticon/motion-master-client'

// Shared readout for the server-measured device wire time next to the browser-observed HTTP
// round-trip, used by every page that performs a fieldbus operation. The gap between the two is
// cross-origin/TLS and transport overhead, not device time. Mirrors the SDO/FoE readouts on the
// Parameters and FoE pages, generalised over a `label` (the operation name shown to the user).

// Formats a millisecond duration: sub-10 ms keeps one decimal so a fast wire transaction does not
// round to "0 ms"; larger values round to whole ms / two-decimal seconds.
export function formatWireMs(ms: number): string {
  if (ms < 10) return `${ms.toFixed(1)} ms`
  if (ms < 1000) return `${Math.round(ms)} ms`
  return `${(ms / 1000).toFixed(2)} s`
}

export interface WireTimingValue {
  // Server-measured device time in ms, or null if the response carried no X-Wire-Us header.
  wireMs: number | null
  // Browser-observed HTTP round-trip in ms (measured around the fetch).
  roundTripMs: number
}

export function WireTiming({
  label,
  timing,
  className,
}: {
  label: string
  timing: WireTimingValue | null
  className?: string
}) {
  if (!timing) return null
  return (
    <span className={className ?? 'text-xs text-grey-500 font-mono whitespace-nowrap'}>
      {timing.wireMs !== null && (
        <span
          className="cursor-help"
          title={`${label} — server-measured duration of the on-device operation itself (control-plane lock acquire + the mailbox/ESC wire transaction(s)), reported by the backend. This is the true cost of talking to the device.`}
        >
          {label} {formatWireMs(timing.wireMs)}
        </span>
      )}
      <span
        className="text-grey-400 cursor-help"
        title="Round-trip — total time this browser observed for the HTTP request, measured around the fetch call. It includes the device time plus cross-origin/TLS and transport overhead, so it is normally much larger than the device figure and is not device time."
      >
        {timing.wireMs !== null ? ' · round-trip ' : 'round-trip '}
        {formatWireMs(timing.roundTripMs)}
      </span>
    </span>
  )
}

// Captures both the server-measured wire time (X-Wire-Us) and the browser round-trip around an API
// call, on both success and failure — the backend attaches X-Wire-Us to the error response too, so
// a slow or failed operation still shows its device cost. Pass a thunk so timing starts right
// before the request. Re-throws the error so react-query / callers still see it.
export function useWireTiming() {
  const [timing, setTiming] = useState<WireTimingValue | null>(null)
  const measure = useCallback(async <T extends Response>(call: () => Promise<T>): Promise<T> => {
    const start = performance.now()
    try {
      const res = await call()
      setTiming({ wireMs: wireTimeMs(res), roundTripMs: performance.now() - start })
      return res
    } catch (err) {
      if (err instanceof Response) {
        setTiming({ wireMs: wireTimeMs(err), roundTripMs: performance.now() - start })
      }
      throw err
    }
  }, [])
  const reset = useCallback(() => setTiming(null), [])
  return { timing, measure, reset }
}
