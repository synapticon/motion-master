import type { ReadyState } from '@synapticon/motion-master-client';
import { expect, test } from 'vitest';
import WebSocket from 'ws';
import { client } from '../src/setup.js';

// The server under test in CI has no fieldbus, so a monitoring can never be created (its
// parameters can't be sourced). These tests therefore cover the routes, validation, and the
// WebSocket subscribe plumbing — the socket-bound behaviour the C++ unit tests can't reach. The
// full create → sample → receive path is exercised on hardware.

const baseUrl = process.env.MM_URL ?? 'https://local.motion-master.synapticon.com:61447';
// The WebSocket runs on its own port (separate loop from the HTTP API).
const wsBaseUrl = process.env.MM_WS_URL ?? 'wss://local.motion-master.synapticon.com:62281';

// Raw fetch for the negative cases below: they post bodies the typed client deliberately can't
// express (missing/invalid fields), so they bypass the client to hit the server's validation.
async function request(method: string, path: string, body?: unknown): Promise<Response> {
  return fetch(`${baseUrl}${path}`, {
    method,
    headers: body === undefined ? undefined : { 'Content-Type': 'application/json' },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
}

function waitForState(target: ReadyState, timeoutMs = 5_000): Promise<void> {
  return new Promise((resolve, reject) => {
    if (client.ws.readyState === target) {
      resolve();
      return;
    }
    const timer = setTimeout(() => {
      off();
      reject(new Error(`WebSocket did not reach "${target}" within ${timeoutMs}ms`));
    }, timeoutMs);
    const off = client.ws.onStateChange((state) => {
      if (state === target) {
        clearTimeout(timer);
        off();
        resolve();
      }
    });
  });
}

const validParam = [1, 8240, 1];

test('GET /api/monitorings is initially empty', async () => {
  const { data } = await client.api.listMonitorings();
  expect(data).toEqual([]);
});

test('POST /api/monitorings rejects malformed configs with 400', async () => {
  const cases: Array<Record<string, unknown>> = [
    { interval: 100, parameters: [validParam] }, // missing topic
    { topic: 'bad/topic', interval: 100, parameters: [validParam] }, // not URL-safe
    { topic: 'pdos', interval: 100, parameters: [validParam] }, // reserved
    { topic: 'x', interval: 4, parameters: [validParam] }, // interval < 5 ms
    { topic: 'x', interval: 3000, parameters: [validParam] }, // interval > 2000 ms
    { topic: 'x', interval: 100, parameters: [] }, // no parameters
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
    interval: 100,
    parameters: [validParam],
  });
  expect(res.status).toBe(400);
});

test('GET/DELETE of an unknown monitoring is 404', async () => {
  expect((await request('GET', '/api/monitorings/nope')).status).toBe(404);
  expect((await request('DELETE', '/api/monitorings/nope')).status).toBe(404);
});

test('client.ws subscribe/unsubscribe keeps the connection open', async () => {
  // The client lazily connects on first subscribe and sends the subscribe frame; unsubscribing the
  // last listener sends unsubscribe. The connection must survive both.
  const unsubscribe = client.ws.subscribe('left-leg', () => {});
  await waitForState('open');
  unsubscribe();
  await new Promise((resolve) => setTimeout(resolve, 200));
  expect(client.ws.readyState).toBe('open');
  client.ws.close();
});

test('server ignores a malformed WebSocket frame without dropping the connection', async () => {
  // The client never sends non-JSON, so this drives a raw socket to prove the server tolerates it.
  const ws = new WebSocket(wsBaseUrl, { rejectUnauthorized: false });
  await new Promise<void>((resolve, reject) => {
    ws.on('open', () => resolve());
    ws.on('error', reject);
  });

  ws.send('not json'); // malformed — must be ignored, not crash the server

  await new Promise((resolve) => setTimeout(resolve, 200));
  expect(ws.readyState).toBe(WebSocket.OPEN);
  ws.close();
});
