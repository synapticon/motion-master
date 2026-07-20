// Helper for the server-measured wire-time channel.
//
// Endpoints that perform a fieldbus transaction (SDO, FoE, ...) report the on-device wire cost —
// control-plane lock acquire + the mailbox/ESC round-trip — via the `X-Wire-Us` response header
// (microseconds), independent of the response body shape. A cross-origin client subtracts this
// from its own observed HTTP round-trip to separate the device cost from browser/transport
// overhead. The header is CORS-exposed by the server so it is readable cross-origin.

// Name of the wire-time response header (microseconds, integer).
export const WIRE_TIME_HEADER = 'X-Wire-Us'

// Reads the server-measured wire time from a response, in milliseconds, or null if the header is
// absent or unparseable. Accepts any `Response` (the generated client's `HttpResponse` extends it).
export function wireTimeMs(res: Response): number | null {
  const raw = res.headers.get(WIRE_TIME_HEADER)
  if (raw === null) return null
  const us = Number(raw)
  return Number.isFinite(us) ? us / 1000 : null
}
