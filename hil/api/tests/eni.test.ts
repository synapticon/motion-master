import { expect, test } from 'vitest';
import { fetchEni } from '@synapticon/motion-master-client';

// The server under test in CI has no fieldbus, so no process image is ever published. An ENI
// describes a mapped bus, and there is nothing to describe before the SAFE-OP transition creates
// the FMMUs and the logical addresses — so what these tests can cover is the refusal, its status,
// and its message. A document that a third-party master accepts is exercised on hardware.

const baseUrl = process.env.MM_URL ?? 'https://local.motion-master.synapticon.com:61447';

test('GET /api/eni refuses a bus with no published process image', async () => {
  const res = await fetch(`${baseUrl}/api/eni`);
  expect(res.status).toBe(409);
  const body = (await res.json()) as { error: string };
  expect(body.error).toContain('no published process image');
  // The message says what to do about it, because the fix is an operator action.
  expect(body.error).toContain('SAFE-OP');
});

test('fetchEni surfaces the refusal as the server worded it', async () => {
  await expect(fetchEni(baseUrl)).rejects.toThrow(/no published process image/);
});
