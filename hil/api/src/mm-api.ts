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
 * @version 6.0.0-alpha.11
 * @baseUrl https://local.motion-master.synapticon.com:8443
 *
 * HTTP API for Motion Master motion control software. A monitoring WebSocket is also available at /ws — clients fetch the PDO schema via GET /api/monitoring/pdos and then subscribe to real-time updates.
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
     * @description Commands one or more devices to the requested EtherCAT Application Layer state and blocks until all targeted devices arrive or the timeout elapses. Devices that do not arrive in time are logged at error level; the call still returns 200. Omit `positions` or pass an empty array to target all discovered devices. State values are the standard AL state register encodings from ETG.1000.6: 1 (Init), 2 (PreOp), 3 (Boot), 4 (SafeOp), 8 (Op).
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
}
