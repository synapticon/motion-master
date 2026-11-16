import { HttpResponse, http } from 'msw/http';
import { describe, expect, it } from 'vitest';
import { HttpError, MotionMasterClient, serverTimeMs } from '../src/index';
import { HTTP_URL, server } from './fakes';

interface Received {
  path: string;
  query: string;
  type: string | null;
  body: number[];
}

// Answers every parse route with { route } and records what the client sent.
function parseServer(): Received[] {
  const received: Received[] = [];
  server.use(
    http.post(`${HTTP_URL}/api/:kind/parse`, async ({ request, params }) => {
      const url = new URL(request.url);
      received.push({
        path: url.pathname,
        query: url.search,
        type: request.headers.get('Content-Type'),
        body: [...new Uint8Array(await request.arrayBuffer())],
      });
      return HttpResponse.json(
        { route: params.kind },
        { headers: { 'Server-Timing': 'server;dur=12.000' } },
      );
    }),
  );
  return received;
}

describe('parse', () => {
  it('posts a file as raw bytes and returns the decoded result', async () => {
    const received = parseServer();
    const client = new MotionMasterClient({ baseUrl: HTTP_URL });
    const bytes = new Uint8Array([1, 2, 3]);
    expect(await client.parseSii(bytes)).toEqual({ route: 'sii' });
    expect(await client.parseIntegroVariant(bytes)).toEqual({ route: 'integro-variant' });
    expect(await client.parseIcMuRestoreFile(bytes)).toEqual({ route: 'ic-mu-restore-file' });
    expect(received).toEqual(
      ['sii', 'integro-variant', 'ic-mu-restore-file'].map((kind) => ({
        path: `/api/${kind}/parse`,
        query: '',
        type: 'application/octet-stream',
        body: [1, 2, 3],
      })),
    );
  });

  it('posts an ESI as XML, with the modules to merge', async () => {
    const received = parseServer();
    const client = new MotionMasterClient({ baseUrl: HTTP_URL });
    expect(await client.parseEsi('<a/>')).toEqual({ route: 'esi' });
    await client.parseEsi(new Uint8Array([0x3c]), { modules: [0x10, 32] });
    expect(received[0]).toEqual({
      path: '/api/esi/parse',
      query: '',
      type: 'application/xml',
      body: [...new TextEncoder().encode('<a/>')],
    });
    // The generated client encodes the comma, and the server decodes the query.
    expect(received[1]).toMatchObject({ query: '?modules=16%2C32', body: [0x3c] });
  });

  it('gives the response of the generated ESI parse, for its headers', async () => {
    const received = parseServer();
    const client = new MotionMasterClient({ baseUrl: HTTP_URL });
    const response = await client.api.parseEsi(new Uint8Array([0x3c]));
    expect(serverTimeMs(response)).toBe(12);
    expect(response.data).toEqual({ route: 'esi' });
    expect(received[0]).toMatchObject({ type: 'application/xml', body: [0x3c] });
  });

  it('throws an HttpError with the reason of the server', async () => {
    server.use(
      http.post(`${HTTP_URL}/api/sii/parse`, () =>
        HttpResponse.json({ error: 'image too short' }, { status: 400 }),
      ),
    );
    const client = new MotionMasterClient({ baseUrl: HTTP_URL });
    const error = await client.parseSii(new Uint8Array([1])).catch((e: unknown) => e);
    expect(error).toBeInstanceOf(HttpError);
    expect(error).toMatchObject({ status: 400, reason: 'image too short' });
  });
});
