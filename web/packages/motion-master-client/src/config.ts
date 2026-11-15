// Reads the settings of a client from config.jsonc, for `MotionMasterClient.fromConfig`. Node.js
// only: it imports the Node.js modules and the JSONC parser when it runs, so a browser build that
// never calls it loads none of them. The index does not export this file.

import type { Api } from './generated/Api';
import { importNode, type NodeModules } from './node-modules';

interface NodeProcess {
  env: Record<string, string | undefined>;
}

/** The body of `POST /api/init`, for example `{ driver: 'soem', adapter: 'eth0' }`. */
export type FieldbusSettings = NonNullable<Parameters<Api['init']>[0]>;

/** The keys of config.jsonc: the keys of config.toml of the Python client, in camelCase. */
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

const CONFIG_NAME = 'config.jsonc';
const TEMPLATE_NAME = 'config.example.jsonc';
const TEMPLATE_URL =
  'https://github.com/synapticon/motion-master/blob/main/web/packages/motion-master-client/config.example.jsonc';

/**
 * Reads config.jsonc and returns the settings for the client. Without a path, it reads config.jsonc
 * in the current directory, and else config.jsonc in the folder of the package. The first is for a
 * client installed from npm. The second is for a clone of the repository. The file is JSON with
 * comments, and a trailing comma is allowed.
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
      throw notFound(`${file} not found`);
    }
  } else {
    const candidates = [...new Set([resolve(CONFIG_NAME), join(packageDir, CONFIG_NAME)])];
    const found = candidates.find((candidate) => existsSync(candidate));
    if (!found) {
      throw notFound(missingConfigMessage(candidates, join(packageDir, TEMPLATE_NAME), existsSync));
    }
    file = found;
  }

  const { parse, printParseErrorCode } =
    await importNode<typeof import('jsonc-parser')>('jsonc-parser');
  const text = readFileSync(file, 'utf8');
  const errors: import('jsonc-parser').ParseError[] = [];
  const config = parse(text, errors, { allowTrailingComma: true }) as ConfigFile;
  if (errors.length > 0) {
    const { error, offset } = errors[0];
    const line = text.slice(0, offset).split('\n').length;
    throw new Error(`${file}:${line}: ${printParseErrorCode(error)}`);
  }
  for (const key of ['host', 'httpPort', 'wsPort'] as const) {
    if (config[key] === undefined) {
      throw new Error(`${file}: "${key}" is missing`);
    }
  }
  applyVerify(config.verify ?? false);
  return {
    baseUrl: `https://${config.host}:${config.httpPort}`,
    wsUrl: `wss://${config.host}:${config.wsPort}`,
    fieldbus: config.fieldbus,
  };
}

/** Returns an error for a file that is not there, with the code Node.js gives such an error. */
function notFound(message: string): Error {
  return Object.assign(new Error(message), { code: 'ENOENT' });
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
    : `write config.jsonc in the current directory from ${TEMPLATE_URL}`;
  return `no config.jsonc in ${looked} -- ${make}`;
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
