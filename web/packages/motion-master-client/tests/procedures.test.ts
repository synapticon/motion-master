import { describe, expect, it } from 'vitest';
import { formatStep, ProcedureError, type ProcedureSnapshot, stepChanges } from '../src/index';
import { type Answer, fakeServer } from './fakes';

const PATH = '/api/devices/1/procedures/store-parameters';

function snapshot(status: ProcedureSnapshot['status'], steps: ProcedureSnapshot['steps']) {
  return { status, runCount: 1, steps };
}

function client(routes: Record<string, (body: unknown) => Answer>) {
  const server = fakeServer(routes);
  return { client: server.client, server };
}

describe('runProcedure', () => {
  it('yields a snapshot at every poll until the run ends', async () => {
    const polls = [
      snapshot('running', [{ id: 'write', status: 'running' }]),
      snapshot('succeeded', [{ id: 'write', status: 'succeeded', value: { ok: true } }]),
    ];
    const { client: c, server } = client({
      [`POST ${PATH}`]: () => [200, snapshot('running', [{ id: 'write', status: 'idle' }])],
      [`GET ${PATH}`]: () => [200, polls.shift()],
    });
    const statuses = [];
    for await (const s of c.runProcedure(1, 'store-parameters', { pollIntervalMs: 1 })) {
      statuses.push(s.status);
    }
    expect(statuses).toEqual(['running', 'running', 'succeeded']);
    expect(server.calls()).not.toContain(`DELETE ${PATH}`);
  });

  it('cancels a run that is still in progress when the loop ends early', async () => {
    const { client: c, server } = client({
      [`POST ${PATH}`]: () => [200, snapshot('running', [])],
      [`DELETE ${PATH}`]: () => [204],
    });
    for await (const s of c.runProcedure(1, 'store-parameters')) {
      expect(s.status).toBe('running');
      break;
    }
    expect(server.calls()).toEqual([`POST ${PATH}`, `DELETE ${PATH}`]);
  });

  it('cancels the run when the signal aborts', async () => {
    const { client: c, server } = client({
      [`POST ${PATH}`]: () => [200, snapshot('running', [])],
      [`GET ${PATH}`]: () => [200, snapshot('running', [])],
      [`DELETE ${PATH}`]: () => [204],
    });
    const controller = new AbortController();
    setTimeout(() => controller.abort(new Error('cancelled')), 10);
    const run = async () => {
      for await (const s of c.runProcedure(1, 'store-parameters', {
        pollIntervalMs: 50,
        signal: controller.signal,
      })) {
        void s;
      }
    };
    await expect(run()).rejects.toThrow('cancelled');
    expect(server.calls().at(-1)).toBe(`DELETE ${PATH}`);
  });

  it('throws a ProcedureError with the reason of the server when it refuses the start', async () => {
    const { client: c } = client({
      [`POST ${PATH}`]: () => [404, { error: "device 1 has no procedure 'store-parameters'" }],
    });
    const run = c.runProcedure(1, 'store-parameters');
    const error = await run.next().catch((e: unknown) => e);
    expect(error).toBeInstanceOf(ProcedureError);
    expect(error).toMatchObject({
      message:
        "store-parameters on device 1: device 1 has no procedure 'store-parameters' (HTTP 404)",
    });
    expect((error as Error).cause).toBeInstanceOf(Response);
  });
});

describe('stepChanges', () => {
  it('lists a step when its status changes, also one that started and ended between two polls', async () => {
    async function* run() {
      yield snapshot('running', [
        { id: 'a', status: 'running' },
        { id: 'b', status: 'idle' },
      ]);
      yield snapshot('running', [
        { id: 'a', status: 'running' },
        { id: 'b', status: 'idle' },
      ]);
      yield snapshot('succeeded', [
        { id: 'a', status: 'succeeded' },
        { id: 'b', status: 'succeeded' },
      ]);
    }
    const changed = [];
    for await (const [, steps] of stepChanges(run())) {
      changed.push(steps.map((s) => `${s.id}:${s.status}`));
    }
    expect(changed).toEqual([['a:running'], [], ['a:succeeded', 'b:succeeded']]);
  });
});

describe('formatStep', () => {
  it('writes the id, the status, and the value or the error as the Python client does', () => {
    expect(formatStep({ id: 'read', status: 'succeeded', value: { a: 1, b: [2, 3] } })).toBe(
      '  read                                 succeeded  {"a":1,"b":[2,3]}',
    );
    expect(formatStep({ id: 'write', status: 'failed', error: 'timeout' })).toBe(
      '  write                                failed  timeout',
    );
    expect(formatStep({ id: 'idle-step', status: 'idle' })).toBe(
      '  idle-step                            idle',
    );
  });
});
