// The MotionMasterClient facade: a single connection object that composes the generated HTTP API
// (`client.api.*`, flat — e.g. `client.api.getVersion()`) with the WebSocket connection
// (`client.ws`). It is a *connection, not a model*: it holds no mutable device/parameter/AL state
// — the HTTP API and the monitoring stream are the source of truth. Isomorphic: pass `WebSocket`
// (and `customFetch`) to run under Node; both default to the platform globals in the browser.

import createDebug from 'debug';
import { AlState, alStateName } from './al-state';
import type { Cia402State, OperationMode } from './cia402';
import { readConfig, type FieldbusSettings } from './config';
import { ProcedureError, StateError } from './errors';
import { Api } from './generated/Api';
import type { Cia402Status, ProcedureRequest, ProcedureSnapshot } from './generated/data-contracts';
import type { ApiConfig } from './generated/http-client';
import { API_BASE_URL, WS_URL } from './constants';
import { fetchProcessDataDump, type MmpdFile } from './mmpd';
import type { ObjectAddress } from './objects';
import { listen } from './stream';
import {
  type MonitoringBatch,
  WebSocketConnection,
  type WebSocketConstructor,
} from './web-socket-connection';

export interface MotionMasterClientOptions {
  /** Origin (or `host:port`) of the HTTP API. Default: the bundled localhost dev origin. */
  baseUrl?: string;
  /** Full URL of the WebSocket connection. Default: the bundled localhost dev origin. */
  wsUrl?: string;
  /**
   * WebSocket implementation for `client.ws`. Defaults to the global `WebSocket` (browsers,
   * Node >= 22); inject the `ws` package's constructor on older Node.
   */
  WebSocket?: WebSocketConstructor;
  /**
   * Second argument for the WebSocket constructor — e.g. `{ rejectUnauthorized: false }` for the
   * Node `ws` client against a self-signed dev cert. Ignored by the browser global.
   */
  webSocketOptions?: unknown;
  /** Custom `fetch` forwarded to the HTTP client (e.g. request logging in tests). */
  customFetch?: ApiConfig['customFetch'];
  /** The body of `POST /api/init`, for example `{ driver: 'soem', adapter: 'eth0' }`. */
  fieldbus?: FieldbusSettings;
}

export type { FieldbusSettings };

// One line for each step a helper takes, such as "found 1 device(s)". Off by default. Turn it on with
// DEBUG=motion-master-client in Node.js, or localStorage.debug = 'motion-master-client' in a
// browser.
const log = createDebug('motion-master-client');

/** An AL state that a device can be commanded to. `UNKNOWN` is only ever reported. */
export type TargetAlState = Exclude<AlState, typeof AlState.UNKNOWN>;

type Data<K extends keyof Api> = Api[K] extends (...args: never[]) => Promise<{ data: infer D }>
  ? D
  : never;

/** The target that `setTarget` writes: 0x607A, 0x60FF or 0x6071. */
export type Cia402TargetKind = Parameters<Api['setCia402Target']>[1]['target'];

/** A scanned device, as `GET /api/devices` gives it. */
export type DeviceInfo = Data<'getDevices'>[number];

// The states a device climbs through, from the bottom. BOOT is not on it, because BOOT is reachable
// only from INIT. satisfied() orders the states with it.
const LADDER: AlState[] = [AlState.INIT, AlState.PREOP, AlState.SAFEOP, AlState.OP];

/** Returns whether a device in `current` already meets `target`. */
function satisfied(current: AlState, target: AlState, exact: boolean): boolean {
  if (current === target) {
    return true;
  }
  if (exact || current === AlState.BOOT || target === AlState.BOOT) {
    return false;
  }
  return LADDER.indexOf(current) > LADDER.indexOf(target);
}

/** Says which devices of a state change did not reach `step`, and why. */
function notReached(
  result: Data<'transitionToState'> | Data<'restartDevices'>,
  step: AlState,
): string {
  const failed = result.devices
    .filter((d) => !d.reached)
    .map(
      (d) =>
        `device ${d.slavePosition} in ${alStateName(d.alState)} with alStatusCode 0x` +
        d.alStatusCode.toString(16).padStart(4, '0'),
    )
    .join(', ');
  return `not every device reached ${alStateName(step)}: ${failed}`;
}

/** Waits `ms` milliseconds, or rejects with the reason of `signal` when it aborts first. */
function sleep(ms: number, signal?: AbortSignal): Promise<void> {
  return new Promise((resolve, reject) => {
    if (signal?.aborted) {
      reject(signal.reason);
      return;
    }
    const onAbort = () => {
      clearTimeout(timer);
      reject(signal?.reason);
    };
    const timer = setTimeout(() => {
      signal?.removeEventListener('abort', onAbort);
      resolve();
    }, ms);
    signal?.addEventListener('abort', onAbort, { once: true });
  });
}

/** Returns whether `error` is a response of the generated client with the HTTP status `status`. */
function hasStatus(error: unknown, status: number): boolean {
  return error instanceof Response && error.status === status;
}

export class MotionMasterClient {
  /**
   * The generated HTTP API client. Flat method surface: `client.api.getVersion()`,
   * `client.api.scan()`, `client.api.sdoUpload(...)`, ...
   */
  readonly api: Api;

  /**
   * The WebSocket connection: `client.ws.subscribe(topic, cb)`,
   * `client.ws.onNotification(cb)`, `client.ws.onProgress(cb)`. Connects lazily.
   */
  readonly ws: WebSocketConnection;

  /** The body of `POST /api/init`, from the options or from config.jsonc. */
  readonly fieldbus?: FieldbusSettings;

  private readonly httpBaseUrl: string;
  private readonly customFetch?: ApiConfig['customFetch'];

  constructor(options: MotionMasterClientOptions = {}) {
    this.httpBaseUrl = options.baseUrl ?? API_BASE_URL;
    this.customFetch = options.customFetch;
    this.fieldbus = options.fieldbus;
    // The generated HttpClient copies its config onto itself with Object.assign, so a
    // `customFetch: undefined` replaces its default fetch and every request fails with
    // "this.customFetch is not a function". Pass the key only when there is a value.
    this.api = new Api({
      baseUrl: this.httpBaseUrl,
      ...(options.customFetch ? { customFetch: options.customFetch } : {}),
    });
    this.ws = new WebSocketConnection({
      url: options.wsUrl ?? WS_URL,
      WebSocket: options.WebSocket,
      protocolsOrOptions: options.webSocketOptions,
    });
  }

  /**
   * Builds a client from config.jsonc. Node.js only.
   *
   * Without `path`, it reads config.jsonc in the current directory, and else config.jsonc in the
   * folder of the package. The first is for a client installed from npm. The second is for a clone
   * of the repository. The file gives the host, the two ports, the TLS verification and the
   * `fieldbus` object, the body of `POST /api/init`. config.example.jsonc is the template.
   *
   * `verify: false` turns off TLS verification for the whole process, because the built-in fetch
   * and WebSocket of Node.js take no TLS option. A certificate file in `verify` is refused: start
   * node with `NODE_EXTRA_CA_CERTS` instead.
   */
  static async fromConfig(options: { path?: string } = {}): Promise<MotionMasterClient> {
    return new MotionMasterClient(await readConfig(options.path));
  }

  /**
   * Opens the fieldbus driver, unless one is open already.
   *
   * Uses `fieldbus` when given, otherwise the settings of the client. Unlike `api.init`, a 409
   * "already initialised" counts as success, so a program can call this every time it starts.
   */
  async init(options: { fieldbus?: FieldbusSettings } = {}): Promise<void> {
    const body = options.fieldbus ?? this.fieldbus;
    if (body === undefined) {
      throw new Error('no fieldbus settings: pass them to MotionMasterClient or to init()');
    }
    try {
      await this.api.init(body);
      log('opened the %s driver', body.driver ?? 'soem');
    } catch (error) {
      if (!hasStatus(error, 409)) {
        throw error;
      }
    }
  }

  /**
   * Closes the fieldbus driver and forgets every device (`reset`).
   *
   * The server commands no AL state, so the devices stay in the state they are in. A reset with no
   * driver open also succeeds. Call `init()` before the next step that needs the bus.
   */
  async reset(): Promise<void> {
    await this.api.reset();
    log('closed the driver and forgot every device');
  }

  /**
   * Scans the bus and returns the number of devices found (`scan`).
   *
   * The devices stay in INIT. Call `init()` first. Unlike `ensureScanned()`, this scans every time
   * and returns 0 rather than throwing when the bus is empty.
   */
  async scan(): Promise<number> {
    const found = (await this.api.scan()).data.slaves;
    log('found %d device(s)', found);
    return found;
  }

  /**
   * Returns the scanned devices, in bus order, as `GET /api/devices` gives them (`getDevices`).
   * The list is empty before the first scan.
   */
  async devices(): Promise<DeviceInfo[]> {
    return (await this.api.getDevices()).data;
  }

  /**
   * Returns every parameter of a device, each with its value and metadata. The list is the object
   * dictionary as the server enumerated it (`getDeviceParameters`).
   */
  async parameters(slavePosition: number): Promise<Data<'getDeviceParameters'>> {
    return (await this.api.getDeviceParameters(slavePosition)).data;
  }

  /** Returns the .hardware_description of a device, parsed (`getDeviceHardwareDescription`). */
  async hardwareDescription(slavePosition: number): Promise<Data<'getDeviceHardwareDescription'>> {
    return (await this.api.getDeviceHardwareDescription(slavePosition)).data;
  }

  /**
   * Reads one object of a device and returns its value, decoded by the server. The address gives
   * the type of the value: `readValue(1, Cia402.STATUSWORD)` is a `Promise<number>`.
   */
  async readValue<T>(slavePosition: number, address: ObjectAddress<T>): Promise<T> {
    const response = await this.api.readParameter(slavePosition, address.index, address.subindex);
    return response.data.value as T;
  }

  /**
   * Reads an object until `predicate(value)` is true, and returns that value.
   *
   * Each read is `readValue()`, so a PDO-mapped object of an exchanging device costs no bus
   * transfer. `timeoutMs` is 1000 and `intervalMs` 50 when not given.
   *
   *     await client.waitForValue(1, Cia402.STATUSWORD, (sw) => (sw & 0x0400) !== 0); // target reached
   *
   * Throws a `DOMException` named `TimeoutError` with the last value when the predicate is still
   * false after `timeoutMs`, and the reason of `signal` when it aborts.
   */
  async waitForValue<T>(
    slavePosition: number,
    address: ObjectAddress<T>,
    predicate: (value: T) => unknown,
    options: { timeoutMs?: number; intervalMs?: number; signal?: AbortSignal } = {},
  ): Promise<T> {
    const { timeoutMs = 1000, intervalMs = 50, signal } = options;
    const deadline = performance.now() + timeoutMs;
    for (;;) {
      signal?.throwIfAborted();
      const value = await this.readValue(slavePosition, address);
      if (predicate(value)) {
        return value;
      }
      if (performance.now() >= deadline) {
        const index = address.index.toString(16).padStart(4, '0');
        const subindex = address.subindex.toString(16).padStart(2, '0');
        throw new DOMException(
          `device ${slavePosition} 0x${index}:${subindex} is ${String(value)} after ${timeoutMs} ms`,
          'TimeoutError',
        );
      }
      await sleep(intervalMs, signal);
    }
  }

  /**
   * Scans when the last scan is missing or wrong, and moves no device.
   *
   * A program that needs the devices known but no AL state calls this instead of `ensureState()`,
   * so that it leaves every device where it is, BOOT included. Call `init()` first.
   *
   * Throws a `StateError` when the scan finds no device.
   */
  async ensureScanned(): Promise<void> {
    // Over SPoE, devicesOnWire is null, so `required` stays false before the first scan.
    // devicesScanned is 0 until a scan ran on either driver.
    const scan = (await this.api.getScanStatus()).data;
    if (scan.required || scan.devicesScanned === 0) {
      log('scanning the bus');
      if ((await this.scan()) === 0) {
        throw new StateError('no devices on the bus');
      }
    }
  }

  /**
   * Brings the devices to the AL state a program needs.
   *
   * Scans when the last scan is missing or wrong, then asks the server to walk each device to
   * `state` (`transitionToState`), one legal transition at a time. A device already above `state`
   * stays where it is, because SAFEOP and OP keep the mailbox of PREOP. Pass `exact: true` when the
   * program needs that one state. A target of BOOT is always exact. Without `positions`, every
   * scanned device counts. Call `init()` first.
   *
   * Throws a `StateError` when a device does not answer or does not reach `state`.
   */
  async ensureState(
    state: TargetAlState,
    options: { positions?: number[]; exact?: boolean } = {},
  ): Promise<void> {
    const { positions, exact = false } = options;
    await this.ensureScanned();

    const query = positions?.length ? { positions: positions.join(',') } : undefined;
    const devices = (await this.api.getDeviceStates(query)).data;
    for (const d of devices) {
      if (d.reachability !== 'answering') {
        throw new StateError(`device ${d.slavePosition} is ${d.reachability}`);
      }
    }
    const group = devices
      .filter((d) => !satisfied(d.alState, state, exact))
      .map((d) => d.slavePosition);
    if (group.length === 0) {
      return;
    }
    log('device(s) [%s] -> %s', group.join(', '), alStateName(state));
    const result = (await this.api.transitionToState({ state, positions: group })).data;
    if (!result.ok) {
      throw new StateError(notReached(result, state));
    }
  }

  /**
   * Restarts devices through INIT, then walks them up to the AL state `target`.
   *
   * The server does the walk (`restartDevices`), one state at a time. Without `positions`, every
   * scanned device restarts. Scans first when the last scan is missing or wrong. Call `init()`
   * first. A restart drops each device out of process data exchange, so a drive that was enabled
   * is disabled.
   *
   * Throws a `StateError` when a device does not reach a step.
   */
  async restart(target: TargetAlState, options: { positions?: number[] } = {}): Promise<void> {
    const { positions } = options;
    await this.ensureScanned();
    const who = positions?.length ? `[${positions.join(', ')}]` : 'all';
    log('restart device(s) %s -> %s', who, alStateName(target));
    const body = positions?.length ? { target, positions } : { target };
    const result = (await this.api.restartDevices(body)).data;
    if (!result.ok) {
      throw new StateError(notReached(result, result.step));
    }
  }

  /**
   * Subscribes to the topic of a monitoring and yields each batch of rows from the WebSocket.
   * The monitoring must exist already: create it with `client.api.createMonitoring`. A batch is a
   * list of rows, `[timestampUs, v0, v1, ...]`, with the values in the order of the parameters of
   * the monitoring. The stream is lossless: a batch holds every cycle recorded since the batch
   * before. Leaving the `for await` loop unsubscribes.
   *
   *     for await (const rows of client.streamMonitoring('left-leg')) { ... }
   */
  streamMonitoring(topic: string): AsyncGenerator<MonitoringBatch, void, undefined> {
    return listen((listener: (rows: MonitoringBatch) => void) =>
      this.ws.subscribe(topic, listener),
    );
  }

  /**
   * Writes the CiA402 operation mode (0x6060) of a drive (`setCia402OperationMode`), such as
   * `OperationMode.CYCLIC_SYNC_VELOCITY`.
   *
   * The server waits until 0x6061 shows the mode, so the drive runs in it when this returns.
   * Returns the CiA402 status. Throws the response, 409, when the drive does not take the mode.
   */
  async setOperationMode(slavePosition: number, mode: OperationMode): Promise<Cia402Status> {
    return (await this.api.setCia402OperationMode(slavePosition, { mode })).data;
  }

  /**
   * Writes the target of one kind, `'position'`, `'velocity'` or `'torque'` (`setCia402Target`).
   * The value is in the units of the object: 0x607A, 0x60FF or 0x6071.
   */
  async setTarget(slavePosition: number, kind: Cia402TargetKind, value: number): Promise<void> {
    await this.api.setCia402Target(slavePosition, { target: kind, value });
  }

  /**
   * Brings a drive to Operation Enabled, and returns its CiA402 status.
   *
   * The server walks the CiA402 state machine (`transitionToCia402State`), and it leaves a quick
   * stop. The device must exchange process data.
   */
  async enable(slavePosition: number): Promise<Cia402Status> {
    return this.toCia402State(slavePosition, 'OperationEnabled');
  }

  /** Brings a drive to Switch On Disabled, and returns its CiA402 status. */
  async disable(slavePosition: number): Promise<Cia402Status> {
    return this.toCia402State(slavePosition, 'SwitchOnDisabled');
  }

  /**
   * Sends a quick stop to a drive (`runCia402Command`), and returns its CiA402 status.
   *
   * It returns at once. The drive stops on its quick stop ramp, and 0x605A says the state it ends
   * in.
   */
  async quickStop(slavePosition: number): Promise<Cia402Status> {
    return (await this.api.runCia402Command(slavePosition, { command: 'quickStop' })).data;
  }

  private async toCia402State(
    slavePosition: number,
    state: Extract<Cia402State, 'OperationEnabled' | 'SwitchOnDisabled'>,
  ): Promise<Cia402Status> {
    return (await this.api.transitionToCia402State(slavePosition, { state })).data;
  }

  /**
   * Starts a procedure on a device and yields its snapshot at every poll until the run ends.
   *
   * The last snapshot yielded has the final status: succeeded, failed or cancelled. Polling is
   * lossless, because every finished step keeps its status and value, so a step that starts and
   * ends between two polls still shows. When the caller leaves the loop before the run ends, the
   * run is cancelled, so that the drive does not continue alone. `pollIntervalMs` is 500 when not
   * given. An abort of `signal` ends the loop with its reason, and cancels the run too.
   *
   *     for await (const [snapshot, changed] of stepChanges(client.runProcedure(1, 'offset-detection'))) { ... }
   *
   * Throws a `ProcedureError` when the server refuses the start: 409 when another procedure runs on
   * the device, 404 when the device has no procedure of that name, 400 for a parameter the
   * procedure refuses.
   */
  async *runProcedure(
    slavePosition: number,
    name: string,
    options: { params?: ProcedureRequest; pollIntervalMs?: number; signal?: AbortSignal } = {},
  ): AsyncGenerator<ProcedureSnapshot, void, undefined> {
    const { params = {}, pollIntervalMs = 500, signal } = options;
    let snapshot: ProcedureSnapshot;
    try {
      snapshot = (await this.api.startProcedure(slavePosition, name, params)).data;
    } catch (error) {
      if (!(error instanceof Response)) {
        throw error;
      }
      const body = (error as { error?: { error?: unknown } }).error;
      const reason = typeof body?.error === 'string' ? body.error : error.statusText;
      throw new ProcedureError(
        `${name} on device ${slavePosition}: ${reason} (HTTP ${error.status})`,
      );
    }
    try {
      for (;;) {
        yield snapshot;
        if (snapshot.status !== 'running') {
          return;
        }
        await sleep(pollIntervalMs, signal);
        snapshot = (await this.api.getProcedure(slavePosition, name)).data;
      }
    } finally {
      if (snapshot.status === 'running') {
        log('cancelling %s on device %d', name, slavePosition);
        // A failed cancel has nothing to add to the error or the early exit that brought the run here.
        await this.api.cancelProcedure(slavePosition, name).catch(() => {});
      }
    }
  }

  /**
   * Reads the recorder clock of the server (`GET /api/process-data/mark`), in epoch microseconds.
   * Pass it as `from` or `to` of `fetchProcessDataDump` to dump one part of a run.
   */
  async readProcessDataMark(): Promise<number> {
    const response = await this.api.getProcessDataMark();
    return response.data.timestampUs;
  }

  /**
   * Fetches and parses the live recorder dump (`GET /api/process-data/dump`) as an `MmpdFile`.
   * `from` and `to` limit it to a span, from `readProcessDataMark`. Without them it is the whole
   * recorder.
   */
  fetchProcessDataDump(
    options: { signal?: AbortSignal; from?: number; to?: number } = {},
  ): Promise<MmpdFile> {
    return fetchProcessDataDump(this.httpBaseUrl, { fetch: this.customFetch, ...options });
  }

  /** Closes the WebSocket connection (and cancels reconnection). The HTTP API needs no teardown. */
  close(): void {
    this.ws.close();
  }
}
