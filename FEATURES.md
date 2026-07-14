# Motion Master Features

Motion Master v6 is motion-control software for SOMANET servo drives over EtherCAT.
It runs as a local daemon exposing an HTTP API (`127.0.0.1:61447`) and a WebSocket
(`127.0.0.1:62281`), driven by a companion Progressive Web App console. This document
catalogs the features it provides today. The stable, built-in HTTP API is specified in
`apps/motion_master/swagger.yml`; architecture and design rationale live in `NEXTGEN.md`.

## Fieldbus / EtherCAT Bus Control

- **Driver lifecycle** — initialise the fieldbus driver on a chosen network adapter
  (`POST /api/init`), scan the bus for slaves (`POST /api/scan`), and reset/clear the
  device list (`POST /api/reset`). A pluggable `FieldbusDriver` interface abstracts the
  transport (SOEM raw-socket EtherCAT today; SPoE planned).
- **Network adapter discovery** — list the host's network adapters to choose the bus NIC
  (`GET /api/adapters`).
- **AL state control** — read the current EtherCAT AL state of devices and transition
  them (INIT / PRE-OP / SAFE-OP / OP / BOOT) via `GET`/`POST /api/devices/state`.
  Illegal AL transitions are rejected up front; entering exchange states reactively
  (re)maps the process image.
- **Partial-bus operations** — one or more devices can be taken to BOOT (firmware) or
  PRE-OP (re-map) while the rest keep exchanging process data; bringing them back
  re-maps the whole bus.
- **Static bus configuration inspection** — per-slave identity (vendor/product/revision/
  serial), Sync Manager, FMMU, Distributed Clock, mailbox windows, and advertised mailbox
  capability bytes (CoE/FoE/EoE/SoE details, decoded into flags in the UI)
  (`GET /api/bus-config`). Shown on the Configuration page; the SII page decodes the same
  capability bytes from the raw EEPROM image.
- **Live ESC health diagnostics** — per-slave EtherCAT Slave Controller error counters,
  port link state, and watchdog expirations (`GET /api/devices/diagnostics`).
- **Distributed-clock sync status** — live DC system-time deviation across devices
  (`GET /api/dc-sync`).

## Process Data (PDO) Exchange

- **Real-time cyclic exchange** — an RT game loop (`SCHED_FIFO`, 1 ms, `mlockall`)
  exchanges process data every cycle, lock-free on the PDO path.
- **Process image inspection** — inspect the published PDO layout and working-counter
  (WKC) health (`GET /api/process-image`).
- **Output staging** — stage a batch of output values into the process image lock-free
  (`POST /api/process-data/outputs`); the RT loop composes them into the wire image.
- **Lossless recorder** — a circular ring records every PDO cycle (raw input + output
  IOmap, epoch-ns timestamp, working counter), sized for a configurable capacity in cycles
  (default 300000 ≈ 5 min at 1 ms). It is the source for live monitoring, point reads, and dumps.
- **Recorder dump** — stream or write the recorder ring as a binary `.mmpd` file
  (`GET`/`POST /api/process-data/dump`), works in any state including OP.
- **PDO mapping** — read a device's cyclic mapping grouped by object, and rewrite it
  over CoE (`GET`/`PUT /api/devices/{slavePosition}/pdo-mapping`); writes reconfigure
  `0x1C12`/`0x1C13` + `0x16xx`/`0x1Axx` in PRE-OP.
- **Process-data watchdog** — read and set the sync-manager (process-data) watchdog
  timeout per device (`GET`/`PUT /api/devices/{slavePosition}/watchdog`).

## Object Dictionary / Parameters (CoE / SDO)

- **Parameter enumeration** — initialise a device's parameter list by enumerating its
  object dictionary (`POST /api/devices/{slavePosition}/parameters/init`). With
  `?readValues=true`, multi-subindex objects (ARRAY/RECORD) are read in one CoE Complete
  Access upload instead of one upload per subindex — far fewer mailbox round-trips, with a
  transparent per-subindex fallback on slaves that don't support it.
- **Bulk & single reads** — refresh cached values of all parameters
  (`POST .../parameters/read`), read the cached list (`GET .../parameters`), and read a
  single parameter, PDO-aware with optional cache-only mode
  (`GET .../parameters/{index}/{subindex}`). The bulk refresh reads PDO-mapped objects
  from the live process image and everything else over the mailbox — batching each
  multi-subindex object into one CoE Complete Access upload where supported, with a
  transparent per-subindex fallback.
- **Parameter writes** — write a single parameter, PDO-aware
  (`PUT .../parameters/{index}/{subindex}`).
- **Raw SDO access** — upload/download an object dictionary entry directly over CoE SDO
  (`GET`/`PUT /api/devices/{slavePosition}/sdo/{index}/{subindex}`).
- **On-disk parameter cache** — object-dictionary definitions are cached on disk keyed by
  device identity. List, download, and delete cache files (`/api/parameter-caches`).

## Device Services

- **File over EtherCAT (FoE)** — read and write files on a device (firmware, config)
  via FoE (`GET`/`PUT /api/devices/{slavePosition}/files/{filename}`).
- **ESC register access** — read and write raw bytes from/to an ESC register
  (`GET`/`PUT /api/devices/{slavePosition}/registers/{address}`).
- **SII / EEPROM** — read a device's SII image, write a raw SII image
  (`GET`/`PUT /api/devices/{slavePosition}/sii`), and parse a raw SII image offline
  (`POST /api/sii/parse`).
- **Device listing** — list all devices or fetch one by bus position
  (`GET /api/devices`, `GET /api/devices/{slavePosition}`).

## CiA402 / SOMANET Motion Profiles

- **Profile views** — a borrowed-view chain `ProfileDevice ← Cia402Drive ← SomanetDrive`
  binds a `Device` for one operation via validated factories, providing CiA402 state
  machine and SOMANET-specific object-dictionary access.
- **Sine-wave / target generators** *(planned)* — RT cyclic target generators launched
  from the profile view (`Cia402Drive::startSineWave(...)`), gated active/idle by a
  per-device control block.

## Monitoring (Live Telemetry)

- **Monitorings** — create, list, get, and delete monitorings
  (`/api/monitorings`, `/api/monitorings/{topic}`). Each streams selected PDO- and
  SDO-sourced parameters.
- **Lossless WebSocket stream** — every recorded cycle is shipped as positional cycle
  rows over the WebSocket (`{"type":"monitoring", ...}`); `interval` is the flush cadence
  (5–2000 ms), not a sample rate, so no cycle is dropped. Up to 5 simultaneous clients.
- **Notifications** — bus/state change events pushed over the WebSocket
  (`{"type":"notification", ...}`).

## Diagnostics & Reference Metadata

- **Log access** — retrieve recent server log output (`GET /api/log`).
- **Reference tables** — AL status codes, ESC registers, FoE error codes, and CoE data
  types (`GET /api/meta/*`).
- **System & version info** — Motion Master version (`GET /api/version`), startup
  configuration (`GET /api/config`), and host OS/hardware info (`GET /api/system-info`).

## Web Console (PWA)

A React PWA at `https://motion-master.synapticon.com` provides UI for the above:

- **Connection** — driver init, adapter selection, TLS certificate status.
- **Fieldbus** — overview, EtherCAT State control, Bus Configuration, Process Image,
  Bus Diagnostics, DC Sync.
- **Per-device** — Parameters, Object Dictionary, PDO Mapping, Registers, SII, FoE.
- **Process Data / Recorder** — live process-data view and recorder page.
- **Monitorings** — configure and plot live telemetry.
- **Tools & reference** — SII parser, AL Status Codes, ESC Registers, FoE Error Codes,
  Data Types, HTTP request inspector, Log, Parameter Caches, and bundled API Docs.

## Security & Deployment

- **TLS** — serves HTTPS/WSS with a bundled Let's Encrypt certificate for
  `local.motion-master.synapticon.com` (pins to `127.0.0.1`); CORS locked to the PWA
  origin.
- **Certificate self-heal** — the binary fetches, validates, and atomically installs a
  fresh certificate at startup when the current one is missing/expired
  (`--update-cert`, `--no-cert-update`); `GET /api/cert` and `POST /api/cert/refresh`
  expose status and on-demand refresh.
- **Packaging** — ships as Linux `.deb`/`.rpm`/`.tar.gz`, Windows `.zip`, and macOS
  arm64 `.tar.gz`, plus a Docker image; all install to `/opt/motion-master/`.
- **Extensibility** — C++ route plug-ins extend the HTTP API under `/api/<yourapp>/...`
  without touching the core server (`libs/example` is the starter template).

## Configuration

- Settings are supplied via an optional JSONC config file (comments allowed); all
  settings have code defaults, and the `Config` struct mirrors the file one-to-one.
