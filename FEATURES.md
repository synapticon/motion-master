# Motion Master Features

Motion Master v6 is motion-control software for SOMANET servo drives over EtherCAT.
It runs as a local daemon exposing an HTTP API (`127.0.0.1:61447`) and a WebSocket
(`127.0.0.1:62281`), driven by a companion Progressive Web App console. This document
catalogs the features it provides today. The stable, built-in HTTP API is specified in
`apps/motion_master/swagger.yml` — and served by the running binary itself at
`GET /api/swagger.yml`. Installation, configuration, and client usage are covered in
[`README.md`](./README.md); architecture and design rationale live in `NEXTGEN.md`.

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

- **Real-time cyclic exchange** — an RT game loop (`SCHED_FIFO`, `mlockall`) exchanges
  process data every cycle, lock-free on the PDO path. The default period is 1 ms on Linux
  and macOS; the Windows release ships 4 ms, which stock Windows timers can actually sustain.
  A missed deadline skips to the next grid point rather than replaying stale frames, so the
  loop degrades predictably instead of drifting.
- **Game-loop health & runtime retiming** — inspect a live snapshot of the RT loop
  (configured/achieved rate, executed/skipped-cycle counters, per-cycle task timing,
  whether RT scheduling was acquired) via `GET /api/game-loop`, and retime the running
  loop to a new cycle period with `PUT /api/game-loop`. The retime takes effect within
  one cycle, is transient (not written back to config), and starts a fresh health epoch;
  the recorder ring is period-independent, so nothing else is resized.
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
  (`GET`/`POST /api/devices/{slavePosition}/registers/{address}`).
- **Parameter persistence** — saving parameters to non-volatile memory (`0x1010`) and
  restoring a group of defaults (`0x1011`) are **procedures**, not requests of their own: both
  write a CANopen signature and then poll the device until it confirms, which takes seconds.
  See *Procedures* below.
- **SII / EEPROM** — read a device's SII image, write a raw SII image
  (`GET`/`PUT /api/devices/{slavePosition}/sii`), and parse a raw SII image offline
  (`POST /api/sii/parse`).
- **Device listing** — list all devices or fetch one by bus position
  (`GET /api/devices`, `GET /api/devices/{slavePosition}`).

## CiA402 / SOMANET Motion Profiles

- **Drive control snapshot** — read a drive's decoded CiA402 state machine state together
  with the raw statusword (`0x6041`), controlword (`0x6040`), and the active operation mode
  (`0x6061`) via `GET /api/devices/{slavePosition}/cia402`. Values are read live — from the
  process image while the device is exchanging, over SDO otherwise — so polling this drives
  a live UI.
- **State machine commands** — `POST /api/devices/{slavePosition}/cia402/command` runs
  `enable` (walking every intermediate transition to Operation Enabled, clearing a latched
  fault first if needed), `disable` (to Switch On Disabled), `quickStop`, or `faultReset`.
- **Operation mode** — set the mode of operation (`0x6060`) with
  `POST /api/devices/{slavePosition}/cia402/mode`; the drive reflects the accepted mode in
  `0x6061` once it takes effect.
- **Cyclic setpoints** — `POST /api/devices/{slavePosition}/cia402/target` writes the one
  setpoint matching the active mode: target position (`0x607A`) in PP/CSP, target velocity
  (`0x60FF`) in PV/CSV, or target torque (`0x6071`, per-mille of rated) in PT/CST. All are
  signed, so negative values command reverse motion or regenerative torque.
- **Profile views** — internally, a borrowed-view chain `ProfileDevice ← Cia402Drive ←
  SomanetDrive` binds a `Device` for one operation via validated factories, providing the
  CiA402 state machine and SOMANET-specific object-dictionary access behind the endpoints
  above.
- **Trajectory playback** *(planned)* — an RT cyclic task plays back a precomputed setpoint
  buffer one point per cycle (sine/chirp/ramp/step are userspace-generated buffers plus a
  `repeat` flag, not separate tasks). Launched off the RT thread by a node-layer function
  that validates the request, does the op-mode and enable handshake through a `Cia402Drive`
  view, and arms an immutable run into a depth-1 latest-wins mailbox slot the task reads —
  one slot per axis, so a single-axis move and a coordinated multi-axis program share one
  mechanism. Skips are absorbed by a per-trajectory policy (preserve shape, or preserve
  wall-clock timing).

## Procedures

- **One shape for every operation that takes time** — `POST`/`GET`/`DELETE` on
  `/api/devices/{slavePosition}/procedures/{procedureName}` start a run, report how it is
  going, and cancel it. A run happens on its own thread, so the API stays responsive and the
  drive keeps cycling; one procedure runs per device at a time. Progress is **polled, not
  streamed**: each `GET` returns the whole accumulated run, so a step that starts and finishes
  between two polls is still reported with its result, and the result is retained after the
  run ends.
- **The catalogue is served, not hard-coded in clients** — `GET
  /api/devices/{slavePosition}/procedures` lists what a device supports, each entry carrying
  its title, description, caveats, whether it can move the shaft, whether the drive must be
  enabled, the steps it reports against, and the **parameters it accepts** (name, type,
  default, bounds or choices, and whether it is required). That is enough to render a control
  and a form for any procedure without knowing it by name. The list is per device: a
  procedure is offered only where it applies.
- **Generic CANopen procedures**, offered on any device with a CoE mailbox: **store
  parameters** (`0x1010`) and **restore default parameters** (`0x1011`, with a `group`
  parameter selecting all / communication / application / manufacturer).
- **SOMANET procedures** — the raw **OS command** (`0x1023`/`0x1024`, request bytes passed
  through as given), **encoder register communication** (read or write one BiSS encoder
  register), the motor measurements (**open phase detection**, **pole pair detection**,
  **phase resistance**, **phase inductance**), **motor phase order detection**, **commutation
  offset detection**, and **offset detection** — the whole commissioning sequence in one
  prepared session. Each prepares the drive itself (diagnostics mode, Operation Enabled, and
  the brake where its command requires it) and restores everything on every path out,
  including a failure or a cancellation.

## Monitoring (Live Telemetry)

- **Monitorings** — create, list, get, and delete monitorings
  (`/api/monitorings`, `/api/monitorings/{topic}`). Each streams selected PDO- and
  SDO-sourced parameters.
- **Lossless WebSocket stream** — every recorded cycle is shipped as positional cycle
  rows over the WebSocket (`{"type":"monitoring", ...}`); `interval` is the flush cadence
  (5–2000 ms), not a sample rate, so no cycle is dropped. Throughput is sized for roughly 5
  simultaneous clients — a capacity budget, not an enforced limit; there is no connection cap.
- **Notifications** — bus/state change events pushed over the WebSocket
  (`{"type":"notification", ...}`).

## Diagnostics & Reference Metadata

- **Log access** — retrieve recent server log output (`GET /api/log`).
- **Reference tables** — AL status codes, ESC registers, FoE error codes, mailbox error
  codes, SDO abort codes, and CoE object data types (`GET /api/meta/*`), each with a
  matching console page.
- **System & version info** — Motion Master version (`GET /api/version`), startup
  configuration (`GET /api/config`), and host OS/hardware info (`GET /api/system-info`).
- **Self-describing API** — the server serves its own OpenAPI spec at
  `GET /api/swagger.yml`, so a client can resolve the contract from the very instance it is
  talking to instead of pinning a copy. The console renders it as browsable API Docs, and
  the reference Python client resolves every call against it at startup.

## Web Console (PWA)

A React PWA at `https://motion-master.synapticon.com` provides UI for the above:

- **Connection** — driver init, adapter selection, TLS certificate status.
- **Fieldbus** — overview, EtherCAT State control, Bus Configuration, Process Image,
  Bus Diagnostics, DC Sync.
- **Per-device** — Motion (drive the CiA402 state machine, operation mode, and cyclic
  setpoints, watching target vs. actual live — shown only for a CiA402 device in OP, since
  both are required to command motion), EtherCAT State, Parameters, Object Dictionary,
  PDO Mapping, Registers, SII, FoE.
- **Process Data / Recorder** — live process-data view and recorder page.
- **Game Loop** — RT loop health and runtime cycle-period control.
- **Monitorings** — configure and plot live telemetry.
- **Tools & reference** — SII parser, AL Status Codes, ESC Registers, FoE Error Codes,
  Mailbox Error Codes, SDO Abort Codes, Data Types, HTTP request inspector, Log, Parameter
  Caches, and bundled API Docs.

## Security & Deployment

- **TLS** — serves HTTPS/WSS with a bundled Let's Encrypt certificate for
  `local.motion-master.synapticon.com` (pins to `127.0.0.1`); CORS locked to the PWA
  origin.
- **Certificate self-heal** — the binary fetches, validates, and atomically installs a
  fresh certificate at startup when the current one is missing, expired, or expiring within
  7 days. `--update-cert` does it on demand from the terminal and exits, `--cert-url` /
  `--key-url` override the source, and `tls.autoUpdate: false` in the config opts out
  entirely for air-gapped installs. `GET /api/cert` and `POST /api/cert/refresh` expose
  status and on-demand refresh to the UI.
- **Packaging** — ships as Linux `.deb`/`.rpm`/`.tar.gz`, Windows `.zip`, and macOS
  arm64 `.tar.gz`, plus a Docker image. The packages and the image install to
  `/opt/motion-master/` (the deb/rpm also symlink `/usr/local/bin/motion-master`); the zip
  and tarballs are self-contained and run from wherever they are extracted. The Windows
  executable is Authenticode code-signed.
- **Extensibility** — C++ route plug-ins extend the HTTP API under `/api/<yourapp>/...`
  without touching the core server (`libs/example` is the starter template).

## Configuration

- **Config file only, always optional** — every setting lives in a JSONC file (comments
  allowed); there are no CLI flags for settings, and each key falls back to a built-in
  default, so the binary runs with no config at all. The `Config` struct mirrors the file
  one-to-one.
- **Two load locations** — an explicit `--config <path>` always wins; absent that, a
  `motion-master.jsonc` sitting next to the executable is auto-discovered. There is no
  system-wide search path. This is what lets the Windows release ship a config presetting a
  4 ms cycle out of the box.
- **What is configurable** — HTTP and WebSocket ports plus the CORS origin (`server`), the
  fieldbus driver and adapter to bring up at startup (`fieldbus`), log verbosity
  (`logLevel`), the RT cycle period (`gameLoop`), recorder ring depth and dump directory
  (`recorder`), the on-disk parameter-definition cache (`parameterCache`), object-dictionary
  read behaviour (`parameters`), and TLS paths plus cert auto-update (`tls`). See
  [`README.md`](./README.md#configuration) for the annotated block reference, and
  `apps/motion_master/motion-master.example.jsonc` for every key with its default.
