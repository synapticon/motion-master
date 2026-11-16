import { HttpResponse, http } from 'msw/http';
import { describe, expect, it } from 'vitest';
import {
  Cia402,
  deviceParameterName,
  deviceParameterText,
  formatHexBytes,
  MotionMasterClient,
  objectAddressText,
  wireTimeMs,
} from '../src/index';
import { fakeServer, HTTP_URL, server } from './fakes';

const FILES = `${HTTP_URL}/api/devices/1/files`;

describe('object values', () => {
  it('writes a value and returns the one the server reads back', async () => {
    const { client, requests } = fakeServer({
      'PUT /api/devices/1/parameters/24831/0': (body) => [
        200,
        { value: (body as { value: number }).value },
      ],
    });
    expect(await client.writeValue(1, Cia402.TARGET_VELOCITY, 250)).toBe(250);
    expect(requests[0].body).toEqual({ value: 250 });
  });

  it('reads and writes the raw bytes of an object', async () => {
    const { client, requests } = fakeServer({
      'GET /api/devices/1/sdo/24641/0': () => [200, { data: [0x37, 0x02] }],
      'PUT /api/devices/1/sdo/24640/0': () => [200, { ok: true }],
    });
    expect(await client.readSdo(1, 0x6041, 0)).toEqual(new Uint8Array([0x37, 0x02]));
    await client.writeSdo(1, 0x6040, 0, new Uint8Array([0x06, 0x00]));
    expect(requests[1].body).toEqual({ data: [0x06, 0x00] });
  });
});

describe('files', () => {
  it('lists the files as the server reports them', async () => {
    const files = [{ name: 'config.csv', byteCount: 120 }, { name: '.hardware_description' }];
    const { client } = fakeServer({ 'GET /api/devices/1/files': () => [200, files] });
    expect(await client.listFiles(1)).toEqual(files);
  });

  it('reads a file as bytes, as text or as JSON', async () => {
    server.use(
      http.get(`${FILES}/:name`, ({ params }) => {
        const body = params.name === 'data.json' ? '{"a":1}' : 'index,value\n1,é\n';
        return new HttpResponse(new TextEncoder().encode(body));
      }),
    );
    const client = new MotionMasterClient({ baseUrl: HTTP_URL });
    const bytes = await client.readFile(1, 'config.csv');
    expect(bytes).toBeInstanceOf(Uint8Array);
    expect(bytes.length).toBe(17);
    expect(await client.readFile(1, 'config.csv', { format: 'text' })).toBe('index,value\n1,é\n');
    expect(await client.readFile(1, 'data.json', { format: 'json' })).toEqual({ a: 1 });
  });

  it('writes a string as UTF-8 and bytes as they are, as an octet stream', async () => {
    const received: { type: string | null; body: number[] }[] = [];
    server.use(
      http.put(`${FILES}/:name`, async ({ request }) => {
        received.push({
          type: request.headers.get('Content-Type'),
          body: [...new Uint8Array(await request.arrayBuffer())],
        });
        return HttpResponse.json({ ok: true });
      }),
    );
    const client = new MotionMasterClient({ baseUrl: HTTP_URL });
    await client.writeFile(1, 'note.txt', 'é');
    await client.writeFile(1, 'image.bin', new Uint8Array([0, 255]));
    expect(received).toEqual([
      { type: 'application/octet-stream', body: [0xc3, 0xa9] },
      { type: 'application/octet-stream', body: [0, 255] },
    ]);
  });

  it('removes a file, and throws the refusal of the server', async () => {
    const { client, calls } = fakeServer({
      'DELETE /api/devices/1/files/old.bin': () => [200, { ok: true }],
      'DELETE /api/devices/1/files/missing.bin': () => [404, { error: 'no such file' }],
    });
    await client.removeFile(1, 'old.bin');
    await expect(client.removeFile(1, 'missing.bin')).rejects.toMatchObject({
      name: 'HttpError',
      reason: 'no such file',
    });
    expect(calls()).toEqual([
      'DELETE /api/devices/1/files/old.bin',
      'DELETE /api/devices/1/files/missing.bin',
    ]);
  });
});

describe('the answer of a file request', () => {
  it('gives the response of a read and a write, for its headers', async () => {
    server.use(
      http.all(`${FILES}/*`, ({ request }) => {
        const headers = { 'Server-Timing': 'wire;dur=1.500' };
        return request.method === 'GET'
          ? new HttpResponse(new Uint8Array([7, 8]), { headers })
          : HttpResponse.json({ ok: true }, { headers });
      }),
    );
    const client = new MotionMasterClient({ baseUrl: HTTP_URL });
    const read = await client.readFile(1, 'config.csv', { format: 'response' });
    expect(read.bodyUsed).toBe(false);
    expect(wireTimeMs(read)).toBe(1.5);
    expect(new Uint8Array(await read.arrayBuffer())).toEqual(new Uint8Array([7, 8]));
    expect(wireTimeMs(await client.writeFile(1, 'note.txt', 'x'))).toBe(1.5);
  });
});

describe('SII', () => {
  it('reads the parsed SII, or the raw image, and writes a raw image', async () => {
    const received: { accept: string | null; type: string | null; body: number[] }[] = [];
    server.use(
      http.all(`${HTTP_URL}/api/devices/1/sii`, async ({ request }) => {
        received.push({
          accept: request.headers.get('Accept'),
          type: request.headers.get('Content-Type'),
          body: [...new Uint8Array(await request.arrayBuffer())],
        });
        if (request.method === 'PUT') {
          return HttpResponse.json({ ok: true });
        }
        return request.headers.get('Accept') === 'application/octet-stream'
          ? new HttpResponse(new Uint8Array([0x01, 0x02]))
          : HttpResponse.json({ general: { nameIdx: 1 } });
      }),
    );
    const client = new MotionMasterClient({ baseUrl: HTTP_URL });
    expect(await client.readSii(1)).toEqual({ general: { nameIdx: 1 } });
    expect(await client.readSii(1, { format: 'bytes' })).toEqual(new Uint8Array([1, 2]));
    await client.writeSii(1, new Uint8Array([9, 8]));
    expect(received.at(-1)).toMatchObject({ type: 'application/octet-stream', body: [9, 8] });
    expect(received[1].accept).toBe('application/octet-stream');
  });
});

describe('file names in the path', () => {
  it('percent-encodes the name in every file request', async () => {
    const paths: string[] = [];
    server.use(
      http.all(`${FILES}/*`, ({ request }) => {
        paths.push(`${request.method} ${new URL(request.url).pathname}`);
        return request.method === 'GET'
          ? new HttpResponse(new Uint8Array([1]))
          : HttpResponse.json({ ok: true });
      }),
    );
    const client = new MotionMasterClient({ baseUrl: HTTP_URL });
    await client.readFile(1, 'a b.txt');
    await client.writeFile(1, 'a+b.txt', 'x');
    await client.removeFile(1, 'fs-stackunlock=DD1317');
    expect(paths).toEqual([
      'GET /api/devices/1/files/a%20b.txt',
      'PUT /api/devices/1/files/a%2Bb.txt',
      'DELETE /api/devices/1/files/fs-stackunlock%3DDD1317',
    ]);
  });
});

describe('text of bytes and addresses', () => {
  it('formats bytes as hex in capitals', () => {
    expect(formatHexBytes([0x0a, 0xff])).toBe('0A FF');
    expect(formatHexBytes(new Uint8Array([1, 2]), ',')).toBe('01,02');
    expect(formatHexBytes([])).toBe('');
  });

  it('writes an object address in hex', () => {
    expect(objectAddressText(0x6041, 0)).toBe('0x6041:00');
    expect(objectAddressText(0x2030, 0x1a)).toBe('0x2030:1A');
  });

  it('writes a parameter of a device as its position and its address or name', () => {
    expect(deviceParameterText(1, 0x6041, 0)).toBe('1:0x6041:00');
    expect(deviceParameterName(2, 0x6041, 0)).toBe('2:STATUSWORD');
    expect(deviceParameterName(1, 0x2fff, 0x0b)).toBe('1:0x2FFF:0B');
  });
});
