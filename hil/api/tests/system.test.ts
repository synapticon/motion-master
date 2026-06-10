import semver from 'semver';
import { expect, test } from 'vitest';
import { api } from '../src/setup.js';

test('version', async () => {
  const { data } = await api.version.getVersion();
  console.log(`motion master version: ${data.version}`);
  expect(semver.valid(data.version)).not.toBeNull();
});

test('started configuration', async () => {
  const { data } = await api.config.getStartedConfig();
  // The effective boot config: merged config-file-over-defaults, so the default blocks are present.
  expect(data).toMatchObject({
    server: expect.any(Object),
    gameLoop: expect.any(Object),
    recorder: expect.any(Object),
  });
});

test('POST /api/process-data/dump is 409 with no process image', async () => {
  // The server under test in CI has no fieldbus, so no image has ever been mapped — the dump has
  // nothing to serialise and must reject. The full dump path is exercised on hardware.
  await expect(api.processData.dumpProcessData()).rejects.toMatchObject({ status: 409 });
});
