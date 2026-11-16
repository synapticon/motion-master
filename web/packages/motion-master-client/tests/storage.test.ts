import { HttpResponse, http } from 'msw/http';
import { describe, expect, it } from 'vitest';
import { HttpError, MotionMasterClient } from '../src/index';
import { HTTP_URL, server } from './fakes';

describe('HRD recording', () => {
  it('reads the recording as JSON, or as CSV with the Accept header', async () => {
    const received: { query: string; accept: string | null }[] = [];
    server.use(
      http.get(`${HTTP_URL}/api/devices/1/hrd`, ({ request }) => {
        const accept = request.headers.get('Accept');
        received.push({ query: new URL(request.url).search, accept });
        return accept === 'text/csv'
          ? HttpResponse.text('raw\n1\n')
          : HttpResponse.json({ data: 'encoder-raw', columns: ['raw'], samples: [[1]] });
      }),
    );
    const client = new MotionMasterClient({ baseUrl: HTTP_URL });
    expect(await client.readHrdRecording(1, 'encoder-raw')).toMatchObject({ columns: ['raw'] });
    expect(await client.readHrdRecording(1, 'encoder-raw', { format: 'csv' })).toBe('raw\n1\n');
    expect(received[0].query).toBe('?data=encoder-raw');
    expect(received[1]).toEqual({ query: '?data=encoder-raw', accept: 'text/csv' });
  });

  it('throws an HttpError with the reason when the drive holds no recording', async () => {
    server.use(
      http.get(`${HTTP_URL}/api/devices/2/hrd`, () =>
        HttpResponse.json({ error: 'no files to read.' }, { status: 409 }),
      ),
    );
    const client = new MotionMasterClient({ baseUrl: HTTP_URL });
    const error = await client
      .readHrdRecording(2, 'encoder-raw', { format: 'csv' })
      .catch((e: unknown) => e);
    expect(error).toBeInstanceOf(HttpError);
    expect(error).toMatchObject({ status: 409, reason: 'no files to read.' });
  });
});

describe('parameter cache', () => {
  const ID = '000022d2-00000301-v5.6.11';
  const FILE = '{ "parameters": [] }\n';

  it('lists, reads and removes an entry', async () => {
    const calls: string[] = [];
    const entry = { id: ID, vendorId: 0x22d2, productCode: 0x301, firmwareVersion: 'v5.6.11' };
    server.use(
      http.get(`${HTTP_URL}/api/parameter-cache`, () => HttpResponse.json([entry])),
      http.all(`${HTTP_URL}/api/parameter-cache/:id`, ({ request, params }) => {
        calls.push(`${request.method} ${String(params.id)}`);
        return request.method === 'GET'
          ? HttpResponse.text(FILE, { headers: { 'Content-Type': 'application/json' } })
          : new HttpResponse(null, { status: 204 });
      }),
    );
    const client = new MotionMasterClient({ baseUrl: HTTP_URL });
    expect(await client.listParameterCacheEntries()).toEqual([entry]);
    expect(await client.readParameterCacheEntry(ID)).toEqual({ parameters: [] });
    expect(await client.readParameterCacheEntry(ID, { format: 'bytes' })).toEqual(
      new TextEncoder().encode(FILE),
    );
    await client.removeParameterCacheEntry(ID);
    expect(calls).toEqual([`GET ${ID}`, `GET ${ID}`, `DELETE ${ID}`]);
  });
});

describe('server log', () => {
  it('returns the log as text', async () => {
    server.use(http.get(`${HTTP_URL}/api/log`, () => HttpResponse.text('a\nb\n')));
    const client = new MotionMasterClient({ baseUrl: HTTP_URL });
    expect(await client.serverLog()).toBe('a\nb\n');
  });
});
