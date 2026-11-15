// The errors that the helpers of the client throw. A failed HTTP request is not one of them: the
// generated client throws its `HttpResponse`, and `apiErrorMessage` reads the reason from it.

/** A device does not answer, or does not reach the AL state it was commanded to. */
export class StateError extends Error {
  override name = 'StateError';
}
