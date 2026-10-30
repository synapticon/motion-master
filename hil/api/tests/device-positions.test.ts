import { expect, test } from 'vitest';

// The server under test in CI has no fieldbus, so every position names no device. The routes that
// take a list of positions answer that with 404, as the routes for one device do, and not with 500,
// which a client takes for a failure of the bus.

const baseUrl = process.env.MM_URL ?? 'https://local.motion-master.synapticon.com:61447';

async function request(method: string, path: string, body?: unknown): Promise<Response> {
  return fetch(`${baseUrl}${path}`, {
    method,
    headers: body === undefined ? undefined : { 'Content-Type': 'application/json' },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
}

test.each([
  ['GET', '/api/devices/state?positions=9', undefined],
  ['GET', '/api/devices/diagnostics?positions=9', undefined],
  ['GET', '/api/dc-sync?positions=9', undefined],
  ['POST', '/api/devices/state', { state: 2, positions: [9] }],
  ['POST', '/api/devices/restart', { target: 2, positions: [9] }],
])('%s %s with an unknown position is 404', async (method, path, body) => {
  const response = await request(method, path, body);
  expect(response.status).toBe(404);
  expect((await response.json()).error).toBe('no device at bus position 9');
});
