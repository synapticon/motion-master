import { expect, test } from 'vitest';
import { client } from '../src/setup.js';

// The server under test in CI has no fieldbus, so no process image is ever published and no device
// can be resolved. These tests cover the route, request parsing/validation, and the per-item result
// contract — the full stage → sent-next-cycle path is exercised on hardware.

const baseUrl = process.env.MM_URL ?? 'https://local.motion-master.synapticon.com:61447';

// Raw fetch for the negative cases: they post bodies the typed client can't express.
async function request(method: string, path: string, body?: unknown): Promise<Response> {
  return fetch(`${baseUrl}${path}`, {
    method,
    headers: body === undefined ? undefined : { 'Content-Type': 'application/json' },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
}

test('GET /api/process-image reports an unconfigured image with no fieldbus', async () => {
  const { data } = await client.api.getProcessImage();
  expect(data.configured).toBe(false);
  expect(data.outputs).toEqual([]);
  expect(data.inputs).toEqual([]);
});

test('POST /api/process-data/outputs returns a per-item result for each entry', async () => {
  const { data } = await client.api.stageProcessDataOutputs([
    [1, 24698, 0, 5000],
    [2, 24640, 0, 15],
  ]);
  // One result per request, in order; nothing can stage without a bus, but each entry echoes its
  // identity and reports why it was not staged (the device cannot be resolved).
  expect(data.results).toHaveLength(2);
  expect(data.results[0]).toMatchObject({ slavePosition: 1, index: 24698, subindex: 0, staged: false });
  expect(data.results[0].error).toContain('not found');
  expect(data.results[1]).toMatchObject({ slavePosition: 2, index: 24640, subindex: 0, staged: false });
});

test('POST /api/process-data/outputs accepts an empty batch', async () => {
  const { data } = await client.api.stageProcessDataOutputs([]);
  expect(data.results).toEqual([]);
});

test('POST /api/process-data/outputs rejects malformed bodies with 400', async () => {
  const cases: unknown[] = [
    { not: 'an array' }, // object, not an array
    [[1, 24698, 0]], // entry too short (missing value)
    [[1, 24698, 0, 5000, 99]], // entry too long
    [[-1, 24698, 0, 5000]], // negative id
    [[1, 'x', 0, 5000]], // non-integer index
  ];
  for (const body of cases) {
    const res = await request('POST', '/api/process-data/outputs', body);
    expect(res.status, JSON.stringify(body)).toBe(400);
  }
});
