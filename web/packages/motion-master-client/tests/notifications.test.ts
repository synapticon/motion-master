import { describe, expect, it, vi } from 'vitest';
import { NOTIFICATION_TOPIC } from '../src/index';
import { fakeServer, fakeSocket } from './fakes';

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
});
