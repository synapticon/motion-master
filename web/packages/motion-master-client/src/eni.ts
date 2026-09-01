// EtherCAT Network Information (ENI) export.
//
// The generated client types `GET /api/eni` as a `File`, which is what swagger-typescript-api makes
// of a binary response. An ENI is text, and what a caller wants is the document plus the warning
// count that came with it, so this wraps the call.

/// The result of an ENI export: the document, and how many parts the server could not read.
export interface EniExport {
  /// The ENI document, conforming to ENI Schema 1.7.
  xml: string
  /// Parts that were asked for and not answered, from `X-Eni-Warnings`. Each one costs an optional
  /// element, never the document, so a non-zero count still describes a bus a master can bring up.
  /// The server logs what each one was.
  warnings: number
}

/// Exports the bus as an ENI document (`GET /api/eni`). `baseUrl` is the HTTP API origin (e.g.
/// `https://host:61447`). Throws on a non-OK response, with the response body's `error` message
/// when there is one — a bus that has not reached SAFE-OP or OP answers 409, because before that
/// there is no mapping to describe. Pass `fetch` to inject an implementation (Node, request
/// logging).
export async function fetchEni(
  baseUrl: string,
  options: { fetch?: typeof fetch; signal?: AbortSignal } = {},
): Promise<EniExport> {
  const doFetch = options.fetch ?? fetch
  const res = await doFetch(`${baseUrl.replace(/\/+$/, '')}/api/eni`, {
    headers: { Accept: 'application/xml' },
    signal: options.signal,
  })
  if (!res.ok) {
    let message = `HTTP ${res.status}`
    try {
      const body = (await res.json()) as { error?: string }
      if (body?.error) {
        message = body.error
      }
    } catch {
      // Non-JSON error body — keep the status message.
    }
    throw new Error(message)
  }
  const warnings = Number.parseInt(res.headers.get('X-Eni-Warnings') ?? '0', 10)
  return {
    xml: await res.text(),
    warnings: Number.isFinite(warnings) ? warnings : 0,
  }
}
