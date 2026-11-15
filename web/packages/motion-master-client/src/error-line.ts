// One line for an error a user can act on, and run(), which prints that line and ends a Node.js
// program. A bug is not such an error, so it keeps its stack trace.

import {
  CaptureError,
  ConnectionError,
  HttpError,
  MonitoringError,
  ProcedureError,
  ProcessDataDumpError,
  StateError,
} from './errors';

/**
 * Returns one line that names an error a user can act on, or `null` for any other error.
 *
 * Those errors are: a server that the client cannot reach, a refused request, a WebSocket that
 * closes, a `StateError`, a `ProcedureError`, a `CaptureError`, a `MonitoringError`, a
 * `ProcessDataDumpError`, a `TimeoutError` of `waitForValue()`, and a file that is not there.
 */
export function errorLine(error: unknown): string | null {
  if (error instanceof ConnectionError && error.request !== null) {
    return `${error.message}. Is the server running, and is config.jsonc correct?`;
  }
  if (
    error instanceof ConnectionError ||
    error instanceof HttpError ||
    error instanceof MonitoringError ||
    error instanceof StateError ||
    error instanceof ProcedureError ||
    error instanceof CaptureError ||
    error instanceof ProcessDataDumpError
  ) {
    return error.message;
  }
  // waitForValue() throws a TimeoutError when the value does not come in time.
  if (error instanceof DOMException && error.name === 'TimeoutError') {
    return error.message;
  }
  if (error instanceof Error && (error as { code?: unknown }).code === 'ENOENT') {
    return error.message;
  }
  return null;
}

interface NodeProcess {
  env: Record<string, string | undefined>;
  exitCode?: number;
}

/**
 * Runs `main` and ends the program with one line on stderr when it fails with an error a user can
 * act on. Node.js only.
 *
 * Any other error is a bug, so it is thrown again and keeps its stack trace. Set `MM_TRACEBACK=1`
 * to get the stack trace for every error.
 */
export async function run(main: () => unknown): Promise<void> {
  try {
    await main();
  } catch (error) {
    const { process } = globalThis as unknown as { process: NodeProcess };
    const line = errorLine(error);
    if (line === null || process.env.MM_TRACEBACK === '1') {
      throw error;
    }
    console.error(`error: ${line}`);
    process.exitCode = 1;
  }
}
