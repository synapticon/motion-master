// Where the examples save their files, and how they name them. Not an example of its own.

import { fileURLToPath } from 'node:url';

/** The git-ignored folder next to the client that the examples save into. */
export const FILES_DIR = fileURLToPath(new URL('../files', import.meta.url));

/** Returns the local time as `YYYYMMDD-HHMMSS`, as the Python examples name their files. */
export function fileStamp(date = new Date()): string {
  const two = (n: number) => String(n).padStart(2, '0');
  return (
    `${date.getFullYear()}${two(date.getMonth() + 1)}${two(date.getDate())}-` +
    `${two(date.getHours())}${two(date.getMinutes())}${two(date.getSeconds())}`
  );
}
