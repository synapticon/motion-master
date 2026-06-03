/* eslint-disable */
/* tslint:disable */
// @ts-nocheck
/*
 * ---------------------------------------------------------------
 * ## THIS FILE WAS GENERATED VIA SWAGGER-TYPESCRIPT-API        ##
 * ##                                                           ##
 * ## AUTHOR: acacode                                           ##
 * ## SOURCE: https://github.com/acacode/swagger-typescript-api ##
 * ---------------------------------------------------------------
 */

/** A device's process-data (sync-manager) watchdog configuration, decoded from the watchdog divider (0x0400) and process-data watchdog time (0x0420) ESC registers. */
export interface ProcessDataWatchdog {
  /**
   * 1-based position of the device on the fieldbus.
   * @example 1
   */
  slavePosition: number;
  /**
   * False when the time register (ticks) is 0, i.e. the watchdog is disabled.
   * @example true
   */
  enabled: boolean;
  /**
   * Configured timeout in nanoseconds: ticks × 40 ns × (divider + 2).
   * @example 100000000
   */
  timeoutNs: number;
  /**
   * The same timeout in milliseconds (convenience for display/editing).
   * @example 100
   */
  timeoutMs: number;
  /**
   * Raw watchdog divider register (0x0400); shared with the PDI watchdog.
   * @example 2498
   */
  divider: number;
  /**
   * Raw process-data watchdog time register (0x0420), in watchdog ticks.
   * @example 1000
   */
  ticks: number;
}

/** A monitoring's configuration plus its current runtime status. */
export interface Monitoring {
  /** @example "left-leg" */
  topic: string;
  /**
   * Present only when a label was supplied at creation.
   * @example "Left Leg"
   */
  name?: string;
  /**
   * Sampling period in milliseconds.
   * @example 1000
   */
  interval: number;
  /**
   * Samples per published batch.
   * @example 16
   */
  bufferSize: number;
  /**
   * Rows currently accumulated toward the next batch.
   * @example 7
   */
  bufferFill: number;
  /** The sampled objects, in the positional order of each WebSocket sample row. */
  parameters: {
    /** @example 1 */
    devicePosition: number;
    /** @example 24676 */
    index: number;
    /** @example 0 */
    subindex: number;
    /**
     * How the value is sourced — `pdo` (decoded from the live process image) or `sdo` (polled in the background and read from cache).
     * @example "pdo"
     */
    source: "pdo" | "sdo";
  }[];
}

export interface ProcessImageObject {
  /**
   * 1-based bus position of the owning device
   * @example 1
   */
  slavePosition: number;
  /**
   * CoE object index
   * @example 24640
   */
  index: number;
  /**
   * CoE object subindex
   * @example 0
   */
  subindex: number;
  /**
   * Object name resolved from the device's parameter map; empty when the object dictionary has not been enumerated for that device
   * @example "Target position"
   */
  name: string;
  /**
   * Absolute bit offset within the direction's image
   * @example 32
   */
  bitOffset: number;
  /**
   * Width of the value in bits
   * @example 32
   */
  bitLength: number;
}

export interface SyncManagerConfig {
  /**
   * Sync Manager number (0–7)
   * @example 2
   */
  index: number;
  /**
   * Physical ESC memory start address the SM guards
   * @example 4352
   */
  physicalStart: number;
  /**
   * Length of the guarded window in bytes
   * @example 12
   */
  length: number;
  /**
   * Raw SM control/flags register (buffer mode, direction, watchdog, enable) — decoded by the client
   * @example 100100
   */
  flags: number;
  /**
   * 0 unused, 1 MbxOut, 2 MbxIn, 3 Outputs, 4 Inputs
   * @example 3
   */
  type: number;
}

export interface FmmuConfig {
  /**
   * FMMU number (0–3)
   * @example 0
   */
  index: number;
  /**
   * Logical (bus-wide) start address
   * @example 0
   */
  logicalStart: number;
  /**
   * Mapped length in bytes
   * @example 12
   */
  length: number;
  /**
   * Start bit within the first logical byte
   * @example 0
   */
  logicalStartBit: number;
  /**
   * End bit within the last logical byte
   * @example 7
   */
  logicalEndBit: number;
  /**
   * Physical ESC start address (ties the FMMU to a Sync Manager)
   * @example 4352
   */
  physicalStart: number;
  /**
   * Start bit within the first physical byte
   * @example 0
   */
  physicalStartBit: number;
  /**
   * ESC FMMU type: 1 read (inputs/TxPDO), 2 write (outputs/RxPDO)
   * @example 2
   */
  type: number;
  /**
   * Whether the FMMU is active
   * @example true
   */
  active: boolean;
}

export interface SlaveConfig {
  /**
   * 1-based bus position
   * @example 1
   */
  slavePosition: number;
  /**
   * Device name for this slave position, empty if unknown
   * @example "SOMANET Node"
   */
  deviceName: string;
  /**
   * Station (configured) address assigned during scan
   * @example 4097
   */
  configuredAddress: number;
  /**
   * Configured station alias from EEPROM
   * @example 0
   */
  aliasAddress: number;
  /**
   * Mapped output (master→slave) bits
   * @example 96
   */
  outputBits: number;
  /**
   * Mapped input (slave→master) bits
   * @example 128
   */
  inputBits: number;
  mailbox: {
    /**
     * Write (master→slave) mailbox length in bytes; 0 if none
     * @example 128
     */
    writeLength: number;
    /**
     * Write mailbox physical ESC offset
     * @example 4096
     */
    writeOffset: number;
    /**
     * Read (slave→master) mailbox length in bytes
     * @example 128
     */
    readLength: number;
    /**
     * Read mailbox physical ESC offset
     * @example 4224
     */
    readOffset: number;
    /**
     * Supported-protocol bits (0x01 AoE, 0x02 EoE, 0x04 CoE, 0x08 FoE, 0x10 SoE, 0x20 VoE) — decoded by the client
     * @example 14
     */
    protocols: number;
  };
  dc: {
    /**
     * Slave has distributed-clock hardware
     * @example true
     */
    capable: boolean;
    /**
     * SYNC0 generation enabled (false for SM-synchronous bring-up)
     * @example false
     */
    active: boolean;
    /**
     * Measured propagation delay in nanoseconds
     * @example 300
     */
    propagationDelay: number;
    /**
     * DC cycle time in nanoseconds
     * @example 0
     */
    cycleTime: number;
    /**
     * Shift from the cycle-modulus boundary in nanoseconds
     * @example 0
     */
    shift: number;
  };
  /** Configured Sync Managers, by index */
  syncManagers: SyncManagerConfig[];
  /** Configured FMMUs, by index */
  fmmus: FmmuConfig[];
}

export interface PortDiagnostics {
  /**
   * Physical link detected on this port (DL Status link bit)
   * @example true
   */
  linkUp: boolean;
  /**
   * Loop closed on this port (no downstream slave, or port disabled)
   * @example false
   */
  loopClosed: boolean;
  /**
   * Stable communication established on this port
   * @example true
   */
  communication: boolean;
  /**
   * Invalid-frame counter (0x0300+): frames with a bad FCS/structure. 8-bit, saturates at 255; watch for a rising delta rather than the absolute value
   * @example 0
   */
  invalidFrame: number;
  /**
   * Physical-layer RX error counter (0x0301+): RX_ER asserted by the PHY
   * @example 0
   */
  rxError: number;
  /**
   * Forwarded RX error counter (0x0308+): errors first flagged by an upstream ESC — pinpoints the segment where corruption began
   * @example 0
   */
  forwardedError: number;
  /**
   * Lost-link counter (0x0310+): link-down events on this port
   * @example 0
   */
  lostLink: number;
}

export interface DeviceDiagnostics {
  /**
   * 1-based position on the fieldbus
   * @example 1
   */
  slavePosition: number;
  /**
   * Device name for this slave position, empty if unknown
   * @example "SOMANET Node"
   */
  deviceName: string;
  /** Per-port link state and error counters (ports 0–3, in order) */
  ports: PortDiagnostics[];
  /**
   * ECAT processing-unit error counter (0x030C): datagrams reaching the unit malformed
   * @example 0
   */
  processingUnitError: number;
  /**
   * PDI error counter (0x030D): problems on the slave-local process-data interface
   * @example 0
   */
  pdiError: number;
  /**
   * Process-data (SM) watchdog expirations (0x0442): the slave stopped seeing fresh outputs
   * @example 0
   */
  processDataWatchdog: number;
  /**
   * PDI watchdog expirations (0x0443)
   * @example 0
   */
  pdiWatchdog: number;
}

export interface DcSyncStatus {
  /**
   * 1-based position on the fieldbus
   * @example 1
   */
  slavePosition: number;
  /**
   * Device name for this slave position, empty if unknown
   * @example "SOMANET Node"
   */
  deviceName: string;
  /**
   * Slave has distributed-clock hardware
   * @example true
   */
  dcCapable: boolean;
  /**
   * This slave is the DC reference clock (the first DC-capable slave) — it defines bus time, so its own systemTimeDifference is zero
   * @example false
   */
  referenceClock: boolean;
  /**
   * System-time delay / propagation delay (0x0928), nanoseconds
   * @example 300
   */
  propagationDelay: number;
  /**
   * Signed deviation of the local system time from the reference clock (0x092C), in nanoseconds. Positive = local clock ahead of the reference, negative = behind; zero on the reference clock. Meaningful only while exchanging in SAFE-OP/OP; converges toward zero as the drift-compensation loop settles
   * @example 0
   */
  systemTimeDifference: number;
}

export interface AlStatusCode {
  /**
   * AL Status Code value (ETG.1000.6 §6.4.1)
   * @example 20
   */
  code: number;
  /**
   * Short human-readable name
   * @example "No valid firmware"
   */
  name: string;
  /**
   * Full description of the error condition
   * @example "No valid firmware is present — the slave needs firmware flashed"
   */
  description: string;
  /**
   * True if a slave reporting this code cannot reach the requested EtherCAT state by retrying. The server abandons the slave immediately during a state transition instead of waiting for timeout; the master must change something (re-init, reflash, power cycle) before another transition attempt can succeed.
   * @example true
   */
  terminal: boolean;
}

export interface EscRegister {
  /**
   * Register address in the ESC address space (decimal)
   * @example 272
   */
  address: number;
  /**
   * Register width in bytes (1, 2, 4, or 8)
   * @example 2
   */
  length: number;
  /**
   * Short snake_case identifier
   * @example "dl_status"
   */
  name: string;
  /**
   * Human-readable description from the Beckhoff ESC datasheet
   * @example "DL status: EEPROM load ok, link detected, communication established per port"
   */
  description: string;
}

export interface FoeErrorCode {
  /**
   * 32-bit FoE error code as sent in the FoE ERROR packet (ETG.1000.6 §5.5)
   * @example 32770
   */
  code: number;
  /**
   * Short human-readable name
   * @example "File not found"
   */
  name: string;
  /**
   * Full description of the error condition
   * @example "The requested file does not exist on the slave"
   */
  description: string;
}

export interface ObjectDataTypeInfo {
  /**
   * ETG.1020 data type code
   * @example 7
   */
  code: number;
  /**
   * Symbolic name of the data type
   * @example "UNSIGNED32"
   */
  name: string;
  /**
   * Declared bit width of one element; 0 for variable-length types
   * @example 32
   */
  bitSize: number;
}

export interface DeviceParameter {
  /**
   * CoE object index
   * @example 24640
   */
  index: number;
  /**
   * CoE object subindex
   * @example 0
   */
  subindex: number;
  /**
   * Textual description from the slave's SDO Info "Get Entry Description"
   * @example "Position actual value"
   */
  name: string;
  /**
   * ETG.1000.6 §5 object code (VAR=7, ARRAY=8, RECORD=9)
   * @example 7
   */
  objectCode: number;
  /**
   * ETG.1020 data type code (e.g. 7 = UNSIGNED32)
   * @example 4
   */
  dataType: number;
  /**
   * Symbolic name of the data type (resolved server-side from `dataType`)
   * @example "INTEGER32"
   */
  dataTypeName: string;
  /**
   * Bit length of the entry's value
   * @example 32
   */
  bitLength: number;
  /**
   * ObjAccess bitfield (read/write per AL state)
   * @example 7
   */
  access: number;
  /**
   * Last-known value, decoded according to `dataType`.  Initialised to a type-appropriate zero (0 for numbers, "" for strings, [] for raw bytes) before the first read.  After a successful SDO upload, holds the decoded value; the JSON encoding follows the variant alternative — number, string, or array of byte values.
   * @example 12345
   */
  value: number | string | number[];
  /**
   * ETG.1004 unit code reported by the slave for this entry.  Absent when the slave does not populate the Unit field of the SDO Info response.  The 32-bit code decomposes into prefix, base SI unit, and exponent per ETG.1004.
   * @example 0
   */
  unit?: number;
  /** Slave-reported default value, decoded with the same logic as `value`.  Absent when the slave does not report a default. */
  defaultValue?: number | string | number[];
  /** Slave-reported minimum value, decoded with the same logic as `value`.  Absent when the slave does not report a minimum. */
  minValue?: number | string | number[];
  /** Slave-reported maximum value, decoded with the same logic as `value`.  Absent when the slave does not report a maximum. */
  maxValue?: number | string | number[];
  /**
   * Freshness of `value` relative to the device. `synced` — matches the device (last successful read or write); `pending` — set locally while offline or after a failed write, not yet confirmed on the device; `unknown` — never read, `value` is the type-appropriate default.
   * @example "synced"
   */
  syncState: "unknown" | "synced" | "pending";
}

export type QueryParamsType = Record<string | number, any>;
export type ResponseFormat = keyof Omit<Body, "body" | "bodyUsed">;

export interface FullRequestParams extends Omit<RequestInit, "body"> {
  /** set parameter to `true` for call `securityWorker` for this request */
  secure?: boolean;
  /** request path */
  path: string;
  /** content type of request body */
  type?: ContentType;
  /** query params */
  query?: QueryParamsType;
  /** format of response (i.e. response.json() -> format: "json") */
  format?: ResponseFormat;
  /** request body */
  body?: unknown;
  /** base url */
  baseUrl?: string;
  /** request cancellation token */
  cancelToken?: CancelToken;
}

export type RequestParams = Omit<
  FullRequestParams,
  "body" | "method" | "query" | "path"
>;

export interface ApiConfig<SecurityDataType = unknown> {
  baseUrl?: string;
  baseApiParams?: Omit<RequestParams, "baseUrl" | "cancelToken" | "signal">;
  securityWorker?: (
    securityData: SecurityDataType | null,
  ) => Promise<RequestParams | void> | RequestParams | void;
  customFetch?: typeof fetch;
}

export interface HttpResponse<D extends unknown, E extends unknown = unknown>
  extends Response {
  data: D;
  error: E;
}

type CancelToken = Symbol | string | number;

export enum ContentType {
  Json = "application/json",
  JsonApi = "application/vnd.api+json",
  FormData = "multipart/form-data",
  UrlEncoded = "application/x-www-form-urlencoded",
  Text = "text/plain",
}

export class HttpClient<SecurityDataType = unknown> {
  public baseUrl: string = "https://local.motion-master.synapticon.com:8443";
  private securityData: SecurityDataType | null = null;
  private securityWorker?: ApiConfig<SecurityDataType>["securityWorker"];
  private abortControllers = new Map<CancelToken, AbortController>();
  private customFetch = (...fetchParams: Parameters<typeof fetch>) =>
    fetch(...fetchParams);

  private baseApiParams: RequestParams = {
    credentials: "same-origin",
    headers: {},
    redirect: "follow",
    referrerPolicy: "no-referrer",
  };

  constructor(apiConfig: ApiConfig<SecurityDataType> = {}) {
    Object.assign(this, apiConfig);
  }

  public setSecurityData = (data: SecurityDataType | null) => {
    this.securityData = data;
  };

  protected encodeQueryParam(key: string, value: any) {
    const encodedKey = encodeURIComponent(key);
    return `${encodedKey}=${encodeURIComponent(typeof value === "number" ? value : `${value}`)}`;
  }

  protected addQueryParam(query: QueryParamsType, key: string) {
    return this.encodeQueryParam(key, query[key]);
  }

  protected addArrayQueryParam(query: QueryParamsType, key: string) {
    const value = query[key];
    return value.map((v: any) => this.encodeQueryParam(key, v)).join("&");
  }

  protected toQueryString(rawQuery?: QueryParamsType): string {
    const query = rawQuery || {};
    const keys = Object.keys(query).filter(
      (key) => "undefined" !== typeof query[key],
    );
    return keys
      .map((key) =>
        Array.isArray(query[key])
          ? this.addArrayQueryParam(query, key)
          : this.addQueryParam(query, key),
      )
      .join("&");
  }

  protected addQueryParams(rawQuery?: QueryParamsType): string {
    const queryString = this.toQueryString(rawQuery);
    return queryString ? `?${queryString}` : "";
  }

  private contentFormatters: Record<ContentType, (input: any) => any> = {
    [ContentType.Json]: (input: any) =>
      input !== null && (typeof input === "object" || typeof input === "string")
        ? JSON.stringify(input)
        : input,
    [ContentType.JsonApi]: (input: any) =>
      input !== null && (typeof input === "object" || typeof input === "string")
        ? JSON.stringify(input)
        : input,
    [ContentType.Text]: (input: any) =>
      input !== null && typeof input !== "string"
        ? JSON.stringify(input)
        : input,
    [ContentType.FormData]: (input: any) => {
      if (input instanceof FormData) {
        return input;
      }

      return Object.keys(input || {}).reduce((formData, key) => {
        const property = input[key];
        formData.append(
          key,
          property instanceof Blob
            ? property
            : typeof property === "object" && property !== null
              ? JSON.stringify(property)
              : `${property}`,
        );
        return formData;
      }, new FormData());
    },
    [ContentType.UrlEncoded]: (input: any) => this.toQueryString(input),
  };

  protected mergeRequestParams(
    params1: RequestParams,
    params2?: RequestParams,
  ): RequestParams {
    return {
      ...this.baseApiParams,
      ...params1,
      ...(params2 || {}),
      headers: {
        ...(this.baseApiParams.headers || {}),
        ...(params1.headers || {}),
        ...((params2 && params2.headers) || {}),
      },
    };
  }

  protected createAbortSignal = (
    cancelToken: CancelToken,
  ): AbortSignal | undefined => {
    if (this.abortControllers.has(cancelToken)) {
      const abortController = this.abortControllers.get(cancelToken);
      if (abortController) {
        return abortController.signal;
      }
      return void 0;
    }

    const abortController = new AbortController();
    this.abortControllers.set(cancelToken, abortController);
    return abortController.signal;
  };

  public abortRequest = (cancelToken: CancelToken) => {
    const abortController = this.abortControllers.get(cancelToken);

    if (abortController) {
      abortController.abort();
      this.abortControllers.delete(cancelToken);
    }
  };

  public request = async <T = any, E = any>({
    body,
    secure,
    path,
    type,
    query,
    format,
    baseUrl,
    cancelToken,
    ...params
  }: FullRequestParams): Promise<HttpResponse<T, E>> => {
    const secureParams =
      ((typeof secure === "boolean" ? secure : this.baseApiParams.secure) &&
        this.securityWorker &&
        (await this.securityWorker(this.securityData))) ||
      {};
    const requestParams = this.mergeRequestParams(params, secureParams);
    const queryString = query && this.toQueryString(query);
    const payloadFormatter = this.contentFormatters[type || ContentType.Json];
    const responseFormat = format || requestParams.format;

    return this.customFetch(
      `${baseUrl || this.baseUrl || ""}${path}${queryString ? `?${queryString}` : ""}`,
      {
        ...requestParams,
        headers: {
          ...(requestParams.headers || {}),
          ...(type && type !== ContentType.FormData
            ? { "Content-Type": type }
            : {}),
        },
        signal:
          (cancelToken
            ? this.createAbortSignal(cancelToken)
            : requestParams.signal) || null,
        body:
          typeof body === "undefined" || body === null
            ? null
            : payloadFormatter(body),
      },
    ).then(async (response) => {
      const r = response as HttpResponse<T, E>;
      r.data = null as unknown as T;
      r.error = null as unknown as E;

      const responseToParse = responseFormat ? response.clone() : response;
      const data = !responseFormat
        ? r
        : await responseToParse[responseFormat]()
            .then((data) => {
              if (r.ok) {
                r.data = data;
              } else {
                r.error = data;
              }
              return r;
            })
            .catch((e) => {
              r.error = e;
              return r;
            });

      if (cancelToken) {
        this.abortControllers.delete(cancelToken);
      }

      if (!response.ok) throw data;
      return data;
    });
  };
}

/**
 * @title Motion Master API
 * @version 6.0.0-alpha.15
 * @baseUrl https://local.motion-master.synapticon.com:8443
 *
 * Motion Master is the motion-control software for Synapticon SOMANET servo
 * drives. This HTTP API drives the EtherCAT fieldbus end to end: bring up a
 * driver, discover the slaves on the bus, command AL state transitions, and
 * read or write the object dictionary, ESC registers, parameters, and files of
 * each device. A companion monitoring WebSocket streams live process data.
 *
 * ## Getting started
 *
 * The fieldbus is controlled through an explicit lifecycle — the driver is not
 * created until you ask for it, so a fresh instance has no open network
 * interface:
 *
 * 1. **Init** — `POST /api/init` constructs a fieldbus driver (`soem`, `spoe`,
 *    or `igh`) and opens the network interface. SOEM has no adapter
 *    auto-detect, so an adapter name or MAC address is required. `init` is
 *    one-shot: a second call returns `409` until you reset.
 * 2. **Scan** — `POST /api/scan` discovers the slaves on the bus and configures
 *    their sync managers and FMMUs. Slaves are left in the `INIT` state.
 * 3. **Transition** — `POST /api/devices/state` moves the bus to a target AL
 *    state (`PreOp`, `SafeOp`, `Op`, …); `GET /api/devices/state` reads where
 *    each slave currently sits.
 * 4. **Reset** — `POST /api/reset` tears the driver down and clears the device
 *    list. `init` must be called again afterwards.
 *
 * `GET /api/adapters` lists the network interfaces available on the host, to
 * help pick the adapter for step 1.
 *
 * ## Working with devices
 *
 * Once the bus has been scanned, each slave is addressed by its bus position
 * (`slavePosition`). The per-device endpoints cover the common service-plane
 * operations:
 *
 * - **Object dictionary (CoE)** — `GET …/sdo/{index}/{subindex}` uploads a
 *   single entry; `POST …/parameters/init` enumerates the full dictionary and
 *   `GET …/parameters` reads the cached result.
 * - **ESC registers** — `GET`/`PUT …/registers/{address}` read and write raw
 *   EtherCAT Slave Controller register bytes.
 * - **Files (FoE)** — `GET`/`PUT …/files/{filename}` transfer files to and from
 *   the device over File-over-EtherCAT (firmware, configuration, logs).
 * - **Presence** — `GET …/online` probes whether a device still answers on the
 *   bus.
 *
 * The `/api/meta/*` endpoints expose static reference tables — AL status codes,
 * ESC register definitions, FoE error codes, and CoE data types — so clients
 * can decode numeric values without hard-coding them.
 *
 * ## Monitoring WebSocket
 *
 * A monitoring WebSocket is available at `/ws`. A client creates a monitoring
 * with `POST /api/monitorings` (a topic, sampling interval, buffer size, and a
 * list of parameters), then subscribes to its topic over the socket by sending
 * `{"subscribe":"<topic>"}` (and `{"unsubscribe":"<topic>"}` to stop). The server
 * samples the parameters off the real-time thread, accumulates `bufferSize` rows,
 * and publishes each batch to that topic — delivered only to the clients
 * subscribed to it. The messages:
 *
 * ```json
 * {"type": "monitoring", "topic": "left-leg", "data": [[1735821000123, 39000, 41], ...]}
 * {"type": "notification", "data": {"event": "slaves_changed"}}
 * ```
 *
 * `data` is an array of sample rows; each row is `[timestampMs, v0, v1, ...]`
 * whose values are positionally ordered by the monitoring's `parameters` (fetch
 * the order, and how each value is sourced, via `GET /api/monitorings/{topic}`).
 * A value is `null` while its owning device is not exchanging (SAFE-OP/OP).
 * Numbers beyond 2^53 lose precision in a JS client; the targeted values are
 * 32-bit integers and floats.
 *
 * ## Networking, TLS, and CORS
 *
 * The server binds to `127.0.0.1:8443` and is reached at
 * `https://local.motion-master.synapticon.com:8443` (a DNS record that resolves
 * to localhost), with a real, publicly-trusted TLS certificate bundled in every
 * release. CORS allows the single origin
 * `https://motion-master.synapticon.com`, the hosted progressive web app.
 *
 * ## Conventions
 *
 * - **Errors** — failures return the matching HTTP status with a JSON body of
 *   the form `{ "error": "<message>" }`. The message is a human-readable string;
 *   `409` on `init` specifically signals "already initialised" so a client can
 *   tell a reconnect from a real failure.
 * - **Spec** — this document is served live at `GET /api/swagger.yml`, and the
 *   running version at `GET /api/version`.
 */
export class Api<
  SecurityDataType extends unknown,
> extends HttpClient<SecurityDataType> {
  adapters = {
    /**
     * No description
     *
     * @name GetAdapters
     * @summary List network adapters on the host
     * @request GET:/api/adapters
     */
    getAdapters: (params: RequestParams = {}) =>
      this.request<
        {
          /**
           * MAC address in colon-separated uppercase format
           * @example "AA:BB:CC:DD:EE:FF"
           */
          mac: string;
          /**
           * OS interface name (e.g. eth0 on Linux, \\Device\\NPF_{GUID} on Windows)
           * @example "eth0"
           */
          name: string;
        }[],
        any
      >({
        path: `/api/adapters`,
        method: "GET",
        format: "json",
        ...params,
      }),
  };
  swaggerYml = {
    /**
     * No description
     *
     * @name GetSwaggerYml
     * @summary OpenAPI specification
     * @request GET:/api/swagger.yml
     */
    getSwaggerYml: (params: RequestParams = {}) =>
      this.request<string, any>({
        path: `/api/swagger.yml`,
        method: "GET",
        ...params,
      }),
  };
  version = {
    /**
     * No description
     *
     * @name GetVersion
     * @summary Get Motion Master version
     * @request GET:/api/version
     */
    getVersion: (params: RequestParams = {}) =>
      this.request<
        {
          /** @example "6.0.0" */
          version: string;
        },
        any
      >({
        path: `/api/version`,
        method: "GET",
        format: "json",
        ...params,
      }),
  };
  devices = {
    /**
     * No description
     *
     * @name GetDevice
     * @summary Get a single fieldbus device by bus position
     * @request GET:/api/devices/{slavePosition}
     */
    getDevice: (slavePosition: number, params: RequestParams = {}) =>
      this.request<
        {
          /**
           * 1-based position on the fieldbus
           * @example 1
           */
          slavePosition: number;
          /**
           * Human-readable device name from SII EEPROM
           * @example "SOMANET Node"
           */
          name: string;
          /**
           * Vendor ID from EEPROM
           * @format int64
           * @example 131073
           */
          vendorId: number;
          /**
           * Product code from EEPROM
           * @format int64
           * @example 5570560
           */
          productCode: number;
          /**
           * Revision number from EEPROM
           * @format int64
           * @example 1
           */
          revisionNumber: number;
          /**
           * Serial number from EEPROM
           * @format int64
           * @example 12345
           */
          serialNumber: number;
        },
        void
      >({
        path: `/api/devices/${slavePosition}`,
        method: "GET",
        format: "json",
        ...params,
      }),

    /**
     * @description Performs a live AL-state read for the device and reports whether it currently has an active SDO mailbox — i.e. it is in PRE-OP, SAFE-OP, or OP with no error indicator set. INIT, BOOT, and any error state are reported as offline. The device must already be known from a prior scan. Reading one device at a time means a single missing device reports offline without failing the others.
     *
     * @name GetDeviceOnline
     * @summary Check whether a device is online
     * @request GET:/api/devices/{slavePosition}/online
     */
    getDeviceOnline: (slavePosition: number, params: RequestParams = {}) =>
      this.request<
        {
          /**
           * 1-based position on the fieldbus
           * @example 1
           */
          slavePosition: number;
          /**
           * True when the device has an active SDO mailbox (PRE-OP, SAFE-OP, or OP with no error indicator); false otherwise.
           * @example true
           */
          online: boolean;
        },
        void | {
          /**
           * Human-readable error message from the driver
           * @example "FPRD slave 1: wkc=0"
           */
          error: string;
        }
      >({
        path: `/api/devices/${slavePosition}/online`,
        method: "GET",
        format: "json",
        ...params,
      }),

    /**
     * @description Enumerates the entire CoE object dictionary of the device at `slavePosition` via the SDO Info service ("Get Object List" → "Get Object Description" → "Get Entry Description") and rebuilds its parameter map. The device must be in PRE-OP, SAFE-OP, or OP (mailbox communication active).  On a fully populated drive the call can take several seconds. When `readValues=true` each entry is additionally read via SDO upload and the decoded value is stored on the parameter; entries that fail to read keep their type-appropriate default and the call still succeeds.
     *
     * @name InitializeDeviceParameters
     * @summary Initialise the parameter list for a device by enumerating its object dictionary
     * @request POST:/api/devices/{slavePosition}/parameters/init
     */
    initializeDeviceParameters: (
      slavePosition: number,
      query?: {
        /**
         * When `true`, perform an SDO upload for every entry and decode the value.  Defaults to `false` — only schema (name, type, access) is populated.
         * @default false
         */
        readValues?: boolean;
      },
      params: RequestParams = {},
    ) =>
      this.request<
        DeviceParameter[],
        void | {
          /**
           * Human-readable error message from the driver
           * @example "FPRD slave 1: wkc=0"
           */
          error: string;
        }
      >({
        path: `/api/devices/${slavePosition}/parameters/init`,
        method: "POST",
        query: query,
        format: "json",
        ...params,
      }),

    /**
     * @description Returns the parameter list populated by the most recent call to `POST /api/devices/{slavePosition}/parameters/init`.  Empty before the first call.  Entries are ordered ascending by `(index, subindex)`.
     *
     * @name GetDeviceParameters
     * @summary Read the previously initialised parameter list for a device
     * @request GET:/api/devices/{slavePosition}/parameters
     */
    getDeviceParameters: (slavePosition: number, params: RequestParams = {}) =>
      this.request<DeviceParameter[], void>({
        path: `/api/devices/${slavePosition}/parameters`,
        method: "GET",
        format: "json",
        ...params,
      }),

    /**
     * @description Reads `length` bytes from the ESC register at `address` on the slave at `slavePosition` using a Configured-Address Read (FPRD) datagram. Pass `address` in decimal (e.g. 272 for DL Status at 0x0110).
     *
     * @name ReadRegister
     * @summary Read bytes from an ESC register
     * @request GET:/api/devices/{slavePosition}/registers/{address}
     */
    readRegister: (
      slavePosition: number,
      address: number,
      query: {
        /**
         * Number of bytes to read
         * @min 1
         * @example 2
         */
        length: number;
      },
      params: RequestParams = {},
    ) =>
      this.request<
        {
          /**
           * Register bytes in ascending address order
           * @example [65,0]
           */
          data: number[];
        },
        void | {
          /**
           * Human-readable error message from the driver
           * @example "FPRD slave 1: wkc=0"
           */
          error: string;
        }
      >({
        path: `/api/devices/${slavePosition}/registers/${address}`,
        method: "GET",
        query: query,
        format: "json",
        ...params,
      }),

    /**
     * @description Writes bytes to the ESC register at `address` on the slave at `slavePosition` using a Configured-Address Write (FPWR) datagram. Pass `address` in decimal (e.g. 272 for DL Status at 0x0110).
     *
     * @name WriteRegister
     * @summary Write bytes to an ESC register
     * @request POST:/api/devices/{slavePosition}/registers/{address}
     */
    writeRegister: (
      slavePosition: number,
      address: number,
      data: {
        /**
         * Bytes to write in ascending address order
         * @example [1,0]
         */
        data: number[];
      },
      params: RequestParams = {},
    ) =>
      this.request<
        {
          /** @example true */
          ok: boolean;
        },
        void | {
          /**
           * Human-readable error message from the driver
           * @example "FPRD slave 1: wkc=0"
           */
          error: string;
        }
      >({
        path: `/api/devices/${slavePosition}/registers/${address}`,
        method: "POST",
        body: data,
        type: ContentType.Json,
        format: "json",
        ...params,
      }),

    /**
     * @description Reads the process-data watchdog timeout the slave at `slavePosition` is configured with, decoded from the watchdog divider (0x0400) and process-data watchdog time (0x0420) ESC registers. This is the configured timeout itself, not the expiration counter exposed by `GET /api/devices/diagnostics`. A device in OP that misses process data past this timeout faults itself to SAFE-OP+error; raising it lets a device tolerate the brief PDO pause of a whole-bus re-map.
     *
     * @name GetProcessDataWatchdog
     * @summary Read the process-data (sync-manager) watchdog configuration
     * @request GET:/api/devices/{slavePosition}/watchdog
     */
    getProcessDataWatchdog: (
      slavePosition: number,
      params: RequestParams = {},
    ) =>
      this.request<
        ProcessDataWatchdog,
        void | {
          /**
           * Human-readable error message from the driver
           * @example "FPRD slave 1: wkc=0"
           */
          error: string;
        }
      >({
        path: `/api/devices/${slavePosition}/watchdog`,
        method: "GET",
        format: "json",
        ...params,
      }),

    /**
     * @description Sets the process-data watchdog timeout of the slave at `slavePosition`, writing the process-data watchdog time register (0x0420). The device's watchdog divider (0x0400) is left untouched (it is shared with the PDI watchdog), so the achieved timeout is rounded to that divider's tick base — the response reports the value actually programmed. A `timeoutMs` of 0 disables the watchdog. The write persists across re-maps and re-scans until the ESC reloads EEPROM (power cycle).
     *
     * @name SetProcessDataWatchdog
     * @summary Set the process-data (sync-manager) watchdog timeout
     * @request PUT:/api/devices/{slavePosition}/watchdog
     */
    setProcessDataWatchdog: (
      slavePosition: number,
      data: {
        /**
         * Desired watchdog timeout in milliseconds; 0 disables the watchdog
         * @min 0
         * @example 200
         */
        timeoutMs: number;
      },
      params: RequestParams = {},
    ) =>
      this.request<
        ProcessDataWatchdog,
        void | {
          /**
           * Human-readable error message from the driver
           * @example "FPRD slave 1: wkc=0"
           */
          error: string;
        }
      >({
        path: `/api/devices/${slavePosition}/watchdog`,
        method: "PUT",
        body: data,
        type: ContentType.Json,
        format: "json",
        ...params,
      }),

    /**
     * @description Performs a CoE SDO upload — reads the value of object `index:subindex` from the device at `slavePosition` and returns the raw bytes as a JSON array. Both `index` and `subindex` accept decimal or hexadecimal notation; prefix with `0x` for hex (e.g. `0x6064` or `24676` for object 0x6064).
     *
     * @name SdoUpload
     * @summary Upload an object dictionary entry from a device (CoE SDO upload)
     * @request GET:/api/devices/{slavePosition}/sdo/{index}/{subindex}
     */
    sdoUpload: (
      slavePosition: number,
      index: number,
      subindex: number,
      params: RequestParams = {},
    ) =>
      this.request<
        {
          /**
           * Object bytes as transferred by the SDO upload
           * @example [0,0,0,0]
           */
          data: number[];
        },
        void | {
          /**
           * Human-readable error message from the driver
           * @example "FPRD slave 1: wkc=0"
           */
          error: string;
        }
      >({
        path: `/api/devices/${slavePosition}/sdo/${index}/${subindex}`,
        method: "GET",
        format: "json",
        ...params,
      }),

    /**
     * @description Performs a CoE SDO download — writes the raw bytes in the request body to object `index:subindex` on the device at `slavePosition`. The byte count must match the object's length. Requires the device to be in PRE-OP, SAFE-OP, or OP (mailbox communication active). Both `index` and `subindex` accept decimal or hexadecimal notation; prefix with `0x` for hex (e.g. `0x6064` or `24676`).
     *
     * @name SdoDownload
     * @summary Download an object dictionary entry to a device (CoE SDO download)
     * @request PUT:/api/devices/{slavePosition}/sdo/{index}/{subindex}
     */
    sdoDownload: (
      slavePosition: number,
      index: number,
      subindex: number,
      data: {
        /**
         * Object bytes to write (little-endian, must match the object length)
         * @example [100,0,0,0]
         */
        data: number[];
      },
      params: RequestParams = {},
    ) =>
      this.request<
        {
          /** @example true */
          ok: boolean;
        },
        void | {
          /**
           * Human-readable error message from the driver
           * @example "FPRD slave 1: wkc=0"
           */
          error: string;
        }
      >({
        path: `/api/devices/${slavePosition}/sdo/${index}/${subindex}`,
        method: "PUT",
        body: data,
        type: ContentType.Json,
        format: "json",
        ...params,
      }),

    /**
     * @description Sends an FoE read request for `filename` to the device at `slavePosition` and returns the raw file bytes.  FoE is available in Boot, Pre-Op, Safe-Op, and Op states (device-dependent); the caller is responsible for ensuring the device is in a suitable state before calling.  The call blocks until the full file has been received or a per-packet timeout of 700 ms is exceeded.
     *
     * @name FoeReadFile
     * @summary Read a file from a device via FoE (File over EtherCAT)
     * @request GET:/api/devices/{slavePosition}/files/{filename}
     */
    foeReadFile: (
      slavePosition: number,
      filename: string,
      params: RequestParams = {},
    ) =>
      this.request<
        Blob,
        void | {
          /**
           * Human-readable error message from the driver
           * @example "FPRD slave 1: wkc=0"
           */
          error: string;
        }
      >({
        path: `/api/devices/${slavePosition}/files/${filename}`,
        method: "GET",
        ...params,
      }),

    /**
     * @description Sends an FoE write request for `filename` to the device at `slavePosition`, streaming the raw request body as the file contents.  FoE is available in Boot, Pre-Op, Safe-Op, and Op states (device-dependent); the caller is responsible for ensuring the device is in a suitable state before calling.
     *
     * @name FoeWriteFile
     * @summary Write a file to a device via FoE (File over EtherCAT)
     * @request PUT:/api/devices/{slavePosition}/files/{filename}
     */
    foeWriteFile: (
      slavePosition: number,
      filename: string,
      data: File,
      params: RequestParams = {},
    ) =>
      this.request<
        {
          /** @example true */
          ok?: boolean;
        },
        void | {
          /**
           * Human-readable error message from the driver
           * @example "FPRD slave 1: wkc=0"
           */
          error: string;
        }
      >({
        path: `/api/devices/${slavePosition}/files/${filename}`,
        method: "PUT",
        body: data,
        format: "json",
        ...params,
      }),

    /**
     * @description Reads live link-quality and watchdog diagnostics from each slave's EtherCAT Slave Controller (DL Status 0x0110, error-counter block 0x0300–0x0313, watchdog block 0x0440–0x0443) via FPRD. Counters are 8-bit, saturate at 255, and are cleared only by a power cycle or explicit write — poll this endpoint and watch for a rising delta rather than an absolute value. The per-port counters localise a degrading link to a specific cable/connector; the watchdog counters distinguish a slave that stopped receiving process data from a master-side problem. Omit `positions` to query all discovered devices. Returns 500 for transports without an ESC (e.g. SPoE).
     *
     * @name GetDeviceDiagnostics
     * @summary Read live ESC health diagnostics for devices
     * @request GET:/api/devices/diagnostics
     */
    getDeviceDiagnostics: (
      query?: {
        /**
         * Comma-separated list of 1-based slave positions to query (e.g. `1,2,3`). Omit to query all discovered devices.
         * @example "1,2"
         */
        positions?: string;
      },
      params: RequestParams = {},
    ) =>
      this.request<
        DeviceDiagnostics[],
        void | {
          /**
           * Human-readable error message from the driver
           * @example "FPRD slave 1: wkc=0"
           */
          error: string;
        }
      >({
        path: `/api/devices/diagnostics`,
        method: "GET",
        query: query,
        format: "json",
        ...params,
      }),

    /**
     * @description Reads the current EtherCAT Application Layer state for one or more devices. Omit the `positions` query parameter to query all discovered devices.
     *
     * @name GetDeviceStates
     * @summary Read current AL state for devices
     * @request GET:/api/devices/state
     */
    getDeviceStates: (
      query?: {
        /**
         * Comma-separated list of 1-based slave positions to query (e.g. `1,2,3`). Omit to query all discovered devices.
         * @example "1,2"
         */
        positions?: string;
      },
      params: RequestParams = {},
    ) =>
      this.request<
        {
          /**
           * 1-based position on the fieldbus
           * @example 1
           */
          slavePosition: number;
          /**
           * Raw AL Status register value (ETG.1000.6 §6.4.1). Bits 3:0 encode the state; bit 4 is the error indicator.
           * @example 17
           */
          alStatus: number;
          /**
           * Current AL state decoded from alStatus (ETG.1000.6 encoding): 1 = Init, 2 = PreOp, 3 = Boot, 4 = SafeOp, 8 = Op.
           * @example 1
           */
          alState: 1 | 2 | 3 | 4 | 8;
          /**
           * True when the AL Status error indicator bit (bit 4) is set
           * @example true
           */
          error: boolean;
          /**
           * AL Status Code register (ETG.1000.6 §6.4.1). Non-zero when error is true; identifies the error cause (e.g. 0x0014 = No valid firmware).
           * @example 20
           */
          alStatusCode: number;
        }[],
        void | {
          /**
           * Human-readable error message from the driver
           * @example "FPRD slave 1: wkc=0"
           */
          error: string;
        }
      >({
        path: `/api/devices/state`,
        method: "GET",
        query: query,
        format: "json",
        ...params,
      }),

    /**
     * @description Commands one or more devices to the requested EtherCAT Application Layer state and blocks until all targeted devices arrive or the timeout elapses. The settled state of every targeted device is then read back and returned in `devices`, each with a `reached` flag; `ok` is true only when every device reached the target. A device that did not reach it carries an `alStatusCode` explaining why. Omit `positions` or pass an empty array to target all discovered devices. State values are the standard AL state register encodings from ETG.1000.6: 1 (Init), 2 (PreOp), 3 (Boot), 4 (SafeOp), 8 (Op).
     *
     * @name TransitionToState
     * @summary Transition devices to an EtherCAT AL state
     * @request POST:/api/devices/state
     */
    transitionToState: (
      data: {
        /**
         * 1-based slave positions to target. Omit or pass an empty array to target all discovered devices.
         * @example [1,2]
         */
        positions?: number[];
        /**
         * Target EtherCAT AL state (ETG.1000.6 AL control register encoding): 1 = Init, 2 = PreOp, 3 = Boot, 4 = SafeOp, 8 = Op.
         * @example 8
         */
        state: 1 | 2 | 3 | 4 | 8;
        /**
         * Maximum time to wait for all devices, in milliseconds
         * @default 5000
         * @example 5000
         */
        timeout?: number;
      },
      params: RequestParams = {},
    ) =>
      this.request<
        {
          /**
           * True only when every targeted device reached the target state.
           * @example true
           */
          ok: boolean;
          /** Settled state of each targeted device, in the order targeted. */
          devices: {
            /**
             * 1-based position on the fieldbus
             * @example 1
             */
            slavePosition: number;
            /**
             * Raw AL Status register value (ETG.1000.6 §6.4.1). Bits 3:0 encode the state; bit 4 is the error indicator.
             * @example 2
             */
            alStatus: number;
            /**
             * Current AL state decoded from alStatus (ETG.1000.6 encoding): 1 = Init, 2 = PreOp, 3 = Boot, 4 = SafeOp, 8 = Op.
             * @example 2
             */
            alState: 1 | 2 | 3 | 4 | 8;
            /**
             * True when the AL Status error indicator bit (bit 4) is set
             * @example false
             */
            error: boolean;
            /**
             * AL Status Code register (ETG.1000.6 §6.4.1). Non-zero when error is true; identifies the error cause (e.g. 0x0014 = No valid firmware).
             * @example 0
             */
            alStatusCode: number;
            /**
             * True when this device reached the requested target state (error clear and alState equal to the requested state).
             * @example true
             */
            reached: boolean;
          }[];
        },
        void | {
          /**
           * Human-readable error message from the driver
           * @example "FPRD slave 1: wkc=0"
           */
          error: string;
        }
      >({
        path: `/api/devices/state`,
        method: "POST",
        body: data,
        type: ContentType.Json,
        format: "json",
        ...params,
      }),

    /**
     * No description
     *
     * @name GetDevices
     * @summary List all fieldbus devices
     * @request GET:/api/devices
     */
    getDevices: (params: RequestParams = {}) =>
      this.request<
        {
          /**
           * 1-based position on the fieldbus
           * @example 1
           */
          slavePosition: number;
          /**
           * Human-readable device name from SII EEPROM
           * @example "SOMANET Node"
           */
          name: string;
          /**
           * Vendor ID from EEPROM
           * @format int64
           * @example 131073
           */
          vendorId: number;
          /**
           * Product code from EEPROM
           * @format int64
           * @example 5570560
           */
          productCode: number;
          /**
           * Revision number from EEPROM
           * @format int64
           * @example 1
           */
          revisionNumber: number;
          /**
           * Serial number from EEPROM
           * @format int64
           * @example 12345
           */
          serialNumber: number;
        }[],
        any
      >({
        path: `/api/devices`,
        method: "GET",
        format: "json",
        ...params,
      }),
  };
  init = {
    /**
     * @description Constructs the requested fieldbus driver, opens the network interface, and makes the driver available for subsequent calls to /api/scan. The driver defaults to SOEM when omitted. SOEM has no adapter auto-detect: a network adapter must be supplied, otherwise init fails.
     *
     * @name Init
     * @summary Initialise the fieldbus driver
     * @request POST:/api/init
     */
    init: (
      data?: {
        /**
         * Fieldbus driver to use
         * @default "soem"
         * @example "soem"
         */
        driver?: "soem" | "spoe" | "igh";
        /**
         * Network interface name or MAC address. Required for SOEM — there is no auto-detect, so an empty value makes init fail.
         * @example "eth0"
         */
        adapter?: string;
      },
      params: RequestParams = {},
    ) =>
      this.request<
        {
          /** @example true */
          ok: boolean;
        },
        | void
        | {
            /** @example "already initialised — call reset() first" */
            error: string;
          }
        | {
            /**
             * Human-readable error message from the driver
             * @example "FPRD slave 1: wkc=0"
             */
            error: string;
          }
      >({
        path: `/api/init`,
        method: "POST",
        body: data,
        type: ContentType.Json,
        format: "json",
        ...params,
      }),
  };
  scan = {
    /**
     * @description Scans the fieldbus for slaves and configures their sync managers and FMMUs. Must be called after a successful POST /api/init. Slaves remain in INIT state after this call; state transitions are driven by subsequent API calls.
     *
     * @name Scan
     * @summary Scan the bus for slaves
     * @request POST:/api/scan
     */
    scan: (params: RequestParams = {}) =>
      this.request<
        {
          /**
           * Number of slaves discovered on the bus
           * @example 3
           */
          slaves: number;
        },
        {
          /**
           * Human-readable error message from the driver
           * @example "FPRD slave 1: wkc=0"
           */
          error: string;
        }
      >({
        path: `/api/scan`,
        method: "POST",
        format: "json",
        ...params,
      }),
  };
  reset = {
    /**
     * @description Transitions all slaves to INIT state, closes the network interface, and removes all Device objects. After this returns, POST /api/init and POST /api/scan may be called again.
     *
     * @name Reset
     * @summary Reset the fieldbus driver and clear the device list
     * @request POST:/api/reset
     */
    reset: (params: RequestParams = {}) =>
      this.request<
        {
          /** @example true */
          ok: boolean;
        },
        any
      >({
        path: `/api/reset`,
        method: "POST",
        format: "json",
        ...params,
      }),
  };
  log = {
    /**
     * @description Returns the most recent in-memory log lines as plain text, one entry per line. The ring buffer holds up to 100 000 entries (~10 MB).
     *
     * @name GetLog
     * @summary Get recent log output
     * @request GET:/api/log
     */
    getLog: (params: RequestParams = {}) =>
      this.request<string, any>({
        path: `/api/log`,
        method: "GET",
        ...params,
      }),
  };
  meta = {
    /**
     * @description Returns the static catalogue of EtherCAT AL Status Codes defined in ETG.1000.6 §6.4.1. These codes appear in the alStatusCode field of the device state response when an error is present and identify the cause of the state transition failure.
     *
     * @name GetAlStatusCodes
     * @summary List all known AL Status Codes
     * @request GET:/api/meta/al-status-codes
     */
    getAlStatusCodes: (params: RequestParams = {}) =>
      this.request<AlStatusCode[], any>({
        path: `/api/meta/al-status-codes`,
        method: "GET",
        format: "json",
        ...params,
      }),

    /**
     * @description Returns the static catalogue of well-known EtherCAT Slave Controller registers from the Beckhoff ESC datasheet (Section II) and ETG.1000.4. Use the address and length fields as inputs to the device register read/write endpoints.
     *
     * @name GetRegisters
     * @summary List all known ESC registers
     * @request GET:/api/meta/esc-registers
     */
    getRegisters: (params: RequestParams = {}) =>
      this.request<EscRegister[], any>({
        path: `/api/meta/esc-registers`,
        method: "GET",
        format: "json",
        ...params,
      }),

    /**
     * @description Returns the static catalogue of standard File over EtherCAT (FoE) error codes defined in ETG.1000.6 §5.5.  These codes appear in the FoE ERROR packet sent by the slave when a file transfer fails, and are surfaced in the error message returned by the file read endpoint.  Vendor-specific codes outside the 0x8000 range are not listed here.
     *
     * @name GetFoeErrorCodes
     * @summary List all known FoE error codes
     * @request GET:/api/meta/foe-error-codes
     */
    getFoeErrorCodes: (params: RequestParams = {}) =>
      this.request<FoeErrorCode[], any>({
        path: `/api/meta/foe-error-codes`,
        method: "GET",
        format: "json",
        ...params,
      }),

    /**
     * @description Returns the static catalogue of ETG.1020 data type codes used in CoE object dictionary entries.  Each entry of a `DeviceParameter` carries a `dataType` field whose value is one of the codes listed here.
     *
     * @name GetDataTypes
     * @summary List all known CoE object dictionary data types
     * @request GET:/api/meta/data-types
     */
    getDataTypes: (params: RequestParams = {}) =>
      this.request<ObjectDataTypeInfo[], any>({
        path: `/api/meta/data-types`,
        method: "GET",
        format: "json",
        ...params,
      }),
  };
  dcSync = {
    /**
     * @description Reads each slave's distributed-clock unit live from its EtherCAT Slave Controller (system-time delay 0x0928 and system-time difference 0x092C) via FPRD. The reference clock is the first DC-capable slave; every other DC slave continuously corrects its local clock toward it, and `systemTimeDifference` is the live deviation in nanoseconds. The figure is meaningful only while the bus is exchanging process data in SAFE-OP/OP (the reference time is distributed in the cyclic frame) and converges toward zero as the slaves' drift loops settle — poll this endpoint and watch for a slave whose deviation stays large or grows rather than an absolute value. Non-DC slaves report `dcCapable` false with zeroed values. Omit `positions` to query all discovered devices. Returns 500 for transports without an ESC (e.g. SPoE).
     *
     * @name GetDcSync
     * @summary Read live distributed-clock synchronisation status for devices
     * @request GET:/api/dc-sync
     */
    getDcSync: (
      query?: {
        /**
         * Comma-separated list of 1-based slave positions to query (e.g. `1,2,3`). Omit to query all discovered devices.
         * @example "1,2"
         */
        positions?: string;
      },
      params: RequestParams = {},
    ) =>
      this.request<
        DcSyncStatus[],
        void | {
          /**
           * Human-readable error message from the driver
           * @example "FPRD slave 1: wkc=0"
           */
          error: string;
        }
      >({
        path: `/api/dc-sync`,
        method: "GET",
        query: query,
        format: "json",
        ...params,
      }),
  };
  processImage = {
    /**
     * @description Returns the whole-bus process image: the byte sizes of each direction, the expected and last working counters and overall health, the number of retained image generations (one per re-map since the last reset), and every mapped object resolved to its absolute bit offset. While an image is live `configured` is true and the layout describes it. When no image is published (no device is in SAFE-OP/OP) but at least one generation has been mapped, `configured` is false and the layout describes the most recent retained generation — the last-known mapping — so a bus that has dropped out of SAFE-OP/OP stays inspectable; `lastWkc` then holds the final exchange value and working-counter health does not apply. The object lists are empty only before any image has ever been mapped. Object `name` is populated only for devices whose object dictionary has been enumerated.
     *
     * @name GetProcessImage
     * @summary Inspect the published EtherCAT process image
     * @request GET:/api/process-image
     */
    getProcessImage: (params: RequestParams = {}) =>
      this.request<
        {
          /**
           * Whether an image is currently published for exchange
           * @example true
           */
          configured: boolean;
          /**
           * Size of the output image (master→slave) in bytes
           * @example 12
           */
          outputBytes: number;
          /**
           * Size of the input image (slave→master) in bytes
           * @example 16
           */
          inputBytes: number;
          /**
           * Working counter expected from the devices currently exchanging
           * @example 3
           */
          expectedWkc: number;
          /**
           * Working counter from the most recent exchange (0 before any)
           * @example 3
           */
          lastWkc: number;
          /**
           * Whether the last working counter meets the expected value
           * @example true
           */
          healthy: boolean;
          /**
           * Number of process images retained since the last reset
           * @example 1
           */
          generations: number;
          /** Output-mapped objects (RxPDO), in image order */
          outputs: ProcessImageObject[];
          /** Input-mapped objects (TxPDO), in image order */
          inputs: ProcessImageObject[];
        },
        any
      >({
        path: `/api/process-image`,
        method: "GET",
        format: "json",
        ...params,
      }),
  };
  busConfig = {
    /**
     * @description Returns the static ESC configuration the master programmed into every slave during the last scan: station/alias addresses, mapped process-data sizes, the mailbox transport windows, the distributed-clock setup, and the configured Sync Managers and FMMUs. Read from cached state with no bus I/O and valid once the bus has been scanned. The list is empty before any scan, or when the active transport has no ESC (e.g. SPoE). Numeric fields (SM/FMMU type, mailbox protocol bits, SM flags) are raw register values; the client decodes them. `deviceName` is empty when no known device occupies the slave.
     *
     * @name GetBusConfig
     * @summary Inspect each slave's static EtherCAT Slave Controller configuration
     * @request GET:/api/bus-config
     */
    getBusConfig: (params: RequestParams = {}) =>
      this.request<SlaveConfig[], any>({
        path: `/api/bus-config`,
        method: "GET",
        format: "json",
        ...params,
      }),
  };
  monitorings = {
    /**
     * @description Returns every registered monitoring as a resource — its configuration plus, per parameter, how its value is sourced, and the current buffer fill.
     *
     * @name ListMonitorings
     * @summary List all monitorings
     * @request GET:/api/monitorings
     */
    listMonitorings: (params: RequestParams = {}) =>
      this.request<Monitoring[], any>({
        path: `/api/monitorings`,
        method: "GET",
        format: "json",
        ...params,
      }),

    /**
     * @description Registers a monitoring and starts sampling it off the real-time thread. Each parameter is classified once against the published process image: PDO-mapped objects are decoded from the live image; the rest are polled over SDO in the background. Create monitorings after the bus is operational so PDO objects are recognised as PDO. Monitoring is live-only — a parameter samples `null` while its owning device is not exchanging (SAFE-OP/OP).
     *
     * @name CreateMonitoring
     * @summary Create a monitoring
     * @request POST:/api/monitorings
     */
    createMonitoring: (
      data: {
        /**
         * URL-safe unique id; also the WebSocket topic. The name `pdos` is reserved.
         * @pattern ^[A-Za-z0-9._-]{1,64}$
         * @example "left-leg"
         */
        topic: string;
        /**
         * Optional human-readable label.
         * @example "Left Leg"
         */
        name?: string;
        /**
         * Sampling period in milliseconds.
         * @min 1
         * @example 1000
         */
        interval: number;
        /**
         * Samples accumulated before a batch is published over the WebSocket.
         * @min 16
         * @example 16
         */
        bufferSize: number;
        /**
         * Objects to sample, each as `[devicePosition, index, subindex]`.
         * @minItems 1
         * @example [[1,8240,1],[1,24676,0]]
         */
        parameters: number[][];
      },
      params: RequestParams = {},
    ) =>
      this.request<
        Monitoring,
        {
          error: string;
        }
      >({
        path: `/api/monitorings`,
        method: "POST",
        body: data,
        type: ContentType.Json,
        format: "json",
        ...params,
      }),

    /**
     * No description
     *
     * @name GetMonitoring
     * @summary Get a monitoring
     * @request GET:/api/monitorings/{topic}
     */
    getMonitoring: (topic: string, params: RequestParams = {}) =>
      this.request<Monitoring, void>({
        path: `/api/monitorings/${topic}`,
        method: "GET",
        format: "json",
        ...params,
      }),

    /**
     * @description Stops sampling and releases the monitoring's SDO parameters from the shared refresher — an object no remaining monitoring needs stops being polled.
     *
     * @name DeleteMonitoring
     * @summary Delete a monitoring
     * @request DELETE:/api/monitorings/{topic}
     */
    deleteMonitoring: (topic: string, params: RequestParams = {}) =>
      this.request<void, void>({
        path: `/api/monitorings/${topic}`,
        method: "DELETE",
        ...params,
      }),
  };
}
