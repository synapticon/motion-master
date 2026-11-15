import { HttpResponse, http } from 'msw/http';
import { describe, expect, it } from 'vitest';
import { ProcessDataDumpError } from '../src/index';
import { fakeServer, HTTP_URL, server } from './fakes';

/** Answers the dump with `respond`, and returns the query of each request. */
function dumpRoute(respond: () => Response) {
  const queries: string[] = [];
  server.use(
    http.get(`${HTTP_URL}/api/process-data/dump`, ({ request }) => {
      queries.push(new URL(request.url).search);
      return respond();
    }),
  );
  return queries;
}

describe('processDataMark', () => {
  it('returns the recorder clock of the server', async () => {
    const { client } = fakeServer({
      'GET /api/process-data/mark': () => [200, { timestampUs: 1_794_757_403_495_881 }],
    });
    expect(await client.processDataMark()).toBe(1_794_757_403_495_881);
  });
});

describe('processDataDump', () => {
  it('returns the bytes of the dump, and passes the span as from and to', async () => {
    const { client } = fakeServer({});
    const queries = dumpRoute(() => new HttpResponse(new Uint8Array([0x4d, 0x4d, 0x50, 0x44])));
    expect(await client.processDataDump({ fromUs: 10, toUs: 20 })).toEqual(
      new Uint8Array([0x4d, 0x4d, 0x50, 0x44]),
    );
    await client.processDataDump();
    expect(queries).toEqual(['?from=10&to=20', '']);
  });

  it('returns null when the server has no cycle to dump', async () => {
    const { client } = fakeServer({});
    dumpRoute(() => HttpResponse.json({ error: 'no process image was mapped' }, { status: 409 }));
    expect(await client.processDataDump()).toBeNull();
  });

  it('throws a ProcessDataDumpError with the reason of the server for any other refusal', async () => {
    const { client } = fakeServer({});
    dumpRoute(() => HttpResponse.json({ error: 'from is later than to' }, { status: 400 }));
    const error = await client.processDataDump({ fromUs: 2, toUs: 1 }).catch((e: unknown) => e);
    expect(error).toBeInstanceOf(ProcessDataDumpError);
    expect(error).toMatchObject({
      message: 'the process-data dump failed: from is later than to (HTTP 400)',
    });
  });
});
