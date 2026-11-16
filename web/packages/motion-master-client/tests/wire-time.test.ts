import { describe, expect, it } from 'vitest';
import { wireTimeMs } from '../src/index';

/** A response with the given Server-Timing header, or none. */
function response(serverTiming?: string): Response {
  return new Response(null, { headers: serverTiming ? { 'Server-Timing': serverTiming } : {} });
}

describe('wireTimeMs', () => {
  it('reads the wire metric of Server-Timing, in milliseconds', () => {
    expect(wireTimeMs(response('wire;dur=1.500'))).toBe(1.5);
    expect(wireTimeMs(response('wire;dur=0.042'))).toBe(0.042);
  });

  it('finds the wire metric among others, with any spacing and parameter order', () => {
    expect(wireTimeMs(response('db;dur=3, wire ; desc="bus" ; dur=2.25'))).toBe(2.25);
  });

  it('returns null without the header, without the metric, and without a duration', () => {
    expect(wireTimeMs(response())).toBeNull();
    expect(wireTimeMs(response('db;dur=3'))).toBeNull();
    expect(wireTimeMs(response('wire'))).toBeNull();
    expect(wireTimeMs(response('wire;dur=fast'))).toBeNull();
  });

  it('reads an entry of the Performance API', () => {
    const entry = {
      serverTiming: [
        { name: 'db', duration: 3, description: '' },
        { name: 'wire', duration: 0.75, description: '' },
      ],
    } as unknown as PerformanceResourceTiming;
    expect(wireTimeMs(entry)).toBe(0.75);
    expect(wireTimeMs({ serverTiming: [] } as unknown as PerformanceResourceTiming)).toBeNull();
  });
});
