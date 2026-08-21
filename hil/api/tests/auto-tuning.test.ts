import type { AutoTuningStatus } from '@synapticon/motion-master-client';
import { beforeAll, expect, test } from 'vitest';
import { client } from '../src/setup.js';

// Auto-tuning runs in a child process that Motion Master starts, and the executable is downloaded
// rather than built. The Docker image the suite normally runs against bakes it in, so these tests
// have a process to talk to. A run against a machine that never had the executable installed skips
// the calls rather than failing them: not having it is a supported state.
let status: AutoTuningStatus;

beforeAll(async () => {
  const { data } = await client.api.getAutoTuning();
  status = data;
  console.log(`auto-tuning: started=${status.started} version=${status.version || 'none'} port=${status.port}`);
});

// A plant model the auto-tuning repository publishes in its own documentation, so the numbers are
// theirs rather than invented here.
const PLANT_MODEL = { numerator: [1.0], denominator: [0.00015, 0.000206] };

test('status names the executable and its state', () => {
  expect(status).toMatchObject({
    enabled: expect.any(Boolean),
    binaryPath: expect.any(String),
    installed: expect.any(Boolean),
    started: expect.any(Boolean),
    version: expect.any(String),
    port: expect.any(Number),
    error: expect.any(String),
  });
  // The path is always reported, because the reason a machine has no auto-tuning is usually that
  // nothing was ever put there — and then the path is the whole answer.
  expect(status.binaryPath).not.toBe('');
  // Either it runs and says which version, or it does not and says why.
  if (status.started) {
    expect(status.error).toBe('');
  } else {
    expect(status.error).not.toBe('');
  }
});

test('a tuning call returns gains', async (ctx) => {
  if (!status.started) {
    ctx.skip();
  }
  const { data } = await client.api.runAutoTuning({
    run: 'auto_tune_velocity_controller',
    data: { plant_model: PLANT_MODEL, zeta: 1.1, demanded_bw: 5.0 },
  });
  expect(data).toMatchObject({
    gains: { velocity: { kp: expect.any(Number), ki: expect.any(Number) } },
    duration: expect.any(Number),
  });
});

test('a rejected input is 200 with an error property', async (ctx) => {
  if (!status.started) {
    ctx.skip();
  }
  // The convention every client depends on: the auto-tuning program reports a refusal in the body,
  // not in the status, and Motion Master passes that through rather than translating it.
  const { data } = await client.api.runAutoTuning({
    run: 'auto_tune_notch',
    // biome-ignore lint/suspicious/noExplicitAny: the point is a body the schema forbids.
    data: {} as any,
  });
  expect(data).toMatchObject({ error: { message: expect.any(String) } });
});

test('an unknown function is the process own 404', async (ctx) => {
  if (!status.started) {
    ctx.skip();
  }
  await expect(
    // biome-ignore lint/suspicious/noExplicitAny: a run name no version of the program has.
    client.api.runAutoTuning({ run: 'no_such_function', data: {} } as any),
  ).rejects.toMatchObject({ status: 404 });
});

test('exit is refused', async (ctx) => {
  if (!status.started) {
    ctx.skip();
  }
  // Motion Master's own rule, and the one worth a regression test: shutting the process down would
  // break auto-tuning for every other client, and it is started only at startup.
  await expect(
    // biome-ignore lint/suspicious/noExplicitAny: the schema omits exit, which is the point.
    client.api.runAutoTuning({ run: 'exit' } as any),
  ).rejects.toMatchObject({ status: 400 });

  // And it is still there afterwards, which is what the refusal is for.
  const { data } = await client.api.runAutoTuning({
    run: 'auto_tune_notch',
    data: { plant_model: PLANT_MODEL },
  });
  expect(data).toMatchObject({ duration: expect.any(Number) });
});

test('measurement data travels in the request body', async (ctx) => {
  if (!status.started) {
    ctx.skip();
  }
  // Three columns of nothing: too little to fit a model, which is the point. A refusal proves the
  // CSV reached the program through the body and was parsed as measurements — no file, no shared
  // filesystem. Fitting a real recording is covered by the auto-tuning repository's own smoke test.
  const csv = Array.from({ length: 8 }, (_, i) => `${i * 0.001},0.0,0.0`).join('\n');
  const { data } = await client.api.runAutoTuning({
    run: 'identify_plant_model',
    data: { csv, f0: 1, f1: 200 },
  });
  // Either it fitted something from the flat signal or it refused; both mean the CSV arrived. What
  // must not happen is a complaint about a missing input.
  const body = data as { error?: { message?: string }; denominators?: number[] };
  if (body.error) {
    expect(body.error.message).not.toMatch(/csv|filepath/i);
  } else {
    expect(body.denominators).toBeInstanceOf(Array);
  }
});

test('the process own API description is served', async (ctx) => {
  if (!status.started) {
    ctx.skip();
  }
  // Fetched from the process on every request, so this is also a liveness check that does not run a
  // function. The generated client types the body as a string; the document is YAML, not JSON.
  const response = await fetch(`${client.api.baseUrl}/api/auto-tuning/swagger.yml`);
  expect(response.status).toBe(200);
  expect(response.headers.get('content-type')).toContain('yaml');
  const text = await response.text();
  expect(text).toMatch(/^openapi:/);
  expect(text).toContain('/api/run');
});
