// The errors that the client throws. Every request of the client, `client.api` included, throws a
// ConnectionError when it does not reach the server and an HttpError when the server refuses it.

/** The client cannot reach the server. `cause` is the error of `fetch`. */
export class ConnectionError extends Error {
  override name = 'ConnectionError';

  /** The request that did not reach the server: its URL, method and headers, without the body. */
  readonly request: Request;

  constructor(request: Request, options?: ErrorOptions) {
    super(`cannot connect to ${new URL(request.url).origin}`, options);
    this.request = request;
  }
}

/** The server refuses a request: its answer has a status outside 200 to 299. */
export class HttpError extends Error {
  override name = 'HttpError';

  /** The refused request: its URL, method and headers, without the body. */
  readonly request: Request;

  /** The answer of the server, with its body unread. The Console reads its headers. */
  readonly response: Response;

  /** The HTTP status of the answer, such as 409. */
  readonly status: number;

  /**
   * The server's reason: the `error` of its JSON body, or the body when it is text, or the status
   * text.
   */
  readonly reason: string;

  constructor(request: Request, response: Response, reason: string) {
    const path = new URL(request.url).pathname;
    super(`${request.method} ${path}: HTTP ${response.status}, ${reason}`);
    this.request = request;
    this.response = response;
    this.status = response.status;
    this.reason = reason;
  }

  /** Returns the HttpError of a refused answer. It reads the reason from a copy of the body. */
  static async from(request: Request, response: Response): Promise<HttpError> {
    const text = (await response.clone().text()).trim();
    let reason = text || response.statusText;
    try {
      const body = JSON.parse(text) as { error?: unknown };
      if (typeof body.error === 'string') {
        reason = body.error;
      }
    } catch {
      // Not JSON: the text itself is the reason.
    }
    return new HttpError(request, response, reason);
  }
}

/** A device does not answer, or does not reach the AL state it was commanded to. */
export class StateError extends Error {
  override name = 'StateError';
}

/** The server refuses to start a procedure. The message is the server's reason. */
export class ProcedureError extends Error {
  override name = 'ProcedureError';
}

/** The server refuses to start or to stop a capture. The message is the server's reason. */
export class CaptureError extends Error {
  override name = 'CaptureError';
}

/** The server could not produce a process-data dump. The message is the server's reason. */
export class ProcessDataDumpError extends Error {
  override name = 'ProcessDataDumpError';
}
