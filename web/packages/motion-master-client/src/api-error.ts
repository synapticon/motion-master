/**
 * The one place that knows how a failed request from the generated client is shaped.
 *
 * `swagger-typescript-api`'s client does not throw an `Error`. On a non-2xx it throws its own
 * `HttpResponse` object, having parsed the response body into that object's `error` property — and
 * Motion Master's own error body is `{ "error": "<message>" }`. So the message a user should see
 * sits at `err.error.error`, two levels down, and reading one level yields the string
 * `"[object Object]"`.
 *
 * That is not a shape any caller should have to know, which is the whole reason this lives here: the
 * wrapping belongs to the generated client, the generated client belongs to this package, so the
 * unwrapping belongs here too rather than being rediscovered in every page that catches a request.
 */

/** A thrown value that might carry a server message. Deliberately loose — this is a `catch` value. */
interface MaybeApiError {
  error?: unknown
  status?: unknown
  statusText?: unknown
}

/**
 * The human-readable reason a request failed.
 *
 * Tries, in order: the server's own message (`{ error }`, at either nesting level), the HTTP status,
 * a thrown `Error`'s message, and finally `fallback`. The server message comes first because it is
 * the only one that says anything specific — Motion Master's node layer writes these, and they name
 * the object, the state, or the grammar that was wrong.
 *
 * @param err       Whatever was caught.
 * @param fallback  Returned when nothing usable can be found.
 */
export function apiErrorMessage(err: unknown, fallback = 'Unknown error'): string {
  if (typeof err === 'object' && err !== null) {
    const candidate = err as MaybeApiError
    // The generated client's HttpResponse: the parsed body sits in `error`.
    if (typeof candidate.error === 'object' && candidate.error !== null) {
      const body = candidate.error as MaybeApiError
      if (typeof body.error === 'string' && body.error !== '') {
        return body.error
      }
    }
    // A body handed over directly, or an already-unwrapped one.
    if (typeof candidate.error === 'string' && candidate.error !== '') {
      return candidate.error
    }
    // No body to read — a network failure, or a response with no JSON. The status is what is left,
    // and it is still more use than a generic sentence.
    if (typeof candidate.status === 'number') {
      const statusText =
        typeof candidate.statusText === 'string' && candidate.statusText !== ''
          ? ` ${candidate.statusText}`
          : ''
      return `HTTP ${candidate.status}${statusText}`
    }
  }
  if (err instanceof Error && err.message !== '') {
    return err.message
  }
  return fallback
}
