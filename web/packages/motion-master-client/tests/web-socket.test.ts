import { describe, expect, it, vi } from 'vitest';
import { WebSocketConnection } from '../src/index';
import { fakeServer, fakeSocket, WS_URL } from './fakes';

/** Waits until `condition` holds, for up to one second. */
async function until(condition: () => boolean): Promise<void> {
  await vi.waitFor(() => {
    if (!condition()) {
      throw new Error('not yet');
    }
  });
}

describe('WebSocketConnection', () => {
  it('keeps a socket that connect() opened when the last listener leaves, as the Console needs', async () => {
    fakeSocket();
    const connection = new WebSocketConnection({ url: WS_URL });
    connection.connect();
    await until(() => connection.readyState === 'open');
    const unsubscribe = connection.subscribe('t1', () => {});
    unsubscribe();
    expect(connection.readyState).toBe('open');
    connection.close();
  });

  it('closes a socket that only a listener opened when the last listener leaves', async () => {
    fakeSocket();
    const connection = new WebSocketConnection({ url: WS_URL });
    const unsubscribe = connection.subscribe('t1', () => {});
    await until(() => connection.readyState === 'open');
    unsubscribe();
    expect(connection.readyState).toBe('closed');
  });

  it('subscribes once per topic, and unsubscribes with the last listener', async () => {
    const socket = fakeSocket();
    const connection = new WebSocketConnection({ url: WS_URL });
    const first = connection.subscribe('t1', () => {});
    const second = connection.subscribe('t1', () => {});
    await until(() => socket.sent.length === 1);
    first();
    second();
    await until(() => socket.sent.length === 2);
    expect(socket.sent).toEqual([{ subscribe: 't1' }, { unsubscribe: 't1' }]);
  });
});

describe('streamMonitoring', () => {
  it('yields the batches of its own topic only', async () => {
    const { client } = fakeServer({});
    const socket = fakeSocket((message) => {
      if ((message as { subscribe?: string }).subscribe === 't1') {
        socket.send({ type: 'monitoring', topic: 'other', data: [[1, 9]] });
        socket.send({ type: 'monitoring', topic: 't1', data: [[2, 20]] });
        socket.send({ type: 'monitoring', topic: 't1', data: [[3, 30]] });
      }
    });
    const batches = [];
    for await (const rows of client.streamMonitoring('t1')) {
      batches.push(rows);
      if (batches.length === 2) {
        break;
      }
    }
    expect(batches).toEqual([[[2, 20]], [[3, 30]]]);
    await until(() => socket.sent.length === 2);
    expect(socket.sent.at(-1)).toEqual({ unsubscribe: 't1' });
    expect(client.ws.readyState).toBe('closed');
  });
});
