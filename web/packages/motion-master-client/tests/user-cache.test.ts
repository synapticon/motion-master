import { HttpResponse, http } from 'msw/http';
import { describe, expect, it } from 'vitest';
import { HttpError, MotionMasterClient, wireTimeMs } from '../src/index';
import { HTTP_URL, server } from './fakes';

const CACHE = `${HTTP_URL}/api/user-cache`;

describe('user cache', () => {
  it('lists the files as the server reports them', async () => {
    const listing = { root: '/cache', files: [{ path: 'dumps/a.mmpd', size: 3, modifiedMs: 1 }] };
    server.use(http.get(CACHE, () => HttpResponse.json(listing)));
    const client = new MotionMasterClient({ baseUrl: HTTP_URL });
    expect(await client.listUserCacheFiles()).toEqual(listing);
  });

  it('encodes each segment of a path and keeps the slashes', async () => {
    const paths: string[] = [];
    server.use(
      http.all(`${CACHE}/*`, ({ request }) => {
        paths.push(`${request.method} ${new URL(request.url).pathname}`);
        return request.method === 'GET'
          ? new HttpResponse(new Uint8Array([1]))
          : HttpResponse.json({ path: 'x', size: 1 });
      }),
    );
    const client = new MotionMasterClient({ baseUrl: HTTP_URL });
    await client.readUserCacheFile('my files/a #1.bin');
    await client.writeUserCacheFile('my files/a #1.bin', new Uint8Array([1]));
    await client.removeUserCacheFile('my files/a #1.bin');
    expect(paths).toEqual(
      ['GET', 'PUT', 'DELETE'].map((m) => `${m} /api/user-cache/my%20files/a%20%231.bin`),
    );
  });

  it('reads a file as bytes, text, JSON or the response', async () => {
    server.use(
      http.get(`${CACHE}/*`, () =>
        HttpResponse.text('{"a":1}', { headers: { 'Server-Timing': 'wire;dur=0.250' } }),
      ),
    );
    const client = new MotionMasterClient({ baseUrl: HTTP_URL });
    expect(await client.readUserCacheFile('x.json')).toEqual(new TextEncoder().encode('{"a":1}'));
    expect(await client.readUserCacheFile('x.json', { format: 'text' })).toBe('{"a":1}');
    expect(await client.readUserCacheFile('x.json', { format: 'json' })).toEqual({ a: 1 });
    const response = await client.readUserCacheFile('x.json', { format: 'response' });
    expect(response.bodyUsed).toBe(false);
    expect(wireTimeMs(response)).toBe(0.25);
  });

  it('writes bytes, or a string as UTF-8, and gives the response', async () => {
    const received: { type: string | null; body: number[] }[] = [];
    server.use(
      http.put(`${CACHE}/*`, async ({ request }) => {
        received.push({
          type: request.headers.get('Content-Type'),
          body: [...new Uint8Array(await request.arrayBuffer())],
        });
        return HttpResponse.json(
          { path: 'x', size: 1 },
          { headers: { 'Server-Timing': 'wire;dur=1.000' } },
        );
      }),
    );
    const client = new MotionMasterClient({ baseUrl: HTTP_URL });
    await client.writeUserCacheFile('a.bin', new Uint8Array([7, 8]));
    const response = await client.writeUserCacheFile('a.txt', 'é');
    expect(wireTimeMs(response)).toBe(1);
    expect(received).toEqual([
      { type: 'application/octet-stream', body: [7, 8] },
      { type: 'application/octet-stream', body: [0xc3, 0xa9] },
    ]);
  });

  it('throws an HttpError with the reason of the server', async () => {
    server.use(
      http.delete(`${CACHE}/*`, () =>
        HttpResponse.json({ error: 'no such file' }, { status: 404 }),
      ),
    );
    const client = new MotionMasterClient({ baseUrl: HTTP_URL });
    const error = await client.removeUserCacheFile('gone.bin').catch((e: unknown) => e);
    expect(error).toBeInstanceOf(HttpError);
    expect(error).toMatchObject({ status: 404, reason: 'no such file' });
  });
});
