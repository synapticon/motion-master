// The Node.js modules that the Node.js-only helpers use, such as reading config.jsonc and writing a
// CSV file. They are imported when a helper runs, so a browser build that never calls one loads none
// of them. The index does not export this file.

/** The parts of the Node.js modules that the client uses. The package has no Node.js types. */
export interface NodeModules {
  fs: {
    existsSync(path: string): boolean;
    readFileSync(path: string, encoding: 'utf8'): string;
    mkdirSync(path: string, options: { recursive: true }): unknown;
    writeFileSync(path: string, data: string): void;
  };
  path: {
    dirname(path: string): string;
    join(...paths: string[]): string;
    resolve(...paths: string[]): string;
  };
  url: { fileURLToPath(url: string): string };
}

/**
 * Imports a module by a name that a bundler cannot see, so that a browser build does not try to
 * resolve it.
 */
export function importNode<T>(name: string): Promise<T> {
  return import(/* @vite-ignore */ name) as Promise<T>;
}
