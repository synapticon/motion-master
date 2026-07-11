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

import {
  AlStatusCode,
  DcSyncStatus,
  DeviceDiagnostics,
  DeviceParameter,
  EscRegister,
  FoeErrorCode,
  Monitoring,
  ObjectDataTypeInfo,
  OutputStageResult,
  ParameterCacheEntry,
  PdoMapping,
  PdoMappingRequest,
  ProcessDataWatchdog,
  ProcessImageObject,
  SlaveConfig,
  SlaveInformationInterface,
} from "./data-contracts";
import { ContentType, HttpClient, RequestParams } from "./http-client";

export class Api<
  SecurityDataType = unknown,
> extends HttpClient<SecurityDataType> {
  /**
   * No description
   *
   * @name GetAdapters
   * @summary List network adapters on the host
   * @request GET:/api/adapters
   */
  getAdapters = (params: RequestParams = {}) =>
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
    });
  /**
   * No description
   *
   * @name GetVersion
   * @summary Get Motion Master version
   * @request GET:/api/version
   */
  getVersion = (params: RequestParams = {}) =>
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
    });
  /**
   * @description The effective configuration after merging the JSONC config file (if any) over the built-in defaults — the same `Config` the server booted with. Read-only; changing it requires editing the config file and restarting. The shape mirrors the config file (server, fieldbus, logLevel, tls, gameLoop, recorder).
   *
   * @name GetStartedConfig
   * @summary Get the configuration Motion Master was started with
   * @request GET:/api/config
   */
  getStartedConfig = (params: RequestParams = {}) =>
    this.request<Record<string, any>, any>({
      path: `/api/config`,
      method: "GET",
      format: "json",
      ...params,
    });
  /**
   * @description A best-effort snapshot of the operating system and hardware Motion Master is running on, for the Connection page's System panel. Every field is best-effort: one that cannot be determined on the current platform is returned empty (strings) or zero (numbers) rather than failing the request. Collected on demand — not cached.
   *
   * @name GetSystemInfo
   * @summary Get host OS and hardware information
   * @request GET:/api/system-info
   */
  getSystemInfo = (params: RequestParams = {}) =>
    this.request<
      {
        /**
         * Friendly OS name.
         * @example "Ubuntu 24.04 LTS"
         */
        osName: string;
        /**
         * Kernel or OS build identifier.
         * @example "6.8.0-31-generic"
         */
        kernel: string;
        /**
         * Machine architecture.
         * @example "x86_64"
         */
        architecture: string;
        /**
         * Network host name.
         * @example "motion-master-pi"
         */
        hostname: string;
        /**
         * CPU brand string.
         * @example "Intel(R) Core(TM) i7-1185G7 @ 3.00GHz"
         */
        cpuModel: string;
        /**
         * Logical processor count.
         * @example 8
         */
        cpuCores: number;
        /**
         * Total physical RAM in bytes (0 if unknown).
         * @format int64
         * @example 16777216000
         */
        totalMemoryBytes: number;
        /**
         * Capacity of the filesystem holding the working directory, in bytes.
         * @format int64
         * @example 250790436864
         */
        diskTotalBytes: number;
        /**
         * Space available to an unprivileged process on that filesystem, in bytes.
         * @format int64
         * @example 98765432100
         */
        diskFreeBytes: number;
        /**
         * Container runtime when running inside one ("docker", "podman", "containerd", "kubernetes", "lxc"); empty on bare metal.
         * @example "docker"
         */
        container: string;
        /**
         * Docker version reported by `docker --version` on the host; empty if the docker CLI is not installed/on PATH (including inside Motion Master's own image).
         * @example "27.1.1"
         */
        dockerVersion: string;
      },
      any
    >({
      path: `/api/system-info`,
      method: "GET",
      format: "json",
      ...params,
    });
  /**
   * @description Reports the validity window and identity of the certificate the server is currently serving. `expiresSoon` trips within 7 days of `notAfter`, so the PWA can prompt the user to refresh. Note that an already-expired certificate blocks the browser from reaching this endpoint at all; the binary self-heals an expired/missing certificate at startup (fetching from the rolling release) and exposes `--update-cert` for manual refresh. Returns 500 if the certificate file cannot be read or parsed.
   *
   * @name GetCert
   * @summary Get the TLS certificate validity window
   * @request GET:/api/cert
   */
  getCert = (params: RequestParams = {}) =>
    this.request<
      {
        /**
         * Filesystem path of the certificate being served.
         * @example "/opt/motion-master/cert.pem"
         */
        path: string;
        /**
         * Subject common name (CN).
         * @example "local.motion-master.synapticon.com"
         */
        subject: string;
        /**
         * Issuer common name (CN).
         * @example "R10"
         */
        issuer: string;
        /**
         * Start of the validity window (ISO 8601 UTC).
         * @format date-time
         * @example "2026-05-01T00:00:00Z"
         */
        notBefore: string;
        /**
         * End of the validity window / expiry (ISO 8601 UTC).
         * @format date-time
         * @example "2026-08-01T00:00:00Z"
         */
        notAfter: string;
        /**
         * Whole days until expiry; negative once expired.
         * @example 56
         */
        daysRemaining: number;
        /**
         * Whether the certificate is already past notAfter.
         * @example false
         */
        expired: boolean;
        /**
         * True when expired or within 7 days of notAfter.
         * @example false
         */
        expiresSoon: boolean;
        /** Every certificate present in the served PEM file, leaf first, in chain order (leaf → intermediate(s)). A fullchain PEM usually stops at the intermediate — the root lives in the OS trust store and is not transmitted — but the last entry's `issuer` still names that root. */
        chain?: {
          /**
           * Subject common name (CN) of this certificate.
           * @example "local.motion-master.synapticon.com"
           */
          subject: string;
          /**
           * Issuer common name (CN) — the subject of the next link up.
           * @example "R10"
           */
          issuer: string;
          /**
           * Subject organization (O) — the friendly CA name (e.g. "Let's Encrypt") that the short CN does not carry; empty if absent (leaf certs often have no O).
           * @example "Let's Encrypt"
           */
          organization: string;
          /**
           * Issuer organization (O) — names the next link's (or, for the last link, the root's) organization; empty if absent.
           * @example "Internet Security Research Group"
           */
          issuerOrganization: string;
        }[];
      },
      {
        /**
         * Human-readable reason the certificate could not be read.
         * @example "cannot parse PEM certificate: /opt/motion-master/cert.pem"
         */
        error: string;
      }
    >({
      path: `/api/cert`,
      method: "GET",
      format: "json",
      ...params,
    });
  /**
   * @description Downloads a fresh certificate and key from the rolling release, validates the pair (parses, CN matches, not expired, key matches cert), and atomically installs them next to the binary. The new certificate only takes effect after a restart (the TLS listener loads the certificate once at startup), indicated by `restartRequired` in the response. Use the still-valid proactive case; an already-expired certificate is healed by the binary at startup instead.
   *
   * @name RefreshCert
   * @summary Fetch and install a fresh TLS certificate
   * @request POST:/api/cert/refresh
   */
  refreshCert = (params: RequestParams = {}) =>
    this.request<
      {
        /** @example "/opt/motion-master/cert.pem" */
        path?: string;
        /** @example "local.motion-master.synapticon.com" */
        subject?: string;
        /** @example "R10" */
        issuer?: string;
        /** @format date-time */
        notBefore?: string;
        /** @format date-time */
        notAfter: string;
        /** @example 89 */
        daysRemaining: number;
        /** @example false */
        expired?: boolean;
        /** @example false */
        expiresSoon?: boolean;
        /**
         * Always true — restart Motion Master to serve the new certificate.
         * @example true
         */
        restartRequired: boolean;
      },
      | {
          /** @example "downloaded key does not match certificate" */
          error: string;
        }
      | {
          /** @example "certificate refresh is not configured" */
          error: string;
        }
    >({
      path: `/api/cert/refresh`,
      method: "POST",
      format: "json",
      ...params,
    });
  /**
   * No description
   *
   * @name GetDevice
   * @summary Get a single fieldbus device by bus position
   * @request GET:/api/devices/{slavePosition}
   */
  getDevice = (slavePosition: number, params: RequestParams = {}) =>
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
    });
  /**
   * @description Constructs the requested fieldbus driver, opens the network interface, and makes the driver available for subsequent calls to /api/scan. The driver defaults to SOEM when omitted. SOEM has no adapter auto-detect: a network adapter must be supplied, otherwise init fails.
   *
   * @name Init
   * @summary Initialise the fieldbus driver
   * @request POST:/api/init
   */
  init = (
    data?: {
      /**
       * Fieldbus driver to use (only soem is implemented today; spoe is planned)
       * @default "soem"
       * @example "soem"
       */
      driver?: "soem" | "spoe";
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
    });
  /**
   * @description Scans the fieldbus for slaves and configures their sync managers and FMMUs. Must be called after a successful POST /api/init. Slaves remain in INIT state after this call; state transitions are driven by subsequent API calls. An empty bus is a successful scan returning 0 slaves, not an error: the master cannot distinguish a bus with no devices powered from a disconnected one, and the user recovers by powering devices on and scanning again. A 500 is returned only on a genuine driver failure (e.g. no driver initialised).
   *
   * @name Scan
   * @summary Scan the bus for slaves
   * @request POST:/api/scan
   */
  scan = (params: RequestParams = {}) =>
    this.request<
      {
        /**
         * Number of slaves discovered on the bus (0 if empty/unpowered)
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
    });
  /**
   * @description Transitions all slaves to INIT state, closes the network interface, and removes all Device objects. After this returns, POST /api/init and POST /api/scan may be called again.
   *
   * @name Reset
   * @summary Reset the fieldbus driver and clear the device list
   * @request POST:/api/reset
   */
  reset = (params: RequestParams = {}) =>
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
    });
  /**
   * @description Returns the most recent in-memory log lines as plain text, one entry per line. The ring buffer holds up to 100 000 entries (~10 MB).
   *
   * @name GetLog
   * @summary Get recent log output
   * @request GET:/api/log
   */
  getLog = (params: RequestParams = {}) =>
    this.request<string, any>({
      path: `/api/log`,
      method: "GET",
      ...params,
    });
  /**
   * @description Returns the static catalogue of EtherCAT AL Status Codes defined in ETG.1000.6 §6.4.1. These codes appear in the alStatusCode field of the device state response when an error is present and identify the cause of the state transition failure.
   *
   * @name GetAlStatusCodes
   * @summary List all known AL Status Codes
   * @request GET:/api/meta/al-status-codes
   */
  getAlStatusCodes = (params: RequestParams = {}) =>
    this.request<AlStatusCode[], any>({
      path: `/api/meta/al-status-codes`,
      method: "GET",
      format: "json",
      ...params,
    });
  /**
   * @description Returns the static catalogue of well-known EtherCAT Slave Controller registers from the Beckhoff ESC datasheet (Section II) and ETG.1000.4. Use the address and length fields as inputs to the device register read/write endpoints.
   *
   * @name GetRegisters
   * @summary List all known ESC registers
   * @request GET:/api/meta/esc-registers
   */
  getRegisters = (params: RequestParams = {}) =>
    this.request<EscRegister[], any>({
      path: `/api/meta/esc-registers`,
      method: "GET",
      format: "json",
      ...params,
    });
  /**
   * @description Returns the static catalogue of standard File over EtherCAT (FoE) error codes defined in ETG.1000.6 §5.5.  These codes appear in the FoE ERROR packet sent by the slave when a file transfer fails, and are surfaced in the error message returned by the file read endpoint.  Vendor-specific codes outside the 0x8000 range are not listed here.
   *
   * @name GetFoeErrorCodes
   * @summary List all known FoE error codes
   * @request GET:/api/meta/foe-error-codes
   */
  getFoeErrorCodes = (params: RequestParams = {}) =>
    this.request<FoeErrorCode[], any>({
      path: `/api/meta/foe-error-codes`,
      method: "GET",
      format: "json",
      ...params,
    });
  /**
   * @description Returns the static catalogue of ETG.1020 data type codes used in CoE object dictionary entries.  Each entry of a `DeviceParameter` carries a `dataType` field whose value is one of the codes listed here.
   *
   * @name GetDataTypes
   * @summary List all known CoE object dictionary data types
   * @request GET:/api/meta/data-types
   */
  getDataTypes = (params: RequestParams = {}) =>
    this.request<ObjectDataTypeInfo[], any>({
      path: `/api/meta/data-types`,
      method: "GET",
      format: "json",
      ...params,
    });
  /**
   * @description Enumerates the entire CoE object dictionary of the device at `slavePosition` via the SDO Info service ("Get Object List" → "Get Object Description" → "Get Entry Description") and rebuilds its parameter map. The device must be in PRE-OP, SAFE-OP, or OP (mailbox communication active).  On a fully populated drive the call can take several seconds. When `readValues=true` each entry is additionally read via SDO upload and the decoded value is stored on the parameter; entries that fail to read keep their type-appropriate default and the call still succeeds.
   *
   * @name InitializeDeviceParameters
   * @summary Initialise the parameter list for a device by enumerating its object dictionary
   * @request POST:/api/devices/{slavePosition}/parameters/init
   */
  initializeDeviceParameters = (
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
    });
  /**
   * @description Re-reads the value of every readable parameter already loaded for the device at `slavePosition`, leaving the parameter list itself intact (it does not re-enumerate the object dictionary — use `POST …/parameters/init` for that).  Each value is read PDO-aware: from the live process image when the device is exchanging and the object is PDO-mapped, otherwise via an SDO upload over the mailbox.  Write-only objects are skipped.  Best-effort: an entry that fails to read keeps its cached value and the call still succeeds.  Returns the refreshed list, ordered by `(index, subindex)`.
   *
   * @name ReadAllDeviceParameters
   * @summary Refresh the cached value of every parameter without re-enumerating the dictionary
   * @request POST:/api/devices/{slavePosition}/parameters/read
   */
  readAllDeviceParameters = (
    slavePosition: number,
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
      path: `/api/devices/${slavePosition}/parameters/read`,
      method: "POST",
      format: "json",
      ...params,
    });
  /**
   * @description Returns the parameter list populated by the most recent call to `POST /api/devices/{slavePosition}/parameters/init`.  Empty before the first call.  Entries are ordered ascending by `(index, subindex)`.
   *
   * @name GetDeviceParameters
   * @summary Read the previously initialised parameter list for a device
   * @request GET:/api/devices/{slavePosition}/parameters
   */
  getDeviceParameters = (slavePosition: number, params: RequestParams = {}) =>
    this.request<DeviceParameter[], void>({
      path: `/api/devices/${slavePosition}/parameters`,
      method: "GET",
      format: "json",
      ...params,
    });
  /**
   * @description Reads one parameter of the device at `slavePosition` and returns the full `DeviceParameter` (value plus metadata: data type, access, sync state). Unlike the raw `GET …/sdo/{index}/{subindex}` endpoint, this is PDO-aware. With `source=auto` (the default) the value is taken from the live process image when the device is exchanging and the object is PDO-mapped, otherwise via an SDO upload, otherwise from the cache. With `source=cache` the cached value is served with no bus access at all. Both `index` and `subindex` accept decimal or hexadecimal notation; prefix with `0x` for hex (e.g. `0x6064` or `24676` for object 0x6064).
   *
   * @name ReadParameter
   * @summary Read a single parameter value (PDO-aware, with optional cache-only mode)
   * @request GET:/api/devices/{slavePosition}/parameters/{index}/{subindex}
   */
  readParameter = (
    slavePosition: number,
    index: number,
    subindex: number,
    query?: {
      /**
       * `auto` (default) reads PDO-when-exchanging else SDO else cache; `cache` returns the cached value with no bus access.
       * @default "auto"
       */
      source?: "auto" | "cache";
    },
    params: RequestParams = {},
  ) =>
    this.request<
      DeviceParameter,
      void | {
        /**
         * Human-readable error message from the driver
         * @example "FPRD slave 1: wkc=0"
         */
        error: string;
      }
    >({
      path: `/api/devices/${slavePosition}/parameters/${index}/${subindex}`,
      method: "GET",
      query: query,
      format: "json",
      ...params,
    });
  /**
   * @description Writes one parameter of the device at `slavePosition`. The value is coerced to the parameter's declared data type, then routed: staged into the process image when the device is exchanging and the object is output (RxPDO) mapped — sent on the next cycle — otherwise written via an SDO download, otherwise held in the cache as pending until the device is back online. This is the endpoint to set a target (e.g. target torque 0x6071) or the modes of operation: it knows which objects are PDO-mapped and stages them into the process data, falling back to SDO transparently. Returns the updated `DeviceParameter` (with the coerced value and resulting sync state). Both `index` and `subindex` accept decimal or hexadecimal notation; prefix with `0x` for hex.
   *
   * @name WriteParameter
   * @summary Write a single parameter value (PDO-aware)
   * @request PUT:/api/devices/{slavePosition}/parameters/{index}/{subindex}
   */
  writeParameter = (
    slavePosition: number,
    index: number,
    subindex: number,
    data: {
      /**
       * The value to set, coerced to the parameter's declared type. A number for integer/real objects, a string for string objects, or an array of bytes for octet/domain objects.
       * @example 100
       */
      value: number | string | number[];
    },
    params: RequestParams = {},
  ) =>
    this.request<
      DeviceParameter,
      void | {
        /**
         * Human-readable error message from the driver
         * @example "FPRD slave 1: wkc=0"
         */
        error: string;
      }
    >({
      path: `/api/devices/${slavePosition}/parameters/${index}/${subindex}`,
      method: "PUT",
      body: data,
      type: ContentType.Json,
      format: "json",
      ...params,
    });
  /**
   * @description Reads `length` bytes from the ESC register at `address` on the slave at `slavePosition` using a Configured-Address Read (FPRD) datagram. Pass `address` in decimal (e.g. 272 for DL Status at 0x0110).
   *
   * @name ReadRegister
   * @summary Read bytes from an ESC register
   * @request GET:/api/devices/{slavePosition}/registers/{address}
   */
  readRegister = (
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
    });
  /**
   * @description Writes bytes to the ESC register at `address` on the slave at `slavePosition` using a Configured-Address Write (FPWR) datagram. Pass `address` in decimal (e.g. 272 for DL Status at 0x0110).
   *
   * @name WriteRegister
   * @summary Write bytes to an ESC register
   * @request POST:/api/devices/{slavePosition}/registers/{address}
   */
  writeRegister = (
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
    });
  /**
   * @description Reads the process-data watchdog timeout the slave at `slavePosition` is configured with, decoded from the watchdog divider (0x0400) and process-data watchdog time (0x0420) ESC registers. This is the configured timeout itself, not the expiration counter exposed by `GET /api/devices/diagnostics`. A device in OP that misses process data past this timeout faults itself to SAFE-OP+error; raising it lets a device tolerate the brief PDO pause of a whole-bus re-map.
   *
   * @name GetProcessDataWatchdog
   * @summary Read the process-data (sync-manager) watchdog configuration
   * @request GET:/api/devices/{slavePosition}/watchdog
   */
  getProcessDataWatchdog = (
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
    });
  /**
   * @description Sets the process-data watchdog timeout of the slave at `slavePosition`, writing the process-data watchdog time register (0x0420). The device's watchdog divider (0x0400) is left untouched (it is shared with the PDI watchdog), so the achieved timeout is rounded to that divider's tick base — the response reports the value actually programmed. A `timeoutMs` of 0 disables the watchdog. The write persists across re-maps and re-scans until the ESC reloads EEPROM (power cycle).
   *
   * @name SetProcessDataWatchdog
   * @summary Set the process-data (sync-manager) watchdog timeout
   * @request PUT:/api/devices/{slavePosition}/watchdog
   */
  setProcessDataWatchdog = (
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
    });
  /**
   * @description Reads the PDO mapping of the slave at `slavePosition` over CoE, grouped by mapping object: the sync-manager assignment (0x1C12 outputs / 0x1C13 inputs) and, for each assigned mapping object (0x16xx / 0x1Axx), its `pdoIndex` and ordered entries. `outputs` are RxPDO objects (master→slave), `inputs` are TxPDO objects (slave→master). Each entry carries its derived `bitOffset` within the direction's window. This is the grouped, round-trippable counterpart of the flat, whole-bus `GET /api/process-image`. Reads fresh over SDO, so the device's mailbox must be active (PRE-OP, SAFE-OP, or OP); a device in INIT or BOOT yields 409.
   *
   * @name GetDevicePdoMapping
   * @summary Read a device's PDO mapping grouped by object (CoE)
   * @request GET:/api/devices/{slavePosition}/pdo-mapping
   */
  getDevicePdoMapping = (slavePosition: number, params: RequestParams = {}) =>
    this.request<
      PdoMapping,
      void | {
        /**
         * Human-readable error message from the driver
         * @example "FPRD slave 1: wkc=0"
         */
        error: string;
      }
    >({
      path: `/api/devices/${slavePosition}/pdo-mapping`,
      method: "GET",
      format: "json",
      ...params,
    });
  /**
   * @description Reconfigures the PDO mapping of the slave at `slavePosition` over CoE: both directions' sync-manager assignment (0x1C12 outputs / 0x1C13 inputs) and the referenced mapping objects (0x16xx / 0x1Axx). `outputs` are RxPDO objects (master→slave), `inputs` are TxPDO objects (slave→master); the array order is the sync-manager assignment order, which fixes each object's position in the process image. Both directions must be present — an empty array clears that sync manager's assignment. **The device must be in PRE-OP.** The mapping and assignment objects are writable only in PRE-OP; in SAFE-OP/OP the sync managers are active and the slave rejects the write, and INIT/BOOT have no CoE mailbox. This is the "drop to PRE-OP, remap, climb back" flow: take the device to PRE-OP (`POST /api/devices/state`), call this, then transition it back to SAFE-OP/OP — at which point the whole-bus process image is re-mapped from the new mapping. The write follows the CoE ordering rule (clear assignment, rewrite each mapping object, then re-assign), is read back and verified, and is retried a few times so a dropped mailbox frame cannot leave the object dictionary half-configured. The response is the device's grouped read-back mapping (same shape as GET), whose `bitOffset` values are derived by the device (the request omits them). This endpoint does not itself change the AL state or re-map the process image.
   *
   * @name WriteDevicePdoMapping
   * @summary Rewrite a device's PDO mapping (CoE)
   * @request PUT:/api/devices/{slavePosition}/pdo-mapping
   */
  writeDevicePdoMapping = (
    slavePosition: number,
    data: PdoMappingRequest,
    params: RequestParams = {},
  ) =>
    this.request<
      PdoMapping,
      void | {
        /**
         * Human-readable error message from the driver
         * @example "FPRD slave 1: wkc=0"
         */
        error: string;
      }
    >({
      path: `/api/devices/${slavePosition}/pdo-mapping`,
      method: "PUT",
      body: data,
      type: ContentType.Json,
      format: "json",
      ...params,
    });
  /**
   * @description Performs a CoE SDO upload — reads the value of object `index:subindex` from the device at `slavePosition` and returns the raw bytes as a JSON array. Both `index` and `subindex` accept decimal or hexadecimal notation; prefix with `0x` for hex (e.g. `0x6064` or `24676` for object 0x6064).
   *
   * @name SdoUpload
   * @summary Upload an object dictionary entry from a device (CoE SDO upload)
   * @request GET:/api/devices/{slavePosition}/sdo/{index}/{subindex}
   */
  sdoUpload = (
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
    });
  /**
   * @description Performs a CoE SDO download — writes the raw bytes in the request body to object `index:subindex` on the device at `slavePosition`. The byte count must match the object's length. Requires the device to be in PRE-OP, SAFE-OP, or OP (mailbox communication active). Both `index` and `subindex` accept decimal or hexadecimal notation; prefix with `0x` for hex (e.g. `0x6064` or `24676`).
   *
   * @name SdoDownload
   * @summary Download an object dictionary entry to a device (CoE SDO download)
   * @request PUT:/api/devices/{slavePosition}/sdo/{index}/{subindex}
   */
  sdoDownload = (
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
    });
  /**
   * @description Sends an FoE read request for `filename` to the device at `slavePosition` and returns the raw file bytes.  FoE is available in Boot, Pre-Op, Safe-Op, and Op states (device-dependent); the caller is responsible for ensuring the device is in a suitable state before calling.  The call blocks until the full file has been received or a per-packet timeout of 700 ms is exceeded.
   *
   * @name FoeReadFile
   * @summary Read a file from a device via FoE (File over EtherCAT)
   * @request GET:/api/devices/{slavePosition}/files/{filename}
   */
  foeReadFile = (
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
    });
  /**
   * @description Sends an FoE write request for `filename` to the device at `slavePosition`, streaming the raw request body as the file contents.  FoE is available in Boot, Pre-Op, Safe-Op, and Op states (device-dependent); the caller is responsible for ensuring the device is in a suitable state before calling.
   *
   * @name FoeWriteFile
   * @summary Write a file to a device via FoE (File over EtherCAT)
   * @request PUT:/api/devices/{slavePosition}/files/{filename}
   */
  foeWriteFile = (
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
    });
  /**
   * @description Reads the raw SII EEPROM image of the device at `slavePosition` through the ESC's EEPROM-control registers and returns either the parsed structure or the raw bytes, selected by the `Accept` request header. With `Accept: application/json` (or `*\/*`, or no `Accept` header) the parsed `SlaveInformationInterface` is returned — the 128-byte fixed header plus the decoded category section (strings, general info, FMMU/Sync-Manager and PDO defaults, distributed-clock settings).  With `Accept: application/octet-stream` the raw EEPROM image is returned unparsed, suitable for a hex dump or archival. EEPROM access is a control-plane operation and is most reliable while the device is in INIT or PRE-OP.  String-index fields in the parsed structure (e.g. `general.nameIdx`) are 1-based references into `category.strings`.
   *
   * @name ReadSii
   * @summary Read a device's SII (Slave Information Interface / EEPROM)
   * @request GET:/api/devices/{slavePosition}/sii
   */
  readSii = (slavePosition: number, params: RequestParams = {}) =>
    this.request<
      SlaveInformationInterface,
      void | {
        /**
         * Human-readable error message from the driver
         * @example "FPRD slave 1: wkc=0"
         */
        error: string;
      }
    >({
      path: `/api/devices/${slavePosition}/sii`,
      method: "GET",
      format: "json",
      ...params,
    });
  /**
   * @description Writes the raw SII image in the request body to the device's EEPROM, one 16-bit word at a time. The image is parse-validated first and rejected if it is not a well-formed SII (a guard against bricking the device). **Destructive:** a wrong image can leave the device unidentifiable until re-flashed. The device adopts the new contents only after a **power cycle**, and the write is most reliable while the device is in INIT or PRE-OP. Intended for restoring a previously downloaded image.
   *
   * @name WriteSii
   * @summary Write a raw SII (EEPROM) image to a device
   * @request PUT:/api/devices/{slavePosition}/sii
   */
  writeSii = (slavePosition: number, data: File, params: RequestParams = {}) =>
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
      path: `/api/devices/${slavePosition}/sii`,
      method: "PUT",
      body: data,
      format: "json",
      ...params,
    });
  /**
   * @description Decodes a raw SII EEPROM image supplied in the request body and returns the parsed structure — the same decoder the device read path uses, exposed as a bus-independent utility. No device is involved; the Tools SII page uses this to view previously downloaded `.bin` files offline.
   *
   * @name ParseSii
   * @summary Parse a raw SII (EEPROM) image
   * @request POST:/api/sii/parse
   */
  parseSii = (data: File, params: RequestParams = {}) =>
    this.request<SlaveInformationInterface, void>({
      path: `/api/sii/parse`,
      method: "POST",
      body: data,
      format: "json",
      ...params,
    });
  /**
   * @description Reads live link-quality and watchdog diagnostics from each slave's EtherCAT Slave Controller (DL Status 0x0110, error-counter block 0x0300–0x0313, watchdog block 0x0440–0x0443) via FPRD. Counters are 8-bit, saturate at 255, and are cleared only by a power cycle or explicit write — poll this endpoint and watch for a rising delta rather than an absolute value. The per-port counters localise a degrading link to a specific cable/connector; the watchdog counters distinguish a slave that stopped receiving process data from a master-side problem. Omit `positions` to query all discovered devices. Returns 500 for transports without an ESC (e.g. SPoE).
   *
   * @name GetDeviceDiagnostics
   * @summary Read live ESC health diagnostics for devices
   * @request GET:/api/devices/diagnostics
   */
  getDeviceDiagnostics = (
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
    });
  /**
   * @description Reads each slave's distributed-clock unit live from its EtherCAT Slave Controller (system-time delay 0x0928 and system-time difference 0x092C) via FPRD. The reference clock is the first DC-capable slave; every other DC slave continuously corrects its local clock toward it, and `systemTimeDifference` is the live deviation in nanoseconds. The figure is meaningful only while the bus is exchanging process data in SAFE-OP/OP (the reference time is distributed in the cyclic frame) and converges toward zero as the slaves' drift loops settle — poll this endpoint and watch for a slave whose deviation stays large or grows rather than an absolute value. Non-DC slaves report `dcCapable` false with zeroed values. Omit `positions` to query all discovered devices. Returns 500 for transports without an ESC (e.g. SPoE).
   *
   * @name GetDcSync
   * @summary Read live distributed-clock synchronisation status for devices
   * @request GET:/api/dc-sync
   */
  getDcSync = (
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
    });
  /**
   * @description Reads the current EtherCAT Application Layer state for one or more devices. Omit the `positions` query parameter to query all discovered devices.
   *
   * @name GetDeviceStates
   * @summary Read current AL state for devices
   * @request GET:/api/devices/state
   */
  getDeviceStates = (
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
    });
  /**
   * @description Commands one or more devices to the requested EtherCAT Application Layer state and blocks until all targeted devices arrive or the timeout elapses. The settled state of every targeted device is then read back and returned in `devices`, each with a `reached` flag; `ok` is true only when every device reached the target. A device that did not reach it carries an `alStatusCode` explaining why. Omit `positions` or pass an empty array to target all discovered devices. State values are the standard AL state register encodings from ETG.1000.6: 1 (Init), 2 (PreOp), 3 (Boot), 4 (SafeOp), 8 (Op).
   *
   * @name TransitionToState
   * @summary Transition devices to an EtherCAT AL state
   * @request POST:/api/devices/state
   */
  transitionToState = (
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
    });
  /**
   * No description
   *
   * @name GetDevices
   * @summary List all fieldbus devices
   * @request GET:/api/devices
   */
  getDevices = (params: RequestParams = {}) =>
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
    });
  /**
   * @description Returns the whole-bus process image: the byte sizes of each direction, the expected and last working counters and overall health, the number of retained image generations (one per re-map since the last reset), and every mapped object resolved to its absolute bit offset. While an image is live `configured` is true and the layout describes it. When no image is published (no device is in SAFE-OP/OP) but at least one generation has been mapped, `configured` is false and the layout describes the most recent retained generation — the last-known mapping — so a bus that has dropped out of SAFE-OP/OP stays inspectable; `lastWkc` then holds the final exchange value and working-counter health does not apply. The object lists are empty only before any image has ever been mapped. Object `name` is populated only for devices whose object dictionary has been enumerated.
   *
   * @name GetProcessImage
   * @summary Inspect the published EtherCAT process image
   * @request GET:/api/process-image
   */
  getProcessImage = (params: RequestParams = {}) =>
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
    });
  /**
   * @description Streams the same `.mmpd` serialisation as the POST variant (the recorder's current span, with the process image embedded as a header) directly in the response body instead of writing a file — for the client SDK / browser, which parses the bytes offline. Request with `Accept: application/octet-stream`. Same span semantics as the POST; works in any state.
   *
   * @name StreamProcessDataDump
   * @summary Stream the recorder ring as a binary .mmpd
   * @request GET:/api/process-data/dump
   */
  streamProcessDataDump = (params: RequestParams = {}) =>
    this.request<
      Blob,
      {
        /** Human-readable reason the dump could not be produced */
        error: string;
      }
    >({
      path: `/api/process-data/dump`,
      method: "GET",
      ...params,
    });
  /**
   * @description Serialises the process-data recorder's current span — every cycle from the oldest still in the ring up to the newest at the instant of the call — to a `.mmpd` file and returns its path. Each cycle's full raw input and output images, sequence, and epoch-nanosecond timestamp are written, with the process image (per-device identity and PDO object layout, including object names and data types where the object dictionary has been enumerated) embedded as a header, so the file decodes fully offline with no running Motion Master and no live bus. Works in any state: while devices are exchanging (SAFE-OP/OP) it captures tail→head at that moment and ignores cycles recorded afterwards; after the bus has left the exchange states it uses the most recent retained image layout. The file is written under the configured `recorder.dumpDir` (default: a `motion-master` subdirectory of the OS temporary directory), created if absent. Motion Master binds 127.0.0.1, so the file is on the caller's own machine — only the path is returned (there is no download, list, or delete endpoint).
   *
   * @name DumpProcessData
   * @summary Dump the recorder ring to a binary .mmpd file
   * @request POST:/api/process-data/dump
   */
  dumpProcessData = (params: RequestParams = {}) =>
    this.request<
      {
        /**
         * Absolute path of the written .mmpd file
         * @example "/tmp/motion-master/dump-20260610T141530Z-300123.mmpd"
         */
        path: string;
      },
      {
        /**
         * Human-readable reason the dump could not be produced
         * @example "the recorder is empty — no cycles have been recorded yet"
         */
        error: string;
      }
    >({
      path: `/api/process-data/dump`,
      method: "POST",
      format: "json",
      ...params,
    });
  /**
   * @description Stages many output (RxPDO) objects at once — the "send all" action of the Process Data page. Each value is coerced to the object's declared data type and, when the object is output-mapped and its device is exchanging (SAFE-OP/OP), written into the output image so it is sent on the next real-time cycle and re-sent every cycle thereafter (set-once, sent-continuously). The batch is best-effort atomic: values are staged sequentially, so a batch can straddle two consecutive ~1 ms cycles. The request never fails as a whole — each object gets its own result, so the client can flag any object that was not cyclically staged (unknown device, coercion failure, object not output-mapped, or bus not exchanging; in the last two cases the value is still written via SDO/cache, just not cyclically driven).
   *
   * @name StageProcessDataOutputs
   * @summary Stage a batch of output values into the process image
   * @request POST:/api/process-data/outputs
   */
  stageProcessDataOutputs = (data: any[][], params: RequestParams = {}) =>
    this.request<
      {
        results: OutputStageResult[];
      },
      {
        error: string;
      }
    >({
      path: `/api/process-data/outputs`,
      method: "POST",
      body: data,
      type: ContentType.Json,
      format: "json",
      ...params,
    });
  /**
   * @description Returns the static ESC configuration the master programmed into every slave during the last scan: station/alias addresses, mapped process-data sizes, the mailbox transport windows, the distributed-clock setup, and the configured Sync Managers and FMMUs. Read from cached state with no bus I/O and valid once the bus has been scanned. The list is empty before any scan, or when the active transport has no ESC (e.g. SPoE). Numeric fields (SM/FMMU type, mailbox protocol bits, SM flags) are raw register values; the client decodes them. `deviceName` is empty when no known device occupies the slave.
   *
   * @name GetBusConfig
   * @summary Inspect each slave's static EtherCAT Slave Controller configuration
   * @request GET:/api/bus-config
   */
  getBusConfig = (params: RequestParams = {}) =>
    this.request<SlaveConfig[], any>({
      path: `/api/bus-config`,
      method: "GET",
      format: "json",
      ...params,
    });
  /**
   * @description Returns every registered monitoring as a resource — its configuration plus, per parameter, how its value is sourced, and the current buffer fill.
   *
   * @name ListMonitorings
   * @summary List all monitorings
   * @request GET:/api/monitorings
   */
  listMonitorings = (params: RequestParams = {}) =>
    this.request<Monitoring[], any>({
      path: `/api/monitorings`,
      method: "GET",
      format: "json",
      ...params,
    });
  /**
   * @description Registers a monitoring and starts sampling it off the real-time thread. Each parameter is classified once against the published process image: PDO-mapped objects are decoded from the live image; the rest are polled over SDO in the background. Create monitorings after the bus is operational so PDO objects are recognised as PDO. Monitoring is live-only — a parameter samples `null` while its owning device is not exchanging (SAFE-OP/OP).
   *
   * @name CreateMonitoring
   * @summary Create a monitoring
   * @request POST:/api/monitorings
   */
  createMonitoring = (
    data: {
      /**
       * URL-safe unique id; also the WebSocket topic.
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
       * Flush cadence in milliseconds (5–2000). Not a sample rate: every recorded cycle since the last flush is delivered, so a longer interval yields a larger batch rather than fewer samples. 16 ms (~one batch per 60 Hz display frame) is a good default for live plotting.
       * @min 5
       * @max 2000
       * @example 16
       */
      interval: number;
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
    });
  /**
   * No description
   *
   * @name GetMonitoring
   * @summary Get a monitoring
   * @request GET:/api/monitorings/{topic}
   */
  getMonitoring = (topic: string, params: RequestParams = {}) =>
    this.request<Monitoring, void>({
      path: `/api/monitorings/${topic}`,
      method: "GET",
      format: "json",
      ...params,
    });
  /**
   * @description Stops sampling and releases the monitoring's SDO parameters from the shared refresher — an object no remaining monitoring needs stops being polled.
   *
   * @name DeleteMonitoring
   * @summary Delete a monitoring
   * @request DELETE:/api/monitorings/{topic}
   */
  deleteMonitoring = (topic: string, params: RequestParams = {}) =>
    this.request<void, void>({
      path: `/api/monitorings/${topic}`,
      method: "DELETE",
      ...params,
    });
  /**
   * @description Returns one entry per cached parameter-definition file on disk, keyed by device identity (vendor, product, revision). The cache stores only the object-dictionary *definitions* (not live values), so a scan of previously-seen hardware can skip the slow SDO enumeration. Independent of the bus — works whether or not a driver is initialised.
   *
   * @name ListParameterCaches
   * @summary List on-disk parameter caches
   * @request GET:/api/parameter-caches
   */
  listParameterCaches = (params: RequestParams = {}) =>
    this.request<ParameterCacheEntry[], any>({
      path: `/api/parameter-caches`,
      method: "GET",
      format: "json",
      ...params,
    });
  /**
   * @description Returns the raw JSON cache file for the given id, verbatim, so it can be saved or inspected offline.
   *
   * @name GetParameterCache
   * @summary Download a parameter-cache file
   * @request GET:/api/parameter-caches/{id}
   */
  getParameterCache = (id: string, params: RequestParams = {}) =>
    this.request<object, void>({
      path: `/api/parameter-caches/${id}`,
      method: "GET",
      format: "json",
      ...params,
    });
  /**
   * @description Removes one cached file from disk. The next scan of that hardware re-enumerates it. Useful when a vendor reused a revision across an object-dictionary change.
   *
   * @name DeleteParameterCache
   * @summary Delete a parameter-cache file
   * @request DELETE:/api/parameter-caches/{id}
   */
  deleteParameterCache = (id: string, params: RequestParams = {}) =>
    this.request<void, void>({
      path: `/api/parameter-caches/${id}`,
      method: "DELETE",
      ...params,
    });
}
