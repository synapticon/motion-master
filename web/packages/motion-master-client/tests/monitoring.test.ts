import { afterEach, describe, expect, it, vi } from 'vitest';
import { Cia402, HttpError } from '../src/index';
import { type Answer, fakeServer, fakeSocket } from './fakes';

const CREATED = {
  topic: 't1',
  interval: 50,
  parameters: [{ devicePosition: 1, index: 0x6041, subindex: 0, source: 'pdo' }],
};

const routes = {
  'POST /api/monitorings': () => [201, CREATED] satisfies Answer,
  'DELETE /api/monitorings/t1': () => [204] satisfies Answer,
};

/**
 * A server that sends one batch of one row every 5 ms once a client subscribes to the topic, as the
 * monitoring of the server does.
 */
function streamingSocket() {
  let timer: ReturnType<typeof setInterval> | undefined;
  let t = 1_000_000;
  const socket = fakeSocket((message) => {
    if ((message as { subscribe?: string }).subscribe === 't1' && timer === undefined) {
      timer = setInterval(() => {
        t += 1000;
        socket.send({ type: 'monitoring', topic: 't1', data: [[t, 0x0237]] });
      }, 5);
    }
  });
  const stop = () => clearInterval(timer);
  return { socket, stop };
}

afterEach(() => {
  vi.unstubAllGlobals();
});

describe('monitoring', () => {
  it('records the rows of the span and deletes the monitoring afterwards', async () => {
    const server = fakeServer(routes);
    const { stop } = streamingSocket();
    const m = await server.client.monitoring(
      [[1, Cia402.STATUSWORD]],
      () => new Promise((resolve) => setTimeout(resolve, 30)),
      { topic: 't1' },
    );
    stop();
    expect(m.rows.length).toBeGreaterThan(2);
    expect(m.sources).toEqual(['pdo']);
    expect(server.requests[0].body).toEqual({
      topic: 't1',
      interval: 50,
      parameters: [[1, 0x6041, 0]],
    });
    expect(server.calls()).toEqual(['POST /api/monitorings', 'DELETE /api/monitorings/t1']);
  });

  it('closes the socket it opened, so that a Node.js program can end', async () => {
    const server = fakeServer(routes);
    const { stop } = streamingSocket();
    await server.client.monitoring([[1, Cia402.STATUSWORD]], () => {}, { topic: 't1' });
    stop();
    expect(server.client.ws.readyState).toBe('closed');
  });

  it('passes on the error of the span, and still deletes the monitoring', async () => {
    const server = fakeServer(routes);
    const { stop } = streamingSocket();
    await expect(
      server.client.monitoring(
        [[1, 0x6041, 0]],
        () => {
          throw new Error('the move failed');
        },
        { topic: 't1' },
      ),
    ).rejects.toThrow('the move failed');
    stop();
    expect(server.calls().at(-1)).toBe('DELETE /api/monitorings/t1');
  });

  it('gives the reason of the server when it refuses the monitoring', async () => {
    const server = fakeServer({
      'POST /api/monitorings': () => [400, { error: 'invalid topic' }],
    });
    const error = await server.client
      .monitoring([[1, 0x6041, 0]], () => {}, { topic: 'bad topic' })
      .then(
        () => new Error('the monitoring was created'),
        (e: unknown) => e as Error,
      );
    expect(error).toMatchObject({
      message: 'cannot create monitoring bad topic: invalid topic (HTTP 400)',
    });
    expect(error.cause).toBeInstanceOf(HttpError);
    expect(server.calls()).toEqual(['POST /api/monitorings']);
  });

  it('sends no request when there is no WebSocket, as in Node.js 20', async () => {
    vi.stubGlobal('WebSocket', undefined);
    const server = fakeServer(routes);
    await expect(server.client.monitoring([[1, 0x6041, 0]], () => {})).rejects.toThrow(
      'No WebSocket implementation available',
    );
    expect(server.requests).toHaveLength(0);
  });

  it('deletes the monitoring when no row arrives', { timeout: 10_000 }, async () => {
    const server = fakeServer(routes);
    fakeSocket();
    await expect(
      server.client.monitoring([[1, 0x6041, 0]], () => {}, { topic: 't1' }),
    ).rejects.toMatchObject({
      name: 'TimeoutError',
      message: 'monitoring t1: no row after 5000 ms',
    });
    expect(server.calls().at(-1)).toBe('DELETE /api/monitorings/t1');
  });

  it('fails when the WebSocket closes during the recording', async () => {
    const server = fakeServer(routes);
    const { socket, stop } = streamingSocket();
    await expect(
      server.client.monitoring(
        [[1, 0x6041, 0]],
        () =>
          new Promise((resolve) => {
            socket.close();
            setTimeout(resolve, 20);
          }),
        { topic: 't1' },
      ),
    ).rejects.toThrow('monitoring t1: the WebSocket closed during the recording');
    stop();
    expect(server.calls().at(-1)).toBe('DELETE /api/monitorings/t1');
  });
});
