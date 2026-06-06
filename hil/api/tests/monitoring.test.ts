import { expect, test } from 'vitest';
import WebSocket from 'ws';

// The server under test in CI has no fieldbus, so a monitoring can never be created (its
// parameters can't be sourced). These tests therefore cover the routes, validation, and the
// WebSocket subscribe plumbing — the socket-bound behaviour the C++ unit tests can't reach. The
// full create → sample → receive path is exercised on hardware.

const baseUrl = process.env.MM_URL ?? 'https://local.motion-master.synapticon.com:8443';
// The realtime WebSocket runs on its own port (separate loop from the HTTP API).
const wsBaseUrl = process.env.MM_WS_URL ?? 'wss://local.motion-master.synapticon.com:8444';

async function request(method: string, path: string, body?: unknown): Promise<Response> {
  return fetch(`${baseUrl}${path}`, {
    method,
    headers: body === undefined ? undefined : { 'Content-Type': 'application/json' },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
}

const validParam = [1, 8240, 1];

test('GET /api/monitorings is initially empty', async () => {
  const res = await request('GET', '/api/monitorings');
  expect(res.status).toBe(200);
  expect(await res.json()).toEqual([]);
});

test('POST /api/monitorings rejects malformed configs with 400', async () => {
  const cases: Array<Record<string, unknown>> = [
    { interval: 1000, bufferSize: 16, parameters: [validParam] }, // missing topic
    { topic: 'bad/topic', interval: 1000, bufferSize: 16, parameters: [validParam] }, // not URL-safe
    { topic: 'pdos', interval: 1000, bufferSize: 16, parameters: [validParam] }, // reserved
    { topic: 'x', interval: 0, bufferSize: 16, parameters: [validParam] }, // interval < 1
    { topic: 'x', interval: 1000, bufferSize: 8, parameters: [validParam] }, // bufferSize < 16
    { topic: 'x', interval: 1000, bufferSize: 16, parameters: [] }, // no parameters
  ];
  for (const body of cases) {
    const res = await request('POST', '/api/monitorings', body);
    expect(res.status, JSON.stringify(body)).toBe(400);
  }
});

test('POST /api/monitorings rejects an unsourceable parameter with 400', async () => {
  // No bus → device/object cannot be classified as PDO or SDO.
  const res = await request('POST', '/api/monitorings', {
    topic: 'left-leg',
    interval: 1000,
    bufferSize: 16,
    parameters: [validParam],
  });
  expect(res.status).toBe(400);
});

test('GET/DELETE of an unknown monitoring is 404', async () => {
  expect((await request('GET', '/api/monitorings/nope')).status).toBe(404);
  expect((await request('DELETE', '/api/monitorings/nope')).status).toBe(404);
});

test('WebSocket accepts subscribe/unsubscribe without dropping the connection', async () => {
  const wsUrl = wsBaseUrl;
  const ws = new WebSocket(wsUrl, { rejectUnauthorized: false });
  await new Promise<void>((resolve, reject) => {
    ws.on('open', () => resolve());
    ws.on('error', reject);
  });

  ws.send(JSON.stringify({ subscribe: 'left-leg' }));
  ws.send('not json'); // malformed — must be ignored, not crash the server
  ws.send(JSON.stringify({ unsubscribe: 'left-leg' }));

  await new Promise((resolve) => setTimeout(resolve, 200));
  expect(ws.readyState).toBe(WebSocket.OPEN);
  ws.close();
});
