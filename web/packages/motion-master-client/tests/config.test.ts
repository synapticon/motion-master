import { mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';
import { MotionMasterClient } from '../src/index';

/** Writes `text` to a config.jsonc in a new folder and returns its path. */
function configFile(text: string): string {
  const path = join(mkdtempSync(join(tmpdir(), 'mm-')), 'config.jsonc');
  writeFileSync(path, text);
  return path;
}

describe('fromConfig', () => {
  it('reads JSON with comments and a trailing comma', async () => {
    const path = configFile(`{
      // The host and the two ports.
      "host": "local.example", "httpPort": 1, "wsPort": 2,
      "verify": true,
      "fieldbus": { "driver": "soem", "adapter": "eth0", },
    }`);
    const client = await MotionMasterClient.fromConfig({ path });
    expect(client.fieldbus).toEqual({ driver: 'soem', adapter: 'eth0' });
  });

  it('reads the template of the package', async () => {
    const before = process.env.NODE_TLS_REJECT_UNAUTHORIZED;
    const client = await MotionMasterClient.fromConfig({
      path: join(import.meta.dirname, '..', 'config.example.jsonc'),
    });
    expect(client.fieldbus?.driver).toBe('soem');
    // The template turns off TLS verification for the process. Give the next test its old setting.
    if (before === undefined) {
      delete process.env.NODE_TLS_REJECT_UNAUTHORIZED;
    } else {
      process.env.NODE_TLS_REJECT_UNAUTHORIZED = before;
    }
  });

  it('names the file and the line of a syntax error', async () => {
    const path = configFile('{\n  "host": "x"\n  "httpPort": 1\n}\n');
    await expect(MotionMasterClient.fromConfig({ path })).rejects.toThrow(
      `${path}:3: CommaExpected`,
    );
  });

  it('names a key that is missing', async () => {
    const path = configFile('{ "host": "x", "wsPort": 2 }');
    await expect(MotionMasterClient.fromConfig({ path })).rejects.toThrow(
      `${path}: "httpPort" is missing`,
    );
  });

  it('says that a named file is missing', async () => {
    await expect(MotionMasterClient.fromConfig({ path: '/no/such/config.jsonc' })).rejects.toThrow(
      '/no/such/config.jsonc not found',
    );
  });

  it('refuses a certificate file in verify, which Node.js cannot load after it starts', async () => {
    const path = configFile(
      '{ "host": "x", "httpPort": 1, "wsPort": 2, "verify": "/etc/cert.pem" }',
    );
    await expect(MotionMasterClient.fromConfig({ path })).rejects.toThrow(
      'NODE_EXTRA_CA_CERTS=/etc/cert.pem',
    );
  });
});
