import { HttpResponse, http } from 'msw/http';
import { afterEach, describe, expect, it, vi } from 'vitest';
import {
  AlState,
  CaptureError,
  ConnectionError,
  errorLine,
  HttpError,
  MonitoringError,
  MotionMasterClient,
  ProcedureError,
  ProcessDataDumpError,
  run,
  fieldbusTimeMs,
  StateError,
} from '../src/index';
import { fakeServer, HTTP_URL, server } from './fakes';

afterEach(() => {
  vi.restoreAllMocks();
  process.exitCode = undefined;
  delete process.env.MM_TRACEBACK;
});

describe('errorLine', () => {
  it('names the method, the path, the status and the reason of a refused request', async () => {
    const { client } = fakeServer({
      'GET /api/scan': () => [200, { required: false, devicesOnWire: 1, devicesScanned: 1 }],
      'GET /api/devices/state': () => [
        200,
        [{ slavePosition: 1, alState: 2, reachability: 'answering' }],
      ],
      'POST /api/devices/state/transition': () => [409, { error: 'the bus is busy' }],
    });
    const error = await client.ensureState(AlState.OP).catch((e: unknown) => e);
    expect(errorLine(error)).toBe('POST /api/devices/state/transition: HTTP 409, the bus is busy');
  });

  it('reads a request that the client sends without the generated client the same way', async () => {
    server.use(
      http.post(`${HTTP_URL}/api/eni/parse`, () =>
        HttpResponse.json({ error: 'not an ENI document' }, { status: 400 }),
      ),
    );
    const client = new MotionMasterClient({ baseUrl: HTTP_URL });
    const error = await client.parseEni('<x/>').catch((e: unknown) => e);
    expect(errorLine(error)).toBe('POST /api/eni/parse: HTTP 400, not an ENI document');
  });

  it('names the server that the client cannot reach', async () => {
    server.use(http.all(`${HTTP_URL}/*`, () => HttpResponse.error()));
    const client = new MotionMasterClient({ baseUrl: HTTP_URL });
    const error = await client.devices().catch((e: unknown) => e);
    expect(errorLine(error)).toBe(
      `cannot connect to ${HTTP_URL}. Is the server running, and is config.jsonc correct?`,
    );
  });

  it('gives the message of the errors of the client, and of a missing file', async () => {
    for (const error of [
      new StateError('device 1 is notAnswering'),
      new ProcedureError('offset-detection on device 1: busy (HTTP 409)'),
      new CaptureError('the capture did not start: busy (HTTP 409)'),
      new ProcessDataDumpError('the process-data dump failed: bad span (HTTP 400)'),
      new MonitoringError('monitoring t1: no row after 5000 ms'),
      new ConnectionError('the WebSocket closed'),
      new DOMException('device 1 0x6041:00 is 592 after 1000 ms', 'TimeoutError'),
    ]) {
      expect(errorLine(error)).toBe(error.message);
    }
    const missing = await MotionMasterClient.fromConfig({ path: '/no/such/config.jsonc' }).catch(
      (e: unknown) => e,
    );
    expect(errorLine(missing)).toBe('/no/such/config.jsonc not found');
  });

  it('returns null for a bug', () => {
    expect(errorLine(new TypeError('x is undefined'))).toBeNull();
    expect(errorLine('a string')).toBeNull();
  });
});

describe('the errors of a request', () => {
  it('refuses a base URL without http or https before any request', () => {
    expect(() => new MotionMasterClient({ baseUrl: 'localhost:61447' })).toThrow(
      'baseUrl must be a URL that starts with http or https: "localhost:61447"',
    );
    expect(() => new MotionMasterClient({ wsUrl: 'https://127.0.0.1:62281' })).toThrow(
      'wsUrl must be a URL that starts with ws or wss',
    );
    // A URL that does not parse goes through, so that the Console keeps running on a typo.
    expect(() => new MotionMasterClient({ baseUrl: 'https://bad host:1' })).not.toThrow();
  });

  it('throws an HttpError that keeps the answer unread, for its headers', async () => {
    server.use(
      http.get(`${HTTP_URL}/api/devices`, () =>
        HttpResponse.json(
          { error: 'busy' },
          {
            status: 409,
            statusText: 'Conflict',
            headers: { 'Server-Timing': 'server;dur=0.300, fieldbus;dur=0.120' },
          },
        ),
      ),
    );
    const client = new MotionMasterClient({ baseUrl: HTTP_URL });
    const error = await client.api.getDevices().catch((e: unknown) => e);
    expect(error).toBeInstanceOf(HttpError);
    expect(error).toMatchObject({
      request: { method: 'GET', url: `${HTTP_URL}/api/devices` },
      status: 409,
      reason: 'busy',
      message: 'GET /api/devices: HTTP 409, busy',
    });
    const { response } = error as HttpError;
    expect(response.bodyUsed).toBe(false);
    expect(fieldbusTimeMs(response)).toBe(0.12);
  });

  it('takes the text of the body, or the status text, when the body has no error', async () => {
    server.use(
      http.get(`${HTTP_URL}/api/devices`, () => new HttpResponse('no route', { status: 404 })),
      http.get(
        `${HTTP_URL}/api/version`,
        () => new HttpResponse(null, { status: 503, statusText: 'Service Unavailable' }),
      ),
    );
    const client = new MotionMasterClient({ baseUrl: HTTP_URL });
    expect(await client.api.getDevices().catch((e: unknown) => e)).toMatchObject({
      reason: 'no route',
    });
    expect(await client.api.getVersion().catch((e: unknown) => e)).toMatchObject({
      reason: 'Service Unavailable',
    });
  });

  it('throws a ConnectionError with the error of fetch as its cause', async () => {
    server.use(http.all(`${HTTP_URL}/*`, () => HttpResponse.error()));
    const client = new MotionMasterClient({ baseUrl: HTTP_URL });
    const error = await client.devices().catch((e: unknown) => e);
    expect(error).toBeInstanceOf(ConnectionError);
    expect(error).toMatchObject({ request: { method: 'GET', url: `${HTTP_URL}/api/devices` } });
    expect((error as Error).cause).toBeInstanceOf(TypeError);
  });

  it('throws an abort as it is', async () => {
    const { client } = fakeServer({ 'GET /api/devices': () => [200, []] });
    const error = await client.api
      .getDevices({ signal: AbortSignal.abort() })
      .catch((e: unknown) => e);
    expect(error).toMatchObject({ name: 'AbortError' });
  });
});

describe('run', () => {
  it('prints one line and sets the exit code for an error a user can act on', async () => {
    const printed = vi.spyOn(console, 'error').mockImplementation(() => {});
    await run(() => {
      throw new StateError('no devices on the bus');
    });
    expect(printed).toHaveBeenCalledWith('error: no devices on the bus');
    expect(process.exitCode).toBe(1);
  });

  it('throws a bug again, so that it keeps its stack trace', async () => {
    await expect(
      run(() => {
        throw new TypeError('x is undefined');
      }),
    ).rejects.toThrow('x is undefined');
  });

  it('throws every error again when MM_TRACEBACK is 1', async () => {
    process.env.MM_TRACEBACK = '1';
    await expect(
      run(() => {
        throw new StateError('no devices on the bus');
      }),
    ).rejects.toThrow(StateError);
  });
});
