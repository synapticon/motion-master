import { describe, expect, it } from 'vitest';
import { AlState, Cia402, OperationMode, StateError } from '../src/index';
import { type Answer, fakeServer } from './fakes';

function client(routes: Record<string, (body: unknown) => Answer>, fieldbus = true) {
  const server = fakeServer(
    routes,
    fieldbus ? { fieldbus: { driver: 'soem' as const, networkAdapter: 'eth0' } } : {},
  );
  return { client: server.client, server };
}

function device(slavePosition: number, alState: number, reachability = 'answering') {
  return { slavePosition, alState, alStatus: alState, alStatusCode: 0, error: false, reachability };
}

describe('init', () => {
  it('sends the fieldbus settings of the client', async () => {
    const { client: c, server } = client({ 'POST /api/init': () => [200, { ok: true }] });
    await c.init();
    expect(server.requests[0].body).toEqual({ driver: 'soem', networkAdapter: 'eth0' });
  });

  it('takes a driver that is open already, 409, as success', async () => {
    const { client: c } = client({ 'POST /api/init': () => [409, { error: 'already' }] });
    await expect(c.init()).resolves.toBeUndefined();
  });

  it('throws on any other refusal', async () => {
    const { client: c } = client({
      'POST /api/init': () => [400, { error: 'bad network adapter' }],
    });
    await expect(c.init()).rejects.toMatchObject({ status: 400 });
  });

  it('refuses to run without fieldbus settings', async () => {
    const { client: c, server } = client({}, false);
    await expect(c.init()).rejects.toThrow('no fieldbus settings');
    expect(server.requests).toHaveLength(0);
  });
});

describe('ensureScanned', () => {
  it('scans when no scan ran, and throws a StateError on an empty bus', async () => {
    const { client: c, server } = client({
      'GET /api/scan': () => [200, { required: false, devicesOnWire: 0, devicesScanned: 0 }],
      'POST /api/scan': () => [200, { slaves: 0 }],
    });
    await expect(c.ensureScanned()).rejects.toThrow(new StateError('no devices on the bus'));
    expect(server.calls()).toEqual(['GET /api/scan', 'POST /api/scan']);
  });

  it('does not scan when the last scan is right', async () => {
    const { client: c, server } = client({
      'GET /api/scan': () => [200, { required: false, devicesOnWire: 1, devicesScanned: 1 }],
    });
    await c.ensureScanned();
    expect(server.calls()).toEqual(['GET /api/scan']);
  });
});

describe('ensureState', () => {
  const scanned = () =>
    [200, { required: false, devicesOnWire: 2, devicesScanned: 2 }] satisfies Answer;

  it('moves only the devices below the state, and leaves a device above it', async () => {
    const { client: c, server } = client({
      'GET /api/scan': scanned,
      'GET /api/devices/state': () => [200, [device(1, AlState.OP), device(2, AlState.PREOP)]],
      'POST /api/devices/state/transition': () => [200, { ok: true, devices: [] }],
    });
    await c.ensureState(AlState.SAFEOP);
    expect(server.requests.at(-1)?.body).toEqual({ state: AlState.SAFEOP, positions: [2] });
  });

  it('moves a device above the state too when exact is true', async () => {
    const { client: c, server } = client({
      'GET /api/scan': scanned,
      'GET /api/devices/state': () => [200, [device(1, AlState.OP)]],
      'POST /api/devices/state/transition': () => [200, { ok: true, devices: [] }],
    });
    await c.ensureState(AlState.PREOP, { positions: [1], exact: true });
    expect(server.calls()).toContain('GET /api/devices/state?positions=1');
    expect(server.requests.at(-1)?.body).toEqual({ state: AlState.PREOP, positions: [1] });
  });

  it('treats BOOT as exact', async () => {
    const { client: c, server } = client({
      'GET /api/scan': scanned,
      'GET /api/devices/state': () => [200, [device(1, AlState.OP)]],
      'POST /api/devices/state/transition': () => [200, { ok: true, devices: [] }],
    });
    await c.ensureState(AlState.BOOT);
    expect(server.requests.at(-1)?.body).toEqual({ state: AlState.BOOT, positions: [1] });
  });

  it('throws a StateError for a device that does not answer', async () => {
    const { client: c } = client({
      'GET /api/scan': scanned,
      'GET /api/devices/state': () => [200, [device(1, AlState.UNKNOWN, 'notAnswering')]],
    });
    await expect(c.ensureState(AlState.PREOP)).rejects.toThrow('device 1 is notAnswering');
  });

  it('throws a StateError that names the device that missed the state', async () => {
    const { client: c } = client({
      'GET /api/scan': scanned,
      'GET /api/devices/state': () => [200, [device(2, AlState.PREOP)]],
      'POST /api/devices/state/transition': () => [
        200,
        {
          ok: false,
          devices: [{ ...device(2, AlState.SAFEOP), alStatusCode: 0x1d, reached: false }],
        },
      ],
    });
    await expect(c.ensureState(AlState.OP)).rejects.toThrow(
      'not every device reached OP: device 2 in SAFEOP with alStatusCode 0x001D',
    );
  });
});

describe('restart', () => {
  it('restarts every device when no positions are given', async () => {
    const { client: c, server } = client({
      'GET /api/scan': () => [200, { required: false, devicesOnWire: 1, devicesScanned: 1 }],
      'POST /api/devices/state/restart': () => [200, { ok: true, step: 8, devices: [] }],
    });
    await c.restart(AlState.OP);
    expect(server.requests.at(-1)?.body).toEqual({ target: AlState.OP });
  });
});

describe('waitForValue', () => {
  it('returns the first value that meets the predicate', async () => {
    let statusword = 0;
    const { client: c } = client({
      'GET /api/devices/1/parameters/24641/0': () => {
        statusword += 0x0400;
        return [200, { value: statusword }];
      },
    });
    const value = await c.waitForValue(1, Cia402.STATUSWORD, (sw) => sw >= 0x0c00, {
      intervalMs: 1,
    });
    expect(value).toBe(0x0c00);
  });

  it('throws a TimeoutError with the last value', async () => {
    const { client: c } = client({
      'GET /api/devices/1/parameters/24641/0': () => [200, { value: 7 }],
    });
    await expect(
      c.waitForValue(1, Cia402.STATUSWORD, () => false, { timeoutMs: 20, intervalMs: 5 }),
    ).rejects.toMatchObject({
      name: 'TimeoutError',
      message: 'device 1 0x6041:00 is 7 after 20 ms',
    });
  });

  it('stops with the reason of an aborted signal', async () => {
    const { client: c } = client({
      'GET /api/devices/1/parameters/24641/0': () => [200, { value: 7 }],
    });
    const controller = new AbortController();
    setTimeout(() => controller.abort(new Error('stopped')), 15);
    await expect(
      c.waitForValue(1, Cia402.STATUSWORD, () => false, {
        timeoutMs: 5000,
        intervalMs: 5,
        signal: controller.signal,
      }),
    ).rejects.toThrow('stopped');
  });
});

describe('CiA402 helpers', () => {
  it('send the same requests as the Python client', async () => {
    const answer = () => [200, { state: 'OperationEnabled' }] satisfies Answer;
    const { client: c, server } = client({
      'POST /api/devices/1/cia402/mode': answer,
      'POST /api/devices/1/cia402/target': () => [204],
      'POST /api/devices/1/cia402/state': answer,
      'POST /api/devices/1/cia402/command': answer,
    });
    await c.setOperationMode(1, OperationMode.CYCLIC_SYNC_VELOCITY);
    await c.setTarget(1, 'velocity', 250);
    await c.enable(1);
    await c.disable(1);
    await c.quickStop(1);
    expect(server.requests.map((r) => [r.path.split('/').at(-1), r.body])).toEqual([
      ['mode', { mode: 9 }],
      ['target', { target: 'velocity', value: 250 }],
      ['state', { state: 'OperationEnabled' }],
      ['state', { state: 'SwitchOnDisabled' }],
      ['command', { command: 'quickStop' }],
    ]);
  });
});
