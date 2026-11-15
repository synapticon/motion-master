// The MotionMasterClient facade: a single connection object that composes the generated HTTP API
// (`client.api.*`, flat — e.g. `client.api.getVersion()`) with the WebSocket connection
// (`client.ws`). It is a *connection, not a model*: it holds no mutable device/parameter/AL state
// — the HTTP API and the monitoring stream are the source of truth. It runs in a browser and in
// Node.js 22 or later with the global `fetch` and `WebSocket` of either.

import createDebug from 'debug';
import { AlState, alStateName } from './al-state';
import type { Cia402State, OperationMode } from './cia402';
import { readConfig, type FieldbusSettings } from './config';
import type { EniExport, EniParseResult } from './eni';
import {
  CaptureError,
  ConnectionError,
  HttpError,
  MonitoringError,
  ProcedureError,
  ProcessDataDumpError,
  StateError,
} from './errors';
import { Api } from './generated/Api';
import type { Cia402Status, ProcedureRequest, ProcedureSnapshot } from './generated/data-contracts';
import type { ApiConfig } from './generated/http-client';
import { API_BASE_URL, WS_URL } from './constants';
import { MonitoringRecording } from './monitoring-recording';
import type { ObjectAddress } from './objects';
import { type DeviceParameterAddress, parameterTriple } from './parameter-address';
import { listen } from './stream';
import {
  type MonitoringBatch,
  type NotificationData,
  WebSocketConnection,
  type WebSocketConstructor,
} from './web-socket-connection';

export interface MotionMasterClientOptions {
  /** Origin of the HTTP API, such as `https://127.0.0.1:61447`. Default: the bundled localhost dev origin. */
  baseUrl?: string;
  /** URL of the WebSocket, such as `wss://127.0.0.1:62281`. Default: the bundled localhost dev origin. */
  wsUrl?: string;
  /**
   * The WebSocket class for `client.ws`. Default: the global `WebSocket` of the browser or of
   * Node.js. Pass the class of the `ws` package to give the WebSocket its own TLS options, through
   * `webSocketOptions`.
   */
  WebSocket?: WebSocketConstructor;
  /**
   * The second argument of the WebSocket constructor, such as `{ rejectUnauthorized: false }` for
   * the `ws` package. Set it only with `WebSocket` from `ws`: the global `WebSocket` takes no TLS
   * option, and a browser refuses an object here.
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

// How long monitoring() waits for the first row. After the span it waits two flush intervals for the
// last rows, and this margin on top.
const FIRST_ROW_TIMEOUT_MS = 5000;
const LAST_FLUSH_MARGIN_MS = 2000;

/** Returns a topic that no other monitoring has, such as `ts-1f3a9c0b7e2d`. */
function uniqueTopic(): string {
  return `ts-${globalThis.crypto.randomUUID().replaceAll('-', '').slice(0, 12)}`;
}

/**
 * Throws when `url` has none of `protocols`, such as `localhost:61447`, which parses as a URL of the
 * scheme `localhost`. `fetch` and `WebSocket` find that only at the first request, and report it as
 * a failed connection. A URL that does not parse at all passes, so that the Console, which builds
 * its client from a host the user typed, keeps running and shows the failed requests.
 */
function checkUrl(option: string, url: string, protocols: readonly string[]): void {
  let protocol: string;
  try {
    protocol = new URL(url).protocol;
  } catch {
    return;
  }
  if (!protocols.includes(protocol)) {
    const schemes = protocols.map((p) => p.slice(0, -1)).join(' or ');
    throw new Error(`${option} must be a URL that starts with ${schemes}: "${url}"`);
  }
}

/**
 * Returns the request of a failed fetch, for its error: the URL, the method and the headers. The
 * body is left out, because a stream cannot be read twice.
 */
function requestOf(input: RequestInfo | URL, init?: RequestInit): Request {
  if (input instanceof Request) {
    return new Request(input.url, { method: input.method, headers: input.headers });
  }
  return new Request(input, { method: init?.method, headers: init?.headers });
}

/**
 * Returns a `fetch` that throws a `ConnectionError` when a request does not reach the server and an
 * `HttpError` when the server refuses it. Every request of the client goes through it, `client.api`
 * included. The global `fetch` stays as it is.
 *
 * @param customFetch The `fetch` to wrap. Without it, the global `fetch` is read at each request, so
 *   a tool that replaces it later, such as MSW in the tests, still gets the requests.
 */
function throwingFetch(customFetch?: typeof fetch): typeof fetch {
  return async (input, init) => {
    const doFetch = customFetch ?? globalThis.fetch;
    let response: Response;
    try {
      response = await doFetch(input, init);
    } catch (error) {
      // fetch rejects with a TypeError when the request does not reach the server. An abort rejects
      // with an AbortError, which stays as it is.
      if (error instanceof TypeError) {
        const request = requestOf(input, init);
        const origin = new URL(request.url).origin;
        throw new ConnectionError(`cannot connect to ${origin}`, request, { cause: error });
      }
      throw error;
    }
    if (!response.ok) {
      throw await HttpError.from(requestOf(input, init), response);
    }
    return response;
  };
}

export class MotionMasterClient {
  /**
   * The generated HTTP API client. Flat method surface: `client.api.getVersion()`,
   * `client.api.scan()`, `client.api.sdoUpload(...)`, ...
   */
  readonly api: Api;

  /**
   * The WebSocket connection: `client.ws.subscribe(topic, cb)`,
   * `client.ws.onNotification(cb)`. Connects lazily.
   */
  readonly ws: WebSocketConnection;

  /** The body of `POST /api/init`, from the options or from config.jsonc. */
  readonly fieldbus?: FieldbusSettings;

  private readonly httpBaseUrl: string;
  /** The fetch of this client. See `throwingFetch`. */
  private readonly fetch: typeof fetch;

  constructor(options: MotionMasterClientOptions = {}) {
    this.httpBaseUrl = options.baseUrl ?? API_BASE_URL;
    const wsUrl = options.wsUrl ?? WS_URL;
    checkUrl('baseUrl', this.httpBaseUrl, ['http:', 'https:']);
    checkUrl('wsUrl', wsUrl, ['ws:', 'wss:']);
    this.fieldbus = options.fieldbus;
    this.fetch = throwingFetch(options.customFetch);
    this.api = new Api({ baseUrl: this.httpBaseUrl, customFetch: this.fetch });
    this.ws = new WebSocketConnection({
      url: wsUrl,
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
      if (!(error instanceof HttpError && error.status === 409)) {
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
   *
   * An abort of `signal` ends the loop with its reason. `close()` ends the loop, and any other close
   * of the WebSocket throws a `ConnectionError`, because the batches sent meanwhile are lost.
   */
  streamMonitoring(
    topic: string,
    options: { signal?: AbortSignal } = {},
  ): AsyncGenerator<MonitoringBatch, void, undefined> {
    return listen(
      (listener: (rows: MonitoringBatch) => void) => this.ws.subscribe(topic, listener),
      { signal: options.signal, onClose: (listener) => this.ws.onClose(listener) },
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
    // An aborted signal starts no run. The start itself takes no signal: an abort while it is on
    // the way could leave a run going that this loop never cancels.
    signal?.throwIfAborted();
    let snapshot: ProcedureSnapshot;
    try {
      snapshot = (await this.api.startProcedure(slavePosition, name, params)).data;
    } catch (error) {
      if (!(error instanceof HttpError)) {
        throw error;
      }
      throw new ProcedureError(
        `${name} on device ${slavePosition}: ${error.reason} (HTTP ${error.status})`,
        { cause: error },
      );
    }
    try {
      for (;;) {
        yield snapshot;
        if (snapshot.status !== 'running') {
          return;
        }
        await sleep(pollIntervalMs, signal);
        snapshot = (await this.api.getProcedure(slavePosition, name, { signal })).data;
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
   * Subscribes to the notifications topic and yields the data of each notification.
   *
   * Every notification source publishes to the one `notifications` topic, so one subscription
   * receives every event, also a kind that a later server adds. `data.event` names the kind. Leaving
   * the `for await` loop unsubscribes.
   *
   * An abort of `signal` ends the loop with its reason. `close()` ends the loop, and any other close
   * of the WebSocket throws a `ConnectionError`, because the notifications sent meanwhile are lost.
   */
  notifications(
    options: { signal?: AbortSignal } = {},
  ): AsyncGenerator<NotificationData, void, undefined> {
    return listen(
      (listener: (data: NotificationData) => void) => this.ws.onNotification(listener),
      { signal: options.signal, onClose: (listener) => this.ws.onClose(listener) },
    );
  }

  /**
   * Records the rows of the parameters while `span` runs, and returns the recording.
   *
   * `parameters` is a list of `[devicePosition, index, subindex]` or `[devicePosition, address]`.
   * Create it after the devices are in SAFEOP or OP, because the server decides once, at creation,
   * whether each object comes from the process data or from SDO reads. Without `topic`, a unique one
   * is chosen. `intervalMs` is the flush cadence, 50 when not given.
   *
   * It creates the monitoring and waits for the first row, so nothing `span` does is missed. After
   * `span`, it waits for the rows of the last flush and deletes the monitoring, also when `span`
   * throws. The rows stay readable in the recording.
   *
   *     const m = await client.monitoring([[1, Cia402.VELOCITY_ACTUAL_VALUE]], async () => {
   *       // the move
   *     });
   *     const velocity = m.column(1, Cia402.VELOCITY_ACTUAL_VALUE);
   *
   * Throws a `MonitoringError` when the server refuses the monitoring, when no row arrives within
   * 5000 ms, when the rows of the last flush do not arrive, and when the WebSocket closes during the
   * recording. An error of `span` wins over them.
   *
   * An abort of `signal` stops the wait for the first row and for the last flush, deletes the
   * monitoring, and throws its reason. `span` gets no signal from here: pass the same signal to what
   * `span` waits for, so that it ends too.
   */
  async monitoring(
    parameters: readonly DeviceParameterAddress[],
    span: () => unknown,
    options: { intervalMs?: number; topic?: string; name?: string; signal?: AbortSignal } = {},
  ): Promise<MonitoringRecording> {
    const { intervalMs = 50, topic = uniqueTopic(), name, signal } = options;
    signal?.throwIfAborted();
    // Throws when there is no WebSocket class, before the monitoring exists on the server.
    this.ws.assertAvailable();
    const body = {
      topic,
      interval: intervalMs,
      parameters: parameters.map(parameterTriple),
      ...(name === undefined ? {} : { name }),
    };
    let created;
    try {
      created = (await this.api.createMonitoring(body)).data;
    } catch (error) {
      if (!(error instanceof HttpError)) {
        throw error;
      }
      throw new MonitoringError(
        `cannot create monitoring ${topic}: ${error.reason} (HTTP ${error.status})`,
        { cause: error },
      );
    }
    const recording = new MonitoringRecording(topic, created.parameters);

    // The WebSocket listener counts the batches and wakes the one wait that runs at a time. A close
    // of the WebSocket and an abort of the signal wake it too.
    let batches = 0;
    let socketError: MonitoringError | null = null;
    let wake = () => {};
    const ended = () => socketError !== null || signal?.aborted === true;
    const waitFor = (condition: () => boolean, timeoutMs: number) =>
      new Promise<boolean>((resolve) => {
        const timer = setTimeout(() => resolve(condition()), timeoutMs);
        wake = () => {
          if (condition() || ended()) {
            clearTimeout(timer);
            resolve(condition());
          }
        };
        wake();
      });
    const onAbort = () => wake();
    signal?.addEventListener('abort', onAbort);
    let unsubscribe = () => {};
    let stopWatching = () => {};
    const stop = async () => {
      signal?.removeEventListener('abort', onAbort);
      // Stop watching first: the last unsubscribe can close the socket, which is no failure.
      stopWatching();
      unsubscribe();
      await this.api.deleteMonitoring(topic).catch(() => {});
    };

    // From here on the monitoring exists, so every failure deletes it.
    try {
      // Watch before subscribing, because subscribing opens the socket, which can fail at once.
      stopWatching = this.ws.onStateChange((state) => {
        if (state === 'closed') {
          socketError = new MonitoringError(
            `monitoring ${topic}: the WebSocket closed during the recording`,
          );
          wake();
        }
      });
      unsubscribe = this.ws.subscribe(topic, (rows) => {
        recording.rows.push(...rows);
        batches += 1;
        wake();
      });
      const arrived = await waitFor(() => recording.rows.length > 0, FIRST_ROW_TIMEOUT_MS);
      signal?.throwIfAborted();
      if (socketError !== null) {
        throw socketError;
      }
      if (!arrived) {
        throw new MonitoringError(`monitoring ${topic}: no row after ${FIRST_ROW_TIMEOUT_MS} ms`);
      }
    } catch (error) {
      await stop();
      throw error;
    }

    let flushed: boolean;
    const flushTimeoutMs = 2 * intervalMs + LAST_FLUSH_MARGIN_MS;
    try {
      await span();
    } finally {
      // A row is in a batch only after the next flush, so wait for two more batches.
      const target = batches + 2;
      flushed = await waitFor(() => batches >= target, flushTimeoutMs);
      await stop();
    }
    signal?.throwIfAborted();
    if (socketError !== null) {
      throw socketError;
    }
    if (!flushed) {
      throw new MonitoringError(
        `monitoring ${topic}: the rows of the last flush did not arrive within ${flushTimeoutMs} ms`,
      );
    }
    return recording;
  }

  /**
   * Starts a capture of the EtherCAT frames that runs until `stopCapture()` (`startCapture`).
   *
   * Returns once the capture records, with the path of its pcap file in the user cache. The capture
   * runs as a program of its own, so a reset, an init or a scan in between does not end it.
   * `adapter` defaults to the adapter of the bus, so name it when no bus is up. `seconds` is the
   * upper limit for a capture that is never stopped, 600 when not given.
   *
   * Throws a `CaptureError` with the server's reason when it refuses, for example while another
   * capture runs (409) or on Windows (501).
   */
  async startCapture(options: { adapter?: string; seconds?: number } = {}): Promise<string> {
    const { adapter, seconds } = options;
    const body = {
      ...(adapter ? { adapter } : {}),
      ...(seconds === undefined ? {} : { seconds }),
    };
    try {
      const started = (await this.api.startCapture(body)).data;
      log('capturing on %s into %s', started.adapter, started.path);
      return started.path;
    } catch (error) {
      if (!(error instanceof HttpError)) {
        throw error;
      }
      throw new CaptureError(`the capture did not start: ${error.reason} (HTTP ${error.status})`, {
        cause: error,
      });
    }
  }

  /**
   * Stops the started capture and returns its path and summary (`stopCapture`).
   *
   * The result is the body of `POST /api/captures/stop`: `path`, the pcap file in the user cache,
   * and `summary`. Throws a `CaptureError` with the server's reason when no capture was started or
   * the capture failed.
   */
  async stopCapture(): Promise<Data<'stopCapture'>> {
    try {
      const stopped = (await this.api.stopCapture()).data;
      log('stopped the capture into %s', stopped.path);
      return stopped;
    } catch (error) {
      if (!(error instanceof HttpError)) {
        throw error;
      }
      throw new CaptureError(
        `the capture did not stop cleanly: ${error.reason} (HTTP ${error.status})`,
        { cause: error },
      );
    }
  }

  /**
   * Returns the recorder clock of the server now, in epoch microseconds (`getProcessDataMark`).
   *
   * Pass it as `fromUs` or `toUs` of `processDataDump()` to dump one part of a run. It is the
   * server's clock, the one that stamps every recorded cycle, so it matches the cycles also when this
   * client runs on another computer.
   */
  async processDataMark(): Promise<number> {
    return (await this.api.getProcessDataMark()).data.timestampUs;
  }

  /**
   * Returns the .mmpd dump of the recorder ring as bytes (`streamProcessDataDump`).
   *
   * `fromUs` and `toUs` limit it to the cycles of a span, both included, in epoch microseconds of the
   * server clock: take them from `processDataMark()`. Without them the dump is the whole ring.
   * `parseMmpd()` decodes the bytes. When the server no longer held every cycle right after
   * `fromUs`, the flags of the header have `MMPD_FLAG_INCOMPLETE_START` set.
   *
   * Returns `null` when the server has no cycle to dump: no process image was ever mapped, or no
   * cycle is in the span (409). Throws a `ProcessDataDumpError` for every other refusal.
   */
  async processDataDump(
    options: { fromUs?: number; toUs?: number; signal?: AbortSignal } = {},
  ): Promise<Uint8Array | null> {
    const { fromUs, toUs, signal } = options;
    let response: Response;
    try {
      // No response format: the generated client returns the answer with its body unread.
      response = await this.api.streamProcessDataDump(
        { from: fromUs, to: toUs },
        { headers: { Accept: 'application/octet-stream' }, signal },
      );
    } catch (error) {
      if (!(error instanceof HttpError)) {
        throw error;
      }
      if (error.status === 409) {
        log('no process-data dump: %s', error.reason);
        return null;
      }
      throw new ProcessDataDumpError(
        `the process-data dump failed: ${error.reason} (HTTP ${error.status})`,
        { cause: error },
      );
    }
    return new Uint8Array(await response.arrayBuffer());
  }

  /**
   * Exports the bus as an ENI document (`exportEni`), with the number of parts the server could not
   * read.
   *
   * A bus that has not reached SAFEOP or OP answers 409, because before that there is no mapping to
   * describe. A refused request throws an HttpError.
   */
  async exportEni(options: { signal?: AbortSignal } = {}): Promise<EniExport> {
    // No response format: the generated client returns the answer with its body unread.
    const response = await this.api.exportEni({
      headers: { Accept: 'application/xml' },
      signal: options.signal,
    });
    const warnings = Number.parseInt(response.headers.get('X-Eni-Warnings') ?? '0', 10);
    return { xml: await response.text(), warnings: Number.isFinite(warnings) ? warnings : 0 };
  }

  /**
   * Reads an ENI document (`parseEni`) and returns its network, with each datagram annotated. Any
   * ENI that follows ETG.2100 is read, not only one that `exportEni()` wrote. A refused request
   * throws an HttpError.
   */
  async parseEni(xml: string, options: { signal?: AbortSignal } = {}): Promise<EniParseResult> {
    // The generated api.parseEni sends a body it has no formatter for as JSON, so the document would
    // arrive as a quoted JSON string. Its answer types `network` as a bare object; EniParseResult
    // has the full shape.
    const response = await this.fetch(`${this.httpBaseUrl}/api/eni/parse`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/xml' },
      body: xml,
      signal: options.signal,
    });
    return (await response.json()) as EniParseResult;
  }

  /** Closes the WebSocket connection (and cancels reconnection). The HTTP API needs no teardown. */
  close(): void {
    this.ws.close();
  }
}
