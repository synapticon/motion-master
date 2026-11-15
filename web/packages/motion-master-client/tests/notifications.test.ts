import { afterEach, describe, expect, it, vi } from 'vitest';
import { ConnectionError, NOTIFICATION_TOPIC } from '../src/index';
import { fakeServer, fakeSocket } from './fakes';

afterEach(() => {
  vi.unstubAllGlobals();
});

/** Reads a stream until it ends, and returns what it threw, or `null`. */
async function drain(stream: AsyncIterable<unknown>): Promise<unknown> {
  try {
    for await (const value of stream) {
      void value;
    }
    return null;
  } catch (error) {
    return error;
  }
}

describe('notifications', () => {
  it('yields the data of each notification, and unsubscribes when the loop ends', async () => {
    const { client } = fakeServer({});
    const socket = fakeSocket((message) => {
      if ((message as { subscribe?: string }).subscribe === NOTIFICATION_TOPIC) {
        socket.send({ type: 'monitoring', topic: 't1', data: [[1, 2]] });
        socket.send({ type: 'notification', data: 'not an object' });
        socket.send({
          type: 'notification',
          data: { event: 'short-working-counter', shortWkcCycles: 3 },
        });
        socket.send({ type: 'notification', data: { event: 'cycle-rate', hz: 990 } });
      }
    });
    const events = [];
    for await (const data of client.notifications()) {
      events.push(data);
      if (events.length === 2) {
        break;
      }
    }
    expect(events).toEqual([
      { event: 'short-working-counter', shortWkcCycles: 3 },
      { event: 'cycle-rate', hz: 990 },
    ]);
    await vi.waitFor(() => expect(socket.sent).toHaveLength(2));
    expect(socket.sent).toEqual([{ subscribe: 'notifications' }, { unsubscribe: 'notifications' }]);
    expect(client.ws.readyState).toBe('closed');
  });

  it('ends with the reason of the signal while it waits for a notification', async () => {
    const { client } = fakeServer({});
    const socket = fakeSocket();
    const controller = new AbortController();
    const stream = client.notifications({ signal: controller.signal });
    setTimeout(() => controller.abort(new Error('cancelled')), 20);
    expect(await drain(stream)).toMatchObject({ message: 'cancelled' });
    expect(socket.sent).toEqual([{ subscribe: 'notifications' }, { unsubscribe: 'notifications' }]);
    expect(client.ws.readyState).toBe('closed');
  });

  it('ends without an error when the program closes the client', async () => {
    const { client } = fakeServer({});
    fakeSocket((message) => {
      if ((message as { subscribe?: string }).subscribe === NOTIFICATION_TOPIC) {
        client.close();
      }
    });
    expect(await drain(client.notifications())).toBeNull();
  });

  it('throws a ConnectionError when the server closes the WebSocket', async () => {
    const { client } = fakeServer({});
    const socket = fakeSocket((message) => {
      if ((message as { subscribe?: string }).subscribe === NOTIFICATION_TOPIC) {
        socket.close();
      }
    });
    const error = await drain(client.notifications());
    expect(error).toBeInstanceOf(ConnectionError);
    expect(error).toMatchObject({ message: 'the WebSocket closed' });
    expect(client.ws.readyState).toBe('closed');
  });

  it('leaves no listener behind when there is no WebSocket class', async () => {
    const { client } = fakeServer({});
    vi.stubGlobal('WebSocket', undefined);
    expect(await drain(client.notifications())).toMatchObject({
      message: expect.stringContaining('no WebSocket class'),
    });
    vi.unstubAllGlobals();
    // A listener left behind would keep the socket open after the next stream ends, so that a
    // Node.js program would not end.
    const socket = fakeSocket((message) => {
      if ((message as { subscribe?: string }).subscribe === NOTIFICATION_TOPIC) {
        socket.send({ type: 'notification', data: { event: 'cycle-rate', hz: 990 } });
      }
    });
    for await (const data of client.notifications()) {
      expect(data).toEqual({ event: 'cycle-rate', hz: 990 });
      break;
    }
    expect(client.ws.readyState).toBe('closed');
  });
});
