// One test runner for the whole workspace. Each project that has tests is listed here and has its own
// vitest.config.ts. Run every project with `pnpm test` at the root.

import { defineConfig } from 'vitest/config';

export default defineConfig({
  test: {
    projects: ['web/packages/motion-master-client'],
  },
});
