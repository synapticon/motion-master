import { defineConfig } from 'vitest/config';

export default defineConfig({
  test: {
    name: 'motion-master-client',
    include: ['tests/**/*.test.ts'],
    environment: 'node',
    setupFiles: ['tests/setup.ts'],
  },
});
