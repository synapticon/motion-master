// Reads the settings of a client from config.json, for `MotionMasterClient.fromConfig`. Node.js only:
// it imports the Node.js modules when it runs, so a browser build that never calls it loads none of
// them. The index does not export this file.

import type { Api } from './generated/Api';

// The parts of the Node.js modules that this file uses. The package has no Node.js types, because
// its other files run in a browser too.
interface NodeModules {
  fs: { existsSync(path: string): boolean; readFileSync(path: string, encoding: 'utf8'): string };
  path: {
    dirname(path: string): string;
    join(...paths: string[]): string;
    resolve(...paths: string[]): string;
  };
  url: { fileURLToPath(url: string): string };
}

interface NodeProcess {
  env: Record<string, string | undefined>;
}

/**
 * Imports a Node.js module by a name that a bundler cannot see, so that a browser build does not
 * try to resolve it.
 */
function importNode<T>(name: string): Promise<T> {
  return import(/* @vite-ignore */ name) as Promise<T>;
}

/** The body of `POST /api/init`, for example `{ driver: 'soem', adapter: 'eth0' }`. */
export type FieldbusSettings = NonNullable<Parameters<Api['init']>[0]>;

/** The keys of config.json: the keys of config.toml of the Python client, in camelCase. */
interface ConfigFile {
  host: string;
  httpPort: number;
  wsPort: number;
  verify?: boolean | string;
  fieldbus?: FieldbusSettings;
}

export interface ClientSettings {
  baseUrl: string;
  wsUrl: string;
  fieldbus?: FieldbusSettings;
}

const CONFIG_NAME = 'config.json';
const TEMPLATE_NAME = 'config.example.json';
const TEMPLATE_URL =
  'https://github.com/synapticon/motion-master/blob/main/web/packages/motion-master-client/config.example.json';

/**
 * Reads config.json and returns the settings for the client. Without a path, it reads config.json
 * in the current directory, and else config.json in the folder of the package. The first is for a
 * client installed from npm. The second is for a clone of the repository.
 */
export async function readConfig(path?: string): Promise<ClientSettings> {
  const { existsSync, readFileSync } = await importNode<NodeModules['fs']>('node:fs');
  const { dirname, join, resolve } = await importNode<NodeModules['path']>('node:path');
  const { fileURLToPath } = await importNode<NodeModules['url']>('node:url');

  // This file is in src/ of a clone and in dist/ of a package, one folder below the package.
  const packageDir = dirname(dirname(fileURLToPath(import.meta.url)));
  let file: string;
  if (path) {
    file = resolve(path);
    if (!existsSync(file)) {
      throw new Error(`${file} not found`);
    }
  } else {
    const candidates = [...new Set([resolve(CONFIG_NAME), join(packageDir, CONFIG_NAME)])];
    const found = candidates.find((candidate) => existsSync(candidate));
    if (!found) {
      throw new Error(
        missingConfigMessage(candidates, join(packageDir, TEMPLATE_NAME), existsSync),
      );
    }
    file = found;
  }

  const config = JSON.parse(readFileSync(file, 'utf8')) as ConfigFile;
  applyVerify(config.verify ?? false);
  return {
    baseUrl: `https://${config.host}:${config.httpPort}`,
    wsUrl: `wss://${config.host}:${config.wsPort}`,
    fieldbus: config.fieldbus,
  };
}

/** Says where `readConfig` looked, and how to make the file. */
function missingConfigMessage(
  candidates: string[],
  template: string,
  existsSync: (path: string) => boolean,
): string {
  const looked = candidates.join(' or ');
  // A clone has the template in the folder of the package. A package from npm does not.
  const make = existsSync(template)
    ? `copy the template: cp ${template} ${candidates[candidates.length - 1]}`
    : `write config.json in the current directory from ${TEMPLATE_URL}`;
  return `no config.json in ${looked} -- ${make}`;
}

/**
 * Applies the TLS verification of the config to the process. The built-in fetch and WebSocket of
 * Node.js take no TLS option, so the setting goes through the environment of the process, and it
 * applies to every connection that the process opens.
 */
function applyVerify(verify: boolean | string): void {
  if (typeof verify === 'string') {
    throw new Error(
      `verify is ${verify}. Node.js cannot load a certificate file after it starts: ` +
        `set verify to true and start node with NODE_EXTRA_CA_CERTS=${verify}`,
    );
  }
  if (!verify) {
    const { process } = globalThis as unknown as { process: NodeProcess };
    process.env.NODE_TLS_REJECT_UNAUTHORIZED = '0';
  }
}
