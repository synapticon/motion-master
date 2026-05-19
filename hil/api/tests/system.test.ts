import semver from 'semver';
import { expect, test } from 'vitest';
import { api } from '../src/setup.js';

test('version', async () => {
  const { data } = await api.version.getVersion();
  console.log(`motion master version: ${data.version}`);
  expect(semver.valid(data.version)).not.toBeNull();
});
