// Turns a listener of the WebSocket connection into an async generator, so a program reads a stream
// with `for await`, as a Python script reads it with `for`. The index does not export this file.

import { ConnectionError } from './errors';

/**
 * Registers a listener on the first `next()` and yields each value it receives, in order. Leaving
 * the loop, with `break`, `return` or an error, removes the listener.
 *
 * The values queue while the loop body runs, so the stream loses none of them. An abort of `signal`
 * ends the stream with its reason, also while it waits for a value. `onClose` tells the stream when
 * the WebSocket closes: a close by `close()` ends it, and any other close throws a
 * `ConnectionError`, because the values sent while the socket was closed are lost.
 */
export async function* listen<T>(
  register: (listener: (value: T) => void) => () => void,
  options: {
    signal?: AbortSignal;
    onClose?: (listener: (byUser: boolean) => void) => () => void;
  } = {},
): AsyncGenerator<T, void, undefined> {
  const { signal, onClose } = options;
  signal?.throwIfAborted();
  const queue: T[] = [];
  // How the stream ends: 'done' when the program closed the client, or the error to throw. An object,
  // because only the callbacks set it.
  const ending: { end: 'done' | { error: unknown } | null } = { end: null };
  let wake: (() => void) | null = null;
  const stop = (reason: 'done' | { error: unknown }) => {
    ending.end ??= reason;
    wake?.();
    wake = null;
  };
  const abort = () => stop({ error: signal?.reason });
  signal?.addEventListener('abort', abort, { once: true });
  // Registered before the listener, because the listener opens the socket, which can fail at once.
  const removeOnClose = onClose?.((byUser) =>
    stop(byUser ? 'done' : { error: new ConnectionError('the WebSocket closed') }),
  );
  let unregister: (() => void) | null = null;
  try {
    unregister = register((value) => {
      queue.push(value);
      wake?.();
      wake = null;
    });
    for (;;) {
      while (queue.length > 0 && ending.end === null) {
        yield queue.shift() as T;
      }
      const { end } = ending;
      if (end === 'done') {
        return;
      }
      if (end !== null) {
        throw end.error;
      }
      await new Promise<void>((resolve) => {
        wake = resolve;
      });
    }
  } finally {
    signal?.removeEventListener('abort', abort);
    // Removed before the listener, because removing the last listener closes the socket.
    removeOnClose?.();
    unregister?.();
  }
}
