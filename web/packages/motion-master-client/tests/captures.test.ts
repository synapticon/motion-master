import { describe, expect, it } from 'vitest';
import { CaptureError } from '../src/index';
import { fakeServer } from './fakes';

const SUMMARY = { frames: 10 };

describe('startCapture and stopCapture', () => {
  it('send only the options given, and return the path and the summary', async () => {
    const server = fakeServer({
      'POST /api/captures/start': () => [
        200,
        { path: 'captures/a.pcap', adapter: 'eth0', seconds: 600 },
      ],
      'POST /api/captures/stop': () => [200, { path: 'captures/a.pcap', summary: SUMMARY }],
    });
    expect(await server.client.startCapture()).toBe('captures/a.pcap');
    await server.client.startCapture({ adapter: 'eth1', seconds: 5 });
    expect(await server.client.stopCapture()).toEqual({
      path: 'captures/a.pcap',
      summary: SUMMARY,
    });
    expect(server.requests.map((r) => r.body)).toEqual([
      {},
      { adapter: 'eth1', seconds: 5 },
      undefined,
    ]);
  });

  it('throw a CaptureError with the reason of the server', async () => {
    const server = fakeServer({
      'POST /api/captures/start': () => [409, { error: 'a capture runs already' }],
      'POST /api/captures/stop': () => [409, { error: 'no capture was started' }],
    });
    const start = await server.client.startCapture().catch((e: unknown) => e);
    expect(start).toBeInstanceOf(CaptureError);
    expect(start).toMatchObject({
      message: 'the capture did not start: a capture runs already (HTTP 409)',
    });
    await expect(server.client.stopCapture()).rejects.toThrow(
      'the capture did not stop cleanly: no capture was started (HTTP 409)',
    );
  });
});
