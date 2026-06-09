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
