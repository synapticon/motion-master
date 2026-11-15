// Turns a listener of the WebSocket connection into an async generator, so a program reads a stream
// with `for await`, as a Python script reads it with `for`. The index does not export this file.

/**
 * Registers a listener on the first `next()` and yields each value it receives, in order. Leaving
 * the loop, with `break`, `return` or an error, removes the listener.
 *
 * The values queue while the loop body runs, so the stream loses none of them.
 */
export async function* listen<T>(
  register: (listener: (value: T) => void) => () => void,
): AsyncGenerator<T, void, undefined> {
  const queue: T[] = [];
  let wake: (() => void) | null = null;
  const unregister = register((value) => {
    queue.push(value);
    wake?.();
    wake = null;
  });
  try {
    for (;;) {
      while (queue.length > 0) {
        yield queue.shift() as T;
      }
      await new Promise<void>((resolve) => {
        wake = resolve;
      });
    }
  } finally {
    unregister();
  }
}
