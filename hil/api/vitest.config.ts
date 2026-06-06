import { defineConfig } from 'vitest/config';

export default defineConfig({
  test: {
    globals: true,
    testTimeout: 300_000,
    hookTimeout: 300_000,
    teardownTimeout: 60_000,
    globalSetup: './src/global-setup.ts',
    include: ['tests/**/*.test.ts'],
    reporters: process.env.GITHUB_ACTIONS ? ['basic', 'github-actions'] : ['verbose'],
    pool: 'forks',
    poolOptions: {
      forks: { singleFork: true },
    },
    env: {
      MM_URL: process.env.MM_URL ?? 'https://local.motion-master.synapticon.com:61447',
    },
  },
});
