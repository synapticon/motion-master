// The ESLint rules for every TypeScript and JavaScript file. Prettier sets the layout, so
// eslint-config-prettier turns off every layout rule of ESLint, and the two tools never disagree.

import js from '@eslint/js';
import prettier from 'eslint-config-prettier';
import { defineConfig } from 'eslint/config';
import reactHooks from 'eslint-plugin-react-hooks';
import globals from 'globals';
import tseslint from 'typescript-eslint';

export default defineConfig(
  {
    ignores: [
      '**/dist/',
      '**/dev-dist/',
      '**/node_modules/',
      // The vcpkg submodule, the build output of CMake, and the Python virtual environments.
      'extern/',
      'build/',
      'out/',
      '**/.venv/',
      // Generated from apps/motion_master/swagger.yml.
      'web/packages/motion-master-client/src/generated/',
    ],
  },
  js.configs.recommended,
  tseslint.configs.recommended,
  {
    files: ['**/*.{ts,tsx}'],
    languageOptions: {
      // The type information is for no-floating-promises: a promise that nobody awaits loses its
      // error.
      parserOptions: {
        // The Vitest configs belong to no tsconfig, so the default project types them.
        projectService: {
          allowDefaultProject: ['vitest.config.ts', 'web/packages/*/vitest.config.ts'],
        },
        tsconfigRootDir: import.meta.dirname,
      },
    },
    rules: {
      '@typescript-eslint/no-floating-promises': 'error',
    },
  },
  {
    files: ['web/apps/**/*.{ts,tsx}', 'web/packages/ui/**/*.{ts,tsx}'],
    languageOptions: { globals: globals.browser },
    // The two classic rules of hooks. The other rules of the plugin serve the React Compiler, which
    // the web apps do not use.
    plugins: { 'react-hooks': reactHooks },
    rules: {
      'react-hooks/rules-of-hooks': 'error',
      'react-hooks/exhaustive-deps': 'error',
    },
  },
  {
    files: ['**/*.{js,mjs,cjs}', '**/vite.config.ts'],
    languageOptions: { globals: globals.node },
  },
  prettier,
  // After the Prettier config, because it turns off curly. With "all", curly cannot conflict with
  // Prettier, so its documentation allows it again.
  {
    rules: {
      curly: ['error', 'all'],
      // x == null tests for null and undefined together, as the Google TypeScript style guide
      // allows.
      eqeqeq: ['error', 'always', { null: 'ignore' }],
    },
  },
);
