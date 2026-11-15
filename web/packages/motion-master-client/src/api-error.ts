import { HttpError } from './errors';

/**
 * Returns the reason a request failed, for a page to show: the server's reason of an `HttpError`,
 * the message of any other `Error`, or `fallback`.
 */
export function apiErrorMessage(err: unknown, fallback = 'Unknown error'): string {
  if (err instanceof HttpError) {
    return err.reason;
  }
  if (err instanceof Error && err.message !== '') {
    return err.message;
  }
  return fallback;
}
