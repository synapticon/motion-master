import { HttpResponse, http } from 'msw/http';
import { describe, expect, it } from 'vitest';
import { apiErrorMessage, HttpError } from '../src/index';
import { fakeServer, HTTP_URL, server } from './fakes';

const XML = '<?xml version="1.0"?><EtherCATConfig/>';

describe('exportEni', () => {
  it('returns the document and the number of parts the server could not read', async () => {
    const { client } = fakeServer({});
    server.use(
      http.get(`${HTTP_URL}/api/eni`, () =>
        HttpResponse.xml(XML, { headers: { 'X-Eni-Warnings': '2' } }),
      ),
    );
    expect(await client.exportEni()).toEqual({ xml: XML, warnings: 2 });
  });

  it('throws an HttpError with the reason of the server', async () => {
    const { client } = fakeServer({});
    server.use(
      http.get(`${HTTP_URL}/api/eni`, () =>
        HttpResponse.json({ error: 'the bus is not in SAFEOP or OP' }, { status: 409 }),
      ),
    );
    const error = await client.exportEni().catch((e: unknown) => e);
    expect(error).toBeInstanceOf(HttpError);
    expect(error).toMatchObject({
      status: 409,
      request: { method: 'GET', url: `${HTTP_URL}/api/eni` },
    });
    expect(apiErrorMessage(error)).toBe('the bus is not in SAFEOP or OP');
  });
});

describe('parseEni', () => {
  it('sends the document as XML and returns the parsed network', async () => {
    const { client } = fakeServer({});
    const sent: { type: string | null; body: string }[] = [];
    server.use(
      http.post(`${HTTP_URL}/api/eni/parse`, async ({ request }) => {
        sent.push({ type: request.headers.get('Content-Type'), body: await request.text() });
        return HttpResponse.json({ network: {}, warnings: [], summary: { devices: 1 } });
      }),
    );
    const result = await client.parseEni(XML);
    expect(result.summary).toEqual({ devices: 1 });
    expect(sent).toEqual([{ type: 'application/xml', body: XML }]);
  });
});
