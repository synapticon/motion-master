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
  BrakeState,
  Cia402Status,
  DcSyncStatus,
  DeviceDiagnostics,
  DeviceParameter,
  EscRegister,
  EsiParseResult,
  FoeErrorCode,
  GameLoopHealth,
  MailboxErrorCode,
  Monitoring,
  ObjectDataTypeInfo,
  OutputStageResult,
  ParameterCacheEntry,
  PdoMapping,
  PdoMappingRequest,
  ProcedureListing,
  ProcedureRequest,
  ProcedureSnapshot,
  ProcessDataWatchdog,
  ProcessImageObject,
  SdoAbortCode,
  SlaveConfig,
  SlaveInformationInterface,
  UserCacheListing,
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
        macLinux: string;
        /**
         * MAC address in dash-separated uppercase format
         * @example "AA-BB-CC-DD-EE-FF"
         */
        macWindows: string;
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
   * @description Returns this document — the OpenAPI specification embedded in the running binary — as YAML. Lets a client discover the exact API contract of the server it is talking to without a packaged file or a matching source checkout; the served spec always matches the running version.
   *
   * @name GetSwaggerYml
   * @summary Get this OpenAPI specification
   * @request GET:/api/swagger.yml
   */
  getSwaggerYml = (params: RequestParams = {}) =>
    this.request<string, any>({
      path: `/api/swagger.yml`,
      method: "GET",
      ...params,
    });
  /**
   * @description The effective configuration after merging the JSONC config file (if any) over the built-in defaults — the same `Config` the server booted with. The config file is either the one passed via `--config` or a `motion-master.jsonc` auto-discovered next to the executable (`--config` wins). Read-only; changing it requires editing the config file and restarting. The shape mirrors the config file (server, fieldbus, logLevel, tls, gameLoop, recorder, parameterCache, parameters).
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
         * The certificate's subjectAltName DNS entries — the hostnames it is valid for, and what a browser actually checks (the CN is legacy and ignored). The bundled certificate covers both the loopback name and the `*.ip.…` wildcard used by off-loopback deployments, so one file serves either. Empty for a certificate with no SAN extension.
         * @example ["local.motion-master.synapticon.com","*.ip.motion-master.synapticon.com"]
         */
        dnsNames?: string[];
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
         * Human-readable device name from SII EEPROM. For SOMANET drives this is the generic group name "SOMANET"; use productName to distinguish products.
         * @example "SOMANET"
         */
        name: string;
        /**
         * Canonical product name, independent of the SII EEPROM. For a recognised SOMANET product this distinguishes the product (e.g. "SOMANET Circulo"); otherwise it falls back to name.
         * @example "SOMANET Node"
         */
        productName: string;
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
        /**
         * Whether the device implements the CiA402 drive profile (both controlword 0x6040 and statusword 0x6041 present in its object dictionary). Gates the CiA402 motion-control endpoints under /api/devices/{slavePosition}/cia402.
         * @example true
         */
        isCia402: boolean;
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
   * @description Tears down all master-side state — unpublishes the process image, frees the recorder, removes all Device objects, and closes the network interface (releasing the NIC). It does not command the slaves to any AL state; the master simply stops talking to the bus. After this returns, POST /api/init and POST /api/scan may be called again.
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
   * @description Returns the static catalogue of CoE SDO abort codes defined in ETG.1000.6 §5.6.2.7.2, Table 41 (which reproduces the CANopen CiA 301 abort transfer codes).  A slave returns one of these when a mailbox SDO read/write fails; the code is embedded in the error text returned by the SDO endpoints.
   *
   * @name GetSdoAbortCodes
   * @summary List all known CoE SDO abort codes
   * @request GET:/api/meta/sdo-abort-codes
   */
  getSdoAbortCodes = (params: RequestParams = {}) =>
    this.request<SdoAbortCode[], any>({
      path: `/api/meta/sdo-abort-codes`,
      method: "GET",
      format: "json",
      ...params,
    });
  /**
   * @description Returns the static catalogue of CoE mailbox error codes defined in ETG.1000.4, Table 30 ("Error Reply Service Data").  A slave returns one of these when the mailbox layer (below a specific protocol such as CoE/FoE) rejects a transfer; the code is embedded in the error text the mailbox endpoints return.
   *
   * @name GetMailboxErrorCodes
   * @summary List all known CoE mailbox error codes
   * @request GET:/api/meta/mailbox-error-codes
   */
  getMailboxErrorCodes = (params: RequestParams = {}) =>
    this.request<MailboxErrorCode[], any>({
      path: `/api/meta/mailbox-error-codes`,
      method: "GET",
      format: "json",
      ...params,
    });
  /**
   * @description Returns the static catalogue of ETG.1020 data type codes used in CoE object dictionary entries.  Each entry of a `DeviceParameter` carries a `dataType` field whose value is one of the codes listed here.
   *
   * @name GetObjectDataTypes
   * @summary List all known CoE object dictionary data types
   * @request GET:/api/meta/object-data-types
   */
  getObjectDataTypes = (params: RequestParams = {}) =>
    this.request<ObjectDataTypeInfo[], any>({
      path: `/api/meta/object-data-types`,
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
         * @example "SDOread slave 1 0x2345:01 failed (no response — mailbox timeout)"
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
         * @example "SDOread slave 1 0x2345:01 failed (no response — mailbox timeout)"
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
         * @example "SDOread slave 1 0x2345:01 failed (no response — mailbox timeout)"
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
         * @example "SDOread slave 1 0x2345:01 failed (no response — mailbox timeout)"
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
         * @example "SDOread slave 1 0x2345:01 failed (no response — mailbox timeout)"
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
         * @example "SDOread slave 1 0x2345:01 failed (no response — mailbox timeout)"
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
         * @example "SDOread slave 1 0x2345:01 failed (no response — mailbox timeout)"
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
         * @example "SDOread slave 1 0x2345:01 failed (no response — mailbox timeout)"
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
         * @example "SDOread slave 1 0x2345:01 failed (no response — mailbox timeout)"
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
   * @description Returns the drive's current control state — the decoded state-machine state, the raw status (0x6041) and control (0x6040) words, and the active operation mode (display object 0x6061). The values are read live (from the process image when the device is exchanging, else over SDO), so poll this to drive a motion UI. Only valid for a CiA402 device (see the isCia402 flag on GET /api/devices).
   *
   * @name GetCia402Status
   * @summary Read a device's CiA402 control snapshot
   * @request GET:/api/devices/{slavePosition}/cia402
   */
  getCia402Status = (slavePosition: number, params: RequestParams = {}) =>
    this.request<Cia402Status, void>({
      path: `/api/devices/${slavePosition}/cia402`,
      method: "GET",
      format: "json",
      ...params,
    });
  /**
   * @description Writes the requested operation mode to 0x6060 and returns the resulting control snapshot. The drive reflects the accepted mode in 0x6061 (the modeOfOperation of the response) once it takes effect.
   *
   * @name SetCia402OperationMode
   * @summary Set a device's CiA402 operation mode
   * @request POST:/api/devices/{slavePosition}/cia402/mode
   */
  setCia402OperationMode = (
    slavePosition: number,
    data: {
      /**
       * CiA402 operation mode (0x6060 value): 0 NoMode, 1 Profile Position, 3 Profile Velocity, 4 Profile Torque, 6 Homing, 8 Cyclic Sync Position, 9 Cyclic Sync Velocity, 10 Cyclic Sync Torque.
       * @example 9
       */
      mode: number;
    },
    params: RequestParams = {},
  ) =>
    this.request<Cia402Status, void>({
      path: `/api/devices/${slavePosition}/cia402/mode`,
      method: "POST",
      body: data,
      type: ContentType.Json,
      format: "json",
      ...params,
    });
  /**
   * @description Drives the CiA402 device-control state machine. "enable" walks every intermediate transition to Operation Enabled (clearing a fault first if needed); "disable" goes to Switch On Disabled; "quickStop" triggers a quick stop; "faultReset" clears a latched fault. The device must be exchanging process data for "enable" to make progress. Returns the resulting control snapshot.
   *
   * @name RunCia402Command
   * @summary Run a CiA402 state-machine command
   * @request POST:/api/devices/{slavePosition}/cia402/command
   */
  runCia402Command = (
    slavePosition: number,
    data: {
      /**
       * The state-machine action to perform.
       * @example "enable"
       */
      command: "enable" | "disable" | "quickStop" | "faultReset";
    },
    params: RequestParams = {},
  ) =>
    this.request<Cia402Status, void>({
      path: `/api/devices/${slavePosition}/cia402/command`,
      method: "POST",
      body: data,
      type: ContentType.Json,
      format: "json",
      ...params,
    });
  /**
   * @description Writes the one setpoint that matches the active operation mode — target position (0x607A, INTEGER32) in PP/CSP, target velocity (0x60FF, INTEGER32) in PV/CSV, or target torque (0x6071, INTEGER16, per-mille of rated) in PT/CST. All are signed: negative values command reverse motion or regenerative torque. Routes through the live process image when the object is PDO-mapped and the device is exchanging, else an SDO download.
   *
   * @name SetCia402Target
   * @summary Set a CiA402 cyclic setpoint (target)
   * @request POST:/api/devices/{slavePosition}/cia402/target
   */
  setCia402Target = (
    slavePosition: number,
    data: {
      /**
       * Which setpoint to write (pick the one matching the active mode).
       * @example "velocity"
       */
      target: "position" | "velocity" | "torque";
      /**
       * The setpoint value in the object's own units (signed). For torque it is per-mille of rated and is narrowed to INTEGER16.
       * @example 1000
       */
      value: number;
    },
    params: RequestParams = {},
  ) =>
    this.request<void, void>({
      path: `/api/devices/${slavePosition}/cia402/target`,
      method: "POST",
      body: data,
      type: ContentType.Json,
      ...params,
    });
  /**
   * @description Returns every procedure available on the device, each with its descriptor and the state of its current or last run. One request is enough to render a whole per-device procedures view: the descriptor carries the title, description and caveats to display, and the snapshot says whether anything is running or how the last run went. The list is per device, not global — a procedure is offered only where it applies, so a third-party slave reports an empty list rather than controls that could only ever fail. A procedure that has never run reports an idle snapshot (`status` `idle`, `runCount` 0, every step `idle`), so every entry has the same shape.
   *
   * @name ListProcedures
   * @summary List the procedures a device supports
   * @request GET:/api/devices/{slavePosition}/procedures
   */
  listProcedures = (slavePosition: number, params: RequestParams = {}) =>
    this.request<ProcedureListing[], void>({
      path: `/api/devices/${slavePosition}/procedures`,
      method: "GET",
      format: "json",
      ...params,
    });
  /**
   * @description Starts the named procedure and returns as soon as the run is under way — this does **not** wait for it to finish. Poll `GET` on the same path for its outcome, or `DELETE` to cancel it. The request body carries the procedure's parameters, and each descriptor from `GET /api/devices/{slavePosition}/procedures` reports its own in `parameters` — name, type, bounds, default, and whether it is required — which is enough to build a form for any procedure without hard-coding one per name. A procedure that takes no parameters may be started with no body. Only one procedure may run on a device at a time; a second start is refused with 409 while the first is in flight. Most procedures require an active mailbox, so the device must be in PRE-OP, SAFE-OP or OP. A run the drive answers with an error is not a failure of *this* request — it is reported in the snapshot as a failed run.
   *
   * @name StartProcedure
   * @summary Start a procedure on a device
   * @request POST:/api/devices/{slavePosition}/procedures/{procedureName}
   */
  startProcedure = (
    slavePosition: number,
    procedureName: string,
    data?: ProcedureRequest,
    params: RequestParams = {},
  ) =>
    this.request<ProcedureSnapshot, void>({
      path: `/api/devices/${slavePosition}/procedures/${procedureName}`,
      method: "POST",
      body: data,
      type: ContentType.Json,
      format: "json",
      ...params,
    });
  /**
   * @description Returns the current run, or the last one to have finished. This is the whole progress surface — there is no push channel — and polling it is lossless: every finished step keeps its terminal status and value, so a step that both starts and completes between two polls is still reported as succeeded. A polling loop is `while status == "running"`. A procedure that has never run is not an absence: it reports an idle snapshot built from the procedure's step template, so a client renders one shape and needs no empty-state branch. The snapshot is retained after a run ends, so a client that reconnects (or a user returning to a page) sees how the last run went. It is dropped when the device set is rebuilt by a scan or reset, because bus positions may then name different hardware.
   *
   * @name GetProcedure
   * @summary Read the state of a procedure on a device
   * @request GET:/api/devices/{slavePosition}/procedures/{procedureName}
   */
  getProcedure = (
    slavePosition: number,
    procedureName: string,
    params: RequestParams = {},
  ) =>
    this.request<ProcedureSnapshot, void>({
      path: `/api/devices/${slavePosition}/procedures/${procedureName}`,
      method: "GET",
      format: "json",
      ...params,
    });
  /**
   * @description Asks the running procedure to stop and returns immediately. Where the procedure is waiting on the drive it also tells the drive to abort (for an OS command, 0x1024 = 3), so a run that is waiting stops when asked rather than carrying on to completion. The run then finishes with status `cancelled`. This cancels the *run*, not the record: the snapshot remains and reports how far it got.
   *
   * @name CancelProcedure
   * @summary Cancel a running procedure on a device
   * @request DELETE:/api/devices/{slavePosition}/procedures/{procedureName}
   */
  cancelProcedure = (
    slavePosition: number,
    procedureName: string,
    params: RequestParams = {},
  ) =>
    this.request<void, void>({
      path: `/api/devices/${slavePosition}/procedures/${procedureName}`,
      method: "DELETE",
      ...params,
    });
  /**
   * @description Reports the brake objects (0x2004) that decide what a release or engage will do: its current state, the release strategy, the pull time and the pull/hold voltages. Read this before commanding the brake. `softwareControllable` is false when the release strategy is `manualOutputVoltage`, meaning the firmware does not drive the brake at all and both commands below are no-ops. `releaseMovesShaft` is true for a pin brake, whose release procedure turns the motor to lift the load off the pin.
   *
   * @name GetBrake
   * @summary Read a drive's brake configuration and state
   * @request GET:/api/devices/{slavePosition}/brake
   */
  getBrake = (slavePosition: number, params: RequestParams = {}) =>
    this.request<BrakeState, void>({
      path: `/api/devices/${slavePosition}/brake`,
      method: "GET",
      format: "json",
      ...params,
    });
  /**
   * @description Writes "disengaged" to the brake state object (0x2004:07) and waits the drive's pull time (0x2004:03) plus `settle` before answering, because the firmware blocks motion — and motion-related OS commands — until the pull time expires. Answers with the brake state read back. **The release only actually happens while the drive is in OP ENABLED.** In any other state the write merely energises the brake output (phase D). Note also that entering OP ENABLED releases the brake automatically in normal operation but **not** in diagnostics mode, where the master owns the brake — which is why a diagnostics procedure has to call this. **On a pin brake this moves the shaft**: the controller raises current progressively until the load has lifted off the pin by the minimum displacement (0x2004:08), reversing direction if it reaches the current ceiling (0x2004:09, a percentage of rated current) first. Check `releaseMovesShaft` on `GET .../brake`. Brake release is open-loop — nothing confirms the brake let go — so the wait is the only margin there is. A brake whose release strategy is `manualOutputVoltage` is left alone and the returned state says so; that is not an error. **A released brake stays released**: nothing re-engages it for you, so on a vertical or loaded axis engage it again when you are done.
   *
   * @name ReleaseBrake
   * @summary Release (disengage) a drive's brake
   * @request POST:/api/devices/{slavePosition}/brake/release
   */
  releaseBrake = (
    slavePosition: number,
    query?: {
      /**
       * Extra wait in milliseconds on top of the drive's pull time. Size it for the machine — release is open-loop, so this is the only margin between "commanded" and "assumed released".
       * @min 0
       * @default 50
       * @example 50
       */
      settle?: number;
    },
    params: RequestParams = {},
  ) =>
    this.request<BrakeState, void>({
      path: `/api/devices/${slavePosition}/brake/release`,
      method: "POST",
      query: query,
      format: "json",
      ...params,
    });
  /**
   * @description Writes "engaged" to the brake state object (0x2004:07) and waits `settle` before answering with the brake state read back. There is no pull time on the way in — a brake is spring-engaged, so engaging it is the removal of voltage — so the wait is only `settle`. As with release, a brake whose release strategy is `manualOutputVoltage` is left alone and the returned state says so.
   *
   * @name EngageBrake
   * @summary Engage a drive's brake
   * @request POST:/api/devices/{slavePosition}/brake/engage
   */
  engageBrake = (
    slavePosition: number,
    query?: {
      /**
       * How long to wait in milliseconds after commanding the brake before answering.
       * @min 0
       * @default 50
       * @example 50
       */
      settle?: number;
    },
    params: RequestParams = {},
  ) =>
    this.request<BrakeState, void>({
      path: `/api/devices/${slavePosition}/brake/engage`,
      method: "POST",
      query: query,
      format: "json",
      ...params,
    });
  /**
   * @description Commands the device to persist its current parameters (the generic CANopen "store parameters" object 0x1010:01) and waits for confirmation. The server writes the ASCII "save" signature, waits a fixed settle (~1 s) for the drive to begin the flash write, then polls 0x1010:01 until it reads back 1 ("save completed"). Because a store in progress can leave the mailbox briefly unresponsive, a poll that does not yet confirm — a value mismatch or a transient read error alike — is retried up to `retries` more times, `interval` apart. The call blocks until the store is confirmed or the retry budget is exhausted (up to a few seconds); the device's mailbox must be active (PRE-OP/SAFE-OP/OP).
   *
   * @name StoreParameters
   * @summary Store a device's parameters to non-volatile memory
   * @request POST:/api/devices/{slavePosition}/store-parameters
   */
  storeParameters = (
    slavePosition: number,
    query?: {
      /**
       * Maximum number of extra confirmation polls after the first.
       * @min 0
       * @default 10
       * @example 5
       */
      retries?: number;
      /**
       * Delay between confirmation polls, in milliseconds.
       * @min 0
       * @default 500
       * @example 500
       */
      interval?: number;
    },
    params: RequestParams = {},
  ) =>
    this.request<void, void>({
      path: `/api/devices/${slavePosition}/store-parameters`,
      method: "POST",
      query: query,
      ...params,
    });
  /**
   * @description Commands the device to restore its default parameters (the generic CANopen "restore default parameters" object 0x1011) for the selected `group`, and waits for confirmation. The server writes the ASCII "load" signature to the matching 0x1011 sub-entry, waits a fixed settle (~1 s) for the device to begin, then polls that sub-entry until it reads back 1 ("restore completed"), retrying a poll that does not yet confirm — a value mismatch or a transient read error alike — up to `retries` more times, `interval` apart. The call blocks until the restore is confirmed or the retry budget is exhausted (up to a few seconds); the device's mailbox must be active (PRE-OP/SAFE-OP/OP). **Destructive:** this overwrites the selected group's parameter values in the device's volatile memory with the device's defaults, replacing the current live values (not the persisted store). Which values it touches, and whether it takes effect immediately or after the next reset, is device-specific.
   *
   * @name RestoreDefaultParameters
   * @summary Restore a device's default parameters
   * @request POST:/api/devices/{slavePosition}/restore-default-parameters
   */
  restoreDefaultParameters = (
    slavePosition: number,
    query?: {
      /**
       * Which group of defaults to restore — all (0x1011:01), communication (0x1011:02), application (0x1011:03), or manufacturer (0x1011:04).
       * @default "all"
       * @example "all"
       */
      group?: "all" | "communication" | "application" | "manufacturer";
      /**
       * Maximum number of extra confirmation polls after the first.
       * @min 0
       * @default 10
       * @example 5
       */
      retries?: number;
      /**
       * Delay between confirmation polls, in milliseconds.
       * @min 0
       * @default 500
       * @example 500
       */
      interval?: number;
    },
    params: RequestParams = {},
  ) =>
    this.request<void, void>({
      path: `/api/devices/${slavePosition}/restore-default-parameters`,
      method: "POST",
      query: query,
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
         * @example "SDOread slave 1 0x2345:01 failed (no response — mailbox timeout)"
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
         * @example "SDOread slave 1 0x2345:01 failed (no response — mailbox timeout)"
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
         * @example "SDOread slave 1 0x2345:01 failed (no response — mailbox timeout)"
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
         * @example "SDOread slave 1 0x2345:01 failed (no response — mailbox timeout)"
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
         * @example "SDOread slave 1 0x2345:01 failed (no response — mailbox timeout)"
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
         * @example "SDOread slave 1 0x2345:01 failed (no response — mailbox timeout)"
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
   * @description Decodes a vendor's ESI XML supplied in the request body. No device is involved — the Tools page uses this to inspect an ESI with no hardware present, which is the only way to see object descriptions, enum option labels, engineering units and min/max bounds: the CoE SDO-Information service reports none of them. The response carries the vendor, every module, and **every device with its own assembled `entries` table** — one row per addressable `(index, subindex)`, merged from the device's own dictionary plus the dictionaries of the modules its slots reference, with each row recording in `source` where it came from. Object-level annotation (an object's description and raw properties) is attached to **subindex 0 only** — that row *is* the object — rather than repeated onto every subindex. A RECORD member still carries its own description on its own row; an ARRAY element carries none, because the ESI describes an array once rather than per element. To read an object's text for any subindex, look at subindex 0 of the same index. Where a slot offers mutually exclusive module variants the merge necessarily collides; it is resolved last-wins and reported in that device's `warnings`. Pass `modules` to model one concrete configuration instead.
   *
   * @name ParseEsi
   * @summary Parse an EtherCAT Slave Information (ESI) file
   * @request POST:/api/esi/parse
   */
  parseEsi = (
    data: string,
    query?: {
      /** Comma-separated `ModuleIdent` values (hexadecimal or decimal) restricting the merge, applied to every device by intersection with the idents that device's slots actually reference. Omit to merge every module each device references. */
      modules?: string;
    },
    params: RequestParams = {},
  ) =>
    this.request<EsiParseResult, void>({
      path: `/api/esi/parse`,
      method: "POST",
      query: query,
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
         * @example "SDOread slave 1 0x2345:01 failed (no response — mailbox timeout)"
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
         * @example "SDOread slave 1 0x2345:01 failed (no response — mailbox timeout)"
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
         * Human-readable device name from SII EEPROM. For SOMANET drives this is the generic group name "SOMANET"; use productName to distinguish products.
         * @example "SOMANET"
         */
        name: string;
        /**
         * Canonical product name, independent of the SII EEPROM. For a recognised SOMANET product this distinguishes the product (e.g. "SOMANET Circulo"); otherwise it falls back to name.
         * @example "SOMANET Node"
         */
        productName: string;
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
        /**
         * Whether the device implements the CiA402 drive profile (both controlword 0x6040 and statusword 0x6041 present in its object dictionary). Gates the CiA402 motion-control endpoints under /api/devices/{slavePosition}/cia402.
         * @example true
         */
        isCia402: boolean;
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
   * @description Returns a point-in-time snapshot of the RT cyclic loop: the configured period and target rate, the cumulative achieved rate since start, the executed- and skipped-cycle counters, per-cycle task-execution timing (last / worst / mean), and whether real-time scheduling was acquired. Skipped cycles accumulate whenever the loop cannot meet its period (common on non-RT hosts such as Windows userspace) — a steadily rising `skippedCycles`, or an `achievedHz` well below `targetHz`, means the configured period is too aggressive for the hardware. Poll this endpoint and diff `executedCycles`/`skippedCycles` against `timestampUs` to chart the instantaneous rate over time.
   *
   * @name GetGameLoop
   * @summary Inspect real-time game-loop health
   * @request GET:/api/game-loop
   */
  getGameLoop = (params: RequestParams = {}) =>
    this.request<GameLoopHealth, any>({
      path: `/api/game-loop`,
      method: "GET",
      format: "json",
      ...params,
    });
  /**
   * @description Retimes the running RT loop to a new cycle period, in microseconds. The change takes effect within one cycle and is transient — it is not written back to the config file, so a restart reverts to the configured `gameLoop.periodUs`. Intended for tuning on hosts that cannot sustain the configured rate: if `skippedCycles` climbs steadily (common on Windows userspace), raise the period (e.g. from 1000 to 2000 µs) until the loop meets its grid. Only the master cadence is retimed — the process-data recorder ring is not resized, and no drive-side watchdog is touched, so raising the period toward a drive's PDO/SM watchdog window can fault that drive. Applying a period starts a fresh health epoch: the cumulative counters (`executedCycles`, `skippedCycles`, the `achievedHz` average, and `maxExecNs`/`avgExecNs`) reset so the snapshot reflects only the new period — letting you see straight away whether the change improved the loop's health. Returns the updated health snapshot.
   *
   * @name SetGameLoopPeriod
   * @summary Change the real-time cycle period
   * @request PUT:/api/game-loop
   */
  setGameLoopPeriod = (
    data: {
      /**
       * New cycle period in microseconds. Must be greater than 0.
       * @format int64
       * @min 1
       * @example 2000
       */
      periodUs: number;
    },
    params: RequestParams = {},
  ) =>
    this.request<GameLoopHealth, void>({
      path: `/api/game-loop`,
      method: "PUT",
      body: data,
      type: ContentType.Json,
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
   * @description Serialises the process-data recorder's current span — every cycle from the oldest still in the ring up to the newest at the instant of the call — to a `.mmpd` file and returns its path. Each cycle's full raw input and output images, sequence, and epoch-nanosecond timestamp are written, with the process image (per-device identity and PDO object layout, including object names and data types where the object dictionary has been enumerated) embedded as a header, so the file decodes fully offline with no running Motion Master and no live bus. Works in any state: while devices are exchanging (SAFE-OP/OP) it captures tail→head at that moment and ignores cycles recorded afterwards; after the bus has left the exchange states it uses the most recent retained image layout. The file is written under the configured `recorder.dumpDir`, created if absent; by default that is a `dumps` subdirectory of the user-cache root, so the written file is also reachable through the `/api/user-cache` endpoints — list it, download it, or delete it there. Only the path is returned here. (Setting an explicit `recorder.dumpDir` outside the cache root opts out of that; the file is then only reachable from a shell on the server.)
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
   * @name ListParameterCacheEntries
   * @summary List on-disk parameter caches
   * @request GET:/api/parameter-cache
   */
  listParameterCacheEntries = (params: RequestParams = {}) =>
    this.request<ParameterCacheEntry[], any>({
      path: `/api/parameter-cache`,
      method: "GET",
      format: "json",
      ...params,
    });
  /**
   * @description Returns the raw JSON cache file for the given id, verbatim, so it can be saved or inspected offline.
   *
   * @name GetParameterCache
   * @summary Download a parameter-cache file
   * @request GET:/api/parameter-cache/{id}
   */
  getParameterCache = (id: string, params: RequestParams = {}) =>
    this.request<object, void>({
      path: `/api/parameter-cache/${id}`,
      method: "GET",
      format: "json",
      ...params,
    });
  /**
   * @description Removes one cached file from disk. The next scan of that hardware re-enumerates it. Useful when a vendor reused a revision across an object-dictionary change.
   *
   * @name DeleteParameterCache
   * @summary Delete a parameter-cache file
   * @request DELETE:/api/parameter-cache/{id}
   */
  deleteParameterCache = (id: string, params: RequestParams = {}) =>
    this.request<void, void>({
      path: `/api/parameter-cache/${id}`,
      method: "DELETE",
      ...params,
    });
  /**
   * @description Returns every file in Motion Master's per-user cache directory, flattened: one entry per file with its full path relative to the cache root, whatever the nesting. Directories have no entries of their own. Independent of the bus — works whether or not a driver is initialised. The cache is a plain file store for data that should outlive a restart. The store itself neither validates nor interprets an upload — `PUT` accepts any bytes at any valid path, and putting a file here does not, on its own, make Motion Master do anything with it; the client chooses the paths. That is separate from whether a *feature* reads a given file, and several do: `parameters/` (the on-disk object-dictionary cache) and `dumps/` (`.mmpd` recorder dumps) are written and consumed by those features, with more to follow. Their files appear in this listing alongside everything else, so a client must not assume every entry was uploaded through this API, nor that deleting one is inconsequential.
   *
   * @name ListUserCacheFiles
   * @summary List the files in the user cache
   * @request GET:/api/user-cache
   */
  listUserCacheFiles = (params: RequestParams = {}) =>
    this.request<UserCacheListing, void>({
      path: `/api/user-cache`,
      method: "GET",
      format: "json",
      ...params,
    });
  /**
   * @description Returns the file verbatim, always as `application/octet-stream` and always with `Content-Disposition: attachment`, `X-Content-Type-Options: nosniff` and a `default-src 'none'; sandbox` CSP. The bytes are whatever a user uploaded, served from the API's own origin, so a browser must never *render* them — a rendered upload would be stored XSS against the origin that controls the drives, which CORS cannot help with. No extension is trusted to name a content type, and the attachment carries no `filename` (the path is user-controlled and must not reach a response header). Clients read the bytes and name the saved file themselves.
   *
   * @name GetUserCacheFile
   * @summary Download a user-cache file
   * @request GET:/api/user-cache/{path}
   */
  getUserCacheFile = (path: string, params: RequestParams = {}) =>
    this.request<Blob, void>({
      path: `/api/user-cache/${path}`,
      method: "GET",
      ...params,
    });
  /**
   * @description Writes the request body to the given path, creating parent directories as needed and replacing any existing file. The write is atomic — a concurrent reader sees either the old contents or the new one, never a partial file. The body is taken as raw bytes whatever its content type.
   *
   * @name PutUserCacheFile
   * @summary Upload a user-cache file
   * @request PUT:/api/user-cache/{path}
   */
  putUserCacheFile = (path: string, data: File, params: RequestParams = {}) =>
    this.request<
      {
        /**
         * The path written, relative to the cache root.
         * @example "configs/machine-a.json"
         */
        path: string;
        /**
         * Bytes written.
         * @example 20480
         */
        size: number;
      },
      void
    >({
      path: `/api/user-cache/${path}`,
      method: "PUT",
      body: data,
      format: "json",
      ...params,
    });
  /**
   * @description Removes a file, or a directory and everything under it. Directories left empty between the removed path and the cache root are pruned, so no husks remain.
   *
   * @name DeleteUserCacheFile
   * @summary Delete a user-cache file or directory
   * @request DELETE:/api/user-cache/{path}
   */
  deleteUserCacheFile = (path: string, params: RequestParams = {}) =>
    this.request<void, void>({
      path: `/api/user-cache/${path}`,
      method: "DELETE",
      ...params,
    });
}
