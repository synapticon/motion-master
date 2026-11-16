import { describe, expect, it } from 'vitest';
import { fieldbusTimeMs, serverTimeMs } from '../src/index';

/** A response with the given Server-Timing header, or none. */
function response(serverTiming?: string): Response {
  return new Response(null, { headers: serverTiming ? { 'Server-Timing': serverTiming } : {} });
}

describe('serverTimeMs and fieldbusTimeMs', () => {
  it('read their metric of Server-Timing, in milliseconds', () => {
    const both = response('server;dur=7.100, fieldbus;dur=6.200');
    expect(serverTimeMs(both)).toBe(7.1);
    expect(fieldbusTimeMs(both)).toBe(6.2);
    expect(fieldbusTimeMs(response('fieldbus;dur=0.042'))).toBe(0.042);
  });

  it('find the metric among others, with any spacing and parameter order', () => {
    expect(fieldbusTimeMs(response('db;dur=3, fieldbus ; desc="bus" ; dur=2.25'))).toBe(2.25);
  });

  it('return null without the header, without the metric, and without a duration', () => {
    expect(serverTimeMs(response())).toBeNull();
    expect(fieldbusTimeMs(response('server;dur=0.300'))).toBeNull();
    expect(fieldbusTimeMs(response('fieldbus'))).toBeNull();
    expect(fieldbusTimeMs(response('fieldbus;dur=fast'))).toBeNull();
  });

  it('read an entry of the Performance API', () => {
    const entry = {
      serverTiming: [
        { name: 'server', duration: 3, description: '' },
        { name: 'fieldbus', duration: 0.75, description: '' },
      ],
    } as unknown as PerformanceResourceTiming;
    expect(serverTimeMs(entry)).toBe(3);
    expect(fieldbusTimeMs(entry)).toBe(0.75);
    expect(fieldbusTimeMs({ serverTiming: [] } as unknown as PerformanceResourceTiming)).toBeNull();
  });
});
