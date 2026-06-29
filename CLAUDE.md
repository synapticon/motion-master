# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Context

**Motion Master v6.0.0** — next-generation motion control software for SOMANET servo drives. This project is a clean-sheet rewrite of the previous `motion_master` codebase; the design rationale and session notes live in `NEXTGEN.md`. Read that file for architectural decisions, class diagrams, and design rationale before making structural changes.

Key design mandates from NEXTGEN.md:
- No exceptions — use `std::expected<T, std::string>` (C++23 stdlib, no `tl::expected`); error strings are used throughout for now — structured error types are a future improvement once it is clear which callers need to branch on error kind
- HTTP API (port 61447) + a single WebSocket on its own port (62281) and event loop — `HttpServer` and `WebSocketServer` are distinct, each with its own uWS app/loop/thread, so a slow or blocking HTTP handler (FoE, SDO, cert fetch) can never stall the WebSocket. The WebSocket is the bidirectional connection: server→client monitoring batches, notifications, and procedure progress; client→server topic subscribe/unsubscribe and (planned) process-data output staging. No Protobuf. (The two-port split supersedes the original single-port mandate, which was a reaction to the old `motion_master`'s ZeroMQ request + pub/sub channels; a second TLS port for the same WebSocket is a far milder thing, and the isolation is worth it.)
- Single `Device` abstraction (replaces `VirtualDevice` + `comm::base::Device` overlap from the old codebase)
- `FieldbusDriver` interface abstracts SOEM and SPoE — `SoemFieldbusDriver` and `SpoeDriver` are the concrete implementations; `FieldbusDriver` owns `socketMutex_`, which serializes the **control-plane** operations (mailbox/SDO, FoE, ESC register, state access) amongst non-RT callers. The **PDO path (`exchangeProcessData`) runs lock-free** — SOEM's port layer is internally thread-safe (per-datagram index allocation + tx/rx mutexes held only for a single non-blocking poll, with cooperative frame demux) and PDO touches disjoint state (the process-data IOmap) from the control plane, so the RT cycle is never blocked by a slow SDO or object-dictionary enumeration. The lock is held for one socket transaction only — never across a sleep, a blocking wait, or a user callback (so `readObjectDictionary` and `transitionToState` lock per transaction, not for their whole multi-second duration)
- `DeviceManager` owns `FieldbusDriver` via `unique_ptr<FieldbusDriver>` (null until `init()` is called) — the driver is constructed by `main.cc` and transferred via `DeviceManager::init(unique_ptr<FieldbusDriver>)`; `DeviceManager` never references concrete driver types
- No service layer — SDO read/write, file transfer, state control, and bus inspection (static SM/FMMU/DC config, process-image layout, and live per-slave ESC link diagnostics — error counters, port link state, watchdog expirations) are methods on `Device` and `DeviceManager`; `HttpServer` and `GameLoop` both take a `DeviceManager&` directly
- `GameLoop` calls `deviceManager_.exchangeProcessData()` — it has no knowledge of `FieldbusDriver`; `exchangeProcessData()` is a no-op when the driver is null so the loop always starts unconditionally
- `DeviceManager` owns slave discovery and network scanning via `FieldbusDriver` — there is no separate `NetworkScanner`
- `App` is the only place that instantiates concrete types (dependency injection at the composition root); `Server::Config` carries an `InitDriverFn` callback wired in `main.cc` so `POST /api/init` can create a driver without the server knowing concrete types
- Namespaces mirror directory layout (`mm::core`, `mm::comm::soem`, `mm::node`, `mm::api`); do not use C++20 modules
- C++ route plug-ins extend the HTTP API without touching `http_server.cc`. The transport glue lives in `mm::api` (`libs/api/web_api.h`): `RouteContext` (references to `DeviceManager`/`MonitoringManager` + `corsOrigin`), `RegisterRoutesFn`, and the `sendJson`/`sendError`/`sendStatus` helpers — this is the **only** layer that depends on uWebSockets, so `mm::node` stays transport-agnostic (do **not** add uWS/HTTP to `node`). A plug-in lib registers its own paths (`/api/yourapp/...`, never the `/api/*` or `/*` wildcards) via `HttpServer::addRoutes(fn)`, called from the composition root (`main.cc`) **before** `start()`; modules run once on the HTTP loop thread after the built-in routes and before the catch-all 404. `libs/example` (`mm::example`, `GET /api/example/devices`) is the copy-me starter — domain logic in `*_logic.{h,cc}` (HTTP-agnostic, unit-testable), formatting in `*_routes.cc`. Plug-in routes are **not** added to `swagger.yml` (that documents the stable built-in API only). See NEXTGEN.md, Session 2026-06-29.
- Config file format is JSONC — parse via `nlohmann::json::parse(stream, nullptr, true, true)` (the fourth `true` enables `ignore_comments`); config files use the `.jsonc` extension and may freely use `//` and `/* */` comments

## Build System

CMake 4.0+ with Ninja and vcpkg. Initialize the vcpkg submodule before the first build:

```bash
git submodule update --init --recursive
```

### Scripts

All common tasks have wrapper scripts in `tools/`. They default to the `x64-linux-debug` preset; pass a preset name as the first argument to override.

```bash
./tools/configure.sh              # cmake --preset
./tools/build.sh                  # cmake --build --preset (no setcap by default — needs no sudo)
./tools/build.sh --setcap         # build, then sudo setcap for raw socket + RT access
./tools/build-dev.sh              # build (with --setcap) + run the test suite; --no-setcap skips sudo
./tools/run.sh                    # generate a tmp self-signed cert and run the binary
./tools/test.sh                   # ctest --output-on-failure
./tools/format.sh                 # clang-format all sources
./tools/lint.sh                   # cpplint (requires: pip install cpplint)
./tools/cppcheck.sh               # cppcheck static analysis
./tools/check.sh                  # format + cppcheck + lint in sequence
./tools/clean.sh                  # remove build/<preset>
./tools/package.sh [preset]       # build .deb and .rpm packages (cert.pem/key.pem must be in build dir)

# Use a different preset:
./tools/configure.sh x64-linux-release
./tools/build.sh x64-linux-release
```

### Raw CMake commands

```bash
cmake --preset x64-linux-debug
cmake --build --preset x64-linux-debug
ctest --test-dir build/x64-linux-debug --output-on-failure
```

Available presets: `x64-linux-debug`, `x64-linux-release`, `x64-windows-debug`, `x64-windows-release`.

Build output goes to `build/<preset>/`. Compiler requirements: C++23, warnings as errors (`-Wall -Wextra -Wpedantic -Werror` on GCC/Clang; `/W4 /WX` on MSVC).

## Versioning

`VERSION` (repo root) is the single source of truth. CMake reads it and propagates the value into the generated `libs/core/version.h` and `Doxyfile` — do not edit those files directly. All other secondary locations are kept in sync by the bump script:

```bash
./tools/bump-version.sh 6.0.0-alpha.1
```

Files updated by the script: `VERSION`, `vcpkg.json`, the root workspace `package.json`, `web/apps/console/package.json`, `web/apps/example/package.json`, `web/packages/motion-master-client/package.json`, `web/packages/ui/package.json`, `hil/api/package.json`, `apps/motion_master/swagger.yml`, the `StringConstant` assertion in `libs/core/tests/version_test.cc`, and the sidebar badge in `web/apps/console/src/layouts/RootLayout.tsx`.

The generated HTTP API client (`web/packages/motion-master-client/src/generated/`) is produced from `swagger.yml` via `swagger-typescript-api` (`pnpm --filter @synapticon/motion-master-client generate`) and is **committed to the repo** — regenerate and commit it whenever the API shape changes. The `api-client-drift` job in `lint.yml` regenerates from `swagger.yml` and fails on any diff, so a stale committed client is caught in CI.

After bumping, commit all changed files, then push a `v<version>` tag to trigger `release.yml` — which builds the binaries **and** publishes `@synapticon/motion-master-client@<version>` to npm (prereleases under the `next` dist-tag; needs the `NPM_TOKEN` repo secret).

## CI

Four per-platform build workflows run on every push/PR — `build-linux-x64.yml`, `build-linux-arm64.yml`, `build-macos-arm64.yml`, `build-windows-x64.yml` (display names `Build & Test (Linux x64)` etc.). They cache vcpkg binaries with `actions/cache@v5` on the platform's archive dir (`~/.cache/vcpkg/archives`, or `~/AppData/Local/vcpkg/archives` on Windows), keyed on OS + `vcpkg.json` hash. The `x-gha` vcpkg binary caching backend was **removed** in the pinned vcpkg version (`56bb241`) — do not use `VCPKG_BINARY_SOURCES: "clear;x-gha,readwrite"` or the `actions/github-script` workaround. The Windows workflow splits the cache into `actions/cache/restore` + `actions/cache/save` (`if: always()`) so a failed build still persists the (slow) dependency cache.

Three further workflows exist alongside the build set:

- **`cert-renewal.yml`** — runs on the 1st of every month. Uses `acme.sh` with the `dns_acmedns` plugin against `auth.acme-dns.io` to issue a fresh Let's Encrypt cert for `local.motion-master.synapticon.com` (DNS-01 via CNAME delegation — no manual DNS touch required after the one-time setup). Stores the renewed cert and key as GitHub Secrets `TLS_CERT` and `TLS_KEY`. Requires `ACMEDNS_CONFIG` (acme-dns credentials JSON) and `GH_PAT_SECRETS` (fine-grained PAT with Secrets read/write on this repo) to be set as repository secrets.
- **`release.yml`** — triggered by `v*` tags. Three parallel build legs (`linux` on `ubuntu-24.04`, `windows` on `windows-latest`, `macos` on `macos-14`) each read `TLS_CERT`/`TLS_KEY` from secrets and write them as `cert.pem`/`key.pem` into the build output; a final `release` job downloads every leg's artifacts and publishes one GitHub Release. The linux leg runs `tools/package.sh` (needs `dpkg-dev` + `rpm`); the windows leg zips the exe with its vcpkg runtime DLLs (the `x64-windows` triplet is dynamic). Five artefacts: `motion-master-<version>-linux-x64.tar.gz`, `-amd64.deb`, `-x86_64.rpm`, `-windows-x64.zip`, and `-macos-arm64.tar.gz`. All packages install to `/opt/motion-master/`; `cert.pem` and `key.pem` are marked as conffiles (deb) / `%config(noreplace)` (rpm) so upgrades never silently overwrite them. On deb, `apt remove` leaves conffiles behind — `apt purge` is required for a full uninstall. On rpm, `dnf remove` removes unmodified config files automatically; modified ones are saved as `.rpmsave`. The release legs ship **x64/amd64 Linux** only — `build-linux-arm64.yml` builds an arm64 *binary* in CI but there is no arm64 `.deb` or Raspberry Pi image leg yet; both are a prerequisite for the planned Pi appliance (NEXTGEN.md, Session 2026-06-12).
- **`lint.yml`** — runs clang-format and cpplint checks on every push and PR.

## Docker

The `Dockerfile` is a two-stage build (build on `ubuntu:24.04`, minimal runtime image). The binary lands in `/opt/motion-master/` — consistent with the deb/rpm install path. `docker-entrypoint.sh` mirrors the cert discovery order of `tools/run.sh`: CERT/KEY env vars → bundled cert baked into the image → acme.sh mount → self-signed fallback.

**Capabilities** — Docker drops most Linux capabilities by default. On bare-metal `setcap` stamps the binary so file capabilities are granted automatically; inside a container file capabilities are ignored and `--cap-add` is used instead:

| Capability | Purpose |
|---|---|
| `CAP_NET_RAW` + `CAP_NET_ADMIN` | SOEM EtherCAT raw sockets and NIC promiscuous mode |
| `CAP_SYS_NICE` | `SCHED_FIFO` RT scheduling on the game loop thread |
| `CAP_IPC_LOCK` + `--ulimit memlock=-1` | `mlockall()` to pin process memory for RT |

Missing RT caps produce a warning and the loop runs non-RT. Missing EtherCAT caps cause `POST /api/init` to fail when a SOEM driver is requested. `--privileged` also works but grants far more than necessary.

**Cert baking** — release CI places `cert.pem`/`key.pem` at the repo root before `docker build` so they are baked into the image. Developer builds without certs get empty placeholder files; the entrypoint detects them (non-empty `-s` check) and falls back to acme.sh or self-signed. Users can override expired baked-in certs at runtime by mounting new ones over `/opt/motion-master/cert.pem` and `/opt/motion-master/key.pem` — the volume mount shadows the image file.

## Architecture

### Directory Layout

```
motion-master/
  apps/
    motion_master/     ← main executable (flat file layout); swagger.yml here is the OpenAPI spec — source for the PWA's bundled API Docs + generated API clients, not shipped with the binary
    playground/        ← scratch binary
  libs/
    core/              ← version, platform timers, cross-cutting utils
    comm/              ← fieldbus interfaces; soem.cc, spoe.cc, igh.cc alongside base
    node/              ← Device, DeviceManager, CiA402, profiles (depends on mm::comm); transport-agnostic — no HTTP/uWS
    api/               ← mm::api: HTTP-transport glue (web_api.h — RouteContext, RegisterRoutesFn, sendJson/Error/Status). Header-only; the only lib that knows uWebSockets
    example/           ← mm::example: copy-me C++ route-plugin starter (/api/example/...); server-side analogue of web/apps/example
  hil/
    jitter_bench/      ← RT scheduling jitter benchmark (Linux only); CSV output + Python plot script
    api/               ← HTTP API + WebSocket integration tests (TypeScript / Vitest; Docker-managed)
  cmake/
    lint.cmake         ← lint, cppcheck, format CMake targets
  packaging/
    postinst           ← deb postinst script; sets capabilities on /opt/motion-master/motion-master (rpm uses an equivalent %post scriptlet inlined in tools/package.sh)
  extern/
    vcpkg/             ← git submodule
  tools/               ← developer scripts
```

Flat layout within each lib/app is intentional — navigate by filename and grep, not nested folders.

### Class Structure (from NEXTGEN.md)

```
main.cc  (composition root — the only place concrete types are instantiated; no `App` class yet)
 ├── Config (CLI options)
 ├── mm::node::DeviceManager      (owns FieldbusDriver + Device[] + ProcessData + ParameterCache; drives scanning)
 │     ├── unique_ptr<FieldbusDriver>   ← SoemFieldbusDriver | SpoeDriver (planned); owns socketMutex_
 │     │                                  null until init(); set via init(unique_ptr<FieldbusDriver>)
 │     ├── unique_ptr<ProcessData>      (published image + generations; per-output atomic staging
 │     │                                  slots; lossless recorder ring (every cycle); WKC health.
 │     │                                  Owned here, handed by raw pointer to each Device)
 │     ├── ParameterCache               (control-plane only; on-disk OD-definition cache keyed by
 │     │                                  identity. Direct member, created once + never replaced, so
 │     │                                  the pointer is stable across scans; handed by raw pointer
 │     │                                  to each Device. Best-effort file I/O; touched only on the
 │     │                                  HTTP/scan threads — outside the RT path entirely)
 │     ├── owns: std::vector<Device>    (each Device borrows FieldbusDriver& + ProcessData* + ParameterCache*)
 │     │     ├── slavePosition, name, vendorId, productCode, revisionNumber, serialNumber (immutable)
 │     │     ├── owns: parameters_  (index/subindex → DeviceParameter{ DeviceParameterValue variant })
 │     │     ├── owns: PdoMappings
 │     │     └── parametersMutex_  (guards parameters_ vs the off-RT monitoring threads)
 │     └── init(), scan(), reset(), configureProcessData(), exchangeProcessData(), transitionToState()
 ├── GameLoop  (RT thread, SCHED_FIFO, 1 ms; the main thread blocks here)
 │     └── runs: CyclicTask[]  (fixed membership — all registered before run())
 │           └── ProcessDataTask → DeviceManager::exchangeProcessData()  (no-op until image published)
 │           └── [planned: SineWaveTask + other RT target generators, idle until activated]
 ├── HttpServer  (own port 61447 + loop/thread)
 │     ├── uses: DeviceManager      (SDO read/write, FoE, state control, bus inspection)
 │     ├── uses: MonitoringManager  (/api/monitorings routes)
 │     └── Config.InitDriverFn      (callback to main.cc; creates concrete driver for POST /api/init)
 ├── WebSocketServer  (own port 62281 + loop/thread; WebSocket connection — monitoring batches out,
 │                     subscribe in; notifications / procedure progress / output-staging in as they land)
 └── MonitoringManager  (off-RT; owns the monitoring registry, turns each into a lossless row stream)
       ├── owns: ParameterRefresher  (background thread polling SDO-only params into a cache)
       ├── owns: sampler thread        (per flush, ships every recorded cycle since each monitoring's
       │                                read cursor — reads the recorder ring, never the bus)
       └── setPublish(cb)              → WebSocketServer::publish(topic, json)

 [planned, not yet in code: NotificationBus (observer decoupling producers from servers),
  FirmwareInstaller (off-RT std::jthread procedure, modelled on MonitoringManager's threads)]
```

**Device profile views are borrowed, not owned by `Device`.** The CiA402 / SOMANET behaviour lives in a
shallow inheritance chain of *views* — `ProfileDevice ← Cia402Drive ← SomanetDrive` — each holding only a
`Device&` (no `Cia402StateMachine` member on `Device`; that 2026-05-16 model was superseded). A view binds
a device for the duration of one operation and is constructed via the validated factories
`createCia402Drive(Device&)` / `createSomanetDrive(Device&)`. A view must never outlive its `Device&` or be
cached across a rescan — long-running RT procedures re-resolve their `Device` via `DeviceManager::findDevice`
each cycle.

### Key Types

```cpp
using DeviceParameterValue = std::variant<
  int8_t, int16_t, int32_t, int64_t,
  uint8_t, uint16_t, uint32_t, uint64_t,
  float, double, std::string, std::vector<uint8_t>
>;
```

Use `std::visit` for type dispatch on `DeviceParameterValue`. `DeviceParameter` holds index, subindex, and a value.

### Game Loop / RT Threading

`GameLoop::run()` blocks the **main thread** — this IS the RT thread. All other subsystems start their own threads before `run()` is called. Shutdown via signal sets an atomic flag checked after each cycle.

`GameLoop` calls `deviceManager_.exchangeProcessData()` each cycle via a `ProcessDataTask` (a `CyclicTask` adapter, so `GameLoop` has no knowledge of `DeviceManager` internals or `FieldbusDriver`). It is a no-op until a process image is published, so the loop runs unconditionally. HTTP handlers call SDO methods on `DeviceManager`/`Device` from their own threads; `FieldbusDriver` serializes all socket access via its internal mutex.

PDO data crosses the RT/non-RT boundary through `ProcessData` (`libs/node/process_data.h`, owned by `DeviceManager`, pointer handed to each `Device`):

- **Inputs and the output read-back come from the recorder ring** (`ProcessDataRing`, `libs/node/process_data_ring.{h,cc}`) — a lock-free circular recorder the RT loop appends one record to **every cycle** (raw input + output IOmap, epoch-ns timestamp, working counter). It is the single RT-written structure and the source for the live monitoring stream, point reads of the freshest value (`head()-1`), and the `.mmpd` dump (`DeviceManager::dumpProcessData` / `POST /api/process-data/dump` — serializes the current `[oldestValidSeq, head)` span plus the process image as a header via `libs/node/process_data_dump.{h,cc}`, works in any state including OP); there are no separate whole-image snapshots. The RT `write()` is wait-free (a few `memcpy` + a per-slot release-stored absolute sequence number); readers re-check that sequence after copying to detect a write that raced the copy, which at a seconds-deep ring is effectively never. Allocated + `mlock`'d at `configureProcessData` for `recorder.historySeconds × (1e6/periodUs)` cycles (default 300 s ≈ 120 MB), re-allocated on a layout-changing re-map (records under the old layout are undecodable), retained across image teardown, freed only by `reset()`/`scan()`.
- **Outputs are staged lock-free.** Each output object owns its own `std::atomic<uint64_t>` slot in `outputSlots` (≤8 wire bytes packed little-endian). Any number of non-RT writers store into *different* objects' slots without contending; same-object writes are last-writer-wins. The RT loop is the only thread that *composes* those slots into the packed wire image each cycle, which is what makes bit-packed objects sharing a byte safe without a lock (NEXTGEN.md "Design B").

An atomically-published `image` pointer (with retained `generations`) gates the whole thing: readers load it lock-free and `readPdo` falls back to SDO when no image is published, nothing has been recorded yet (`head()==0`), or (inputs only) the bus is unhealthy (`lastWkc < expectedWkc`).

**Monitoring runs off the RT loop and is lossless.** It is *not* a `CyclicTask`. `MonitoringManager` owns a background sampler thread; each monitoring holds a **read cursor** into the recorder ring, and on each flush ships **every** cycle recorded in `[cursor, head)` as one batch, then advances the cursor — so no cycle is dropped (`interval` is the flush *cadence*, not a sample rate; bounded 5–2000 ms). A cursor lapped by more than a whole ring is logged (not notified) and resynced to the oldest record. PDO-mapped parameters are decoded from each cycle's ring record; SDO-only parameters are polled in the background by the owned `ParameterRefresher` and read from its cache (one cached value per flush). Row timestamps on the wire are **epoch microseconds** (JS-exact, distinct per sub-ms cycle). `Device::parametersMutex_` guards the parameter map against these off-RT threads racing the control plane.

**`CyclicTask` membership is fixed.** All tasks are registered before `run()`; `GameLoop` never adds or removes tasks at runtime (the design rationale is in NEXTGEN.md, session 2026-06-05 — *RT tasks are fixed-membership*). Today only `ProcessDataTask` is registered (`main.cc`). This fixed-membership rule splits runtime procedures into two kinds:
- **RT cyclic procedures** *(planned — not yet in code)*: SineWave / profile / ramp generators — anything that must write a target into the output region every cycle — will be `CyclicTask`s registered up front and *idle until activated* via a control block, exactly like `ProcessDataTask` is a no-op until an image is published. The intended control block is a per-device lock-free published `SineWaveParams` (held on `Device` as a `unique_ptr`, like `parametersMutex_`); one slot per device *is* the fixed pool. **The launch lives on the view, not the HTTP handler or scheduler:** `Cia402Drive::startSineWave(...) → expected<>` validates, does the op-mode handshake synchronously off-RT, then stores the params with `active = true` — with fixed membership a "launch" is just a control-block write, a synchronous single-device state change. The single `SineWaveTask` (holds `DeviceManager&`) iterates devices each cycle, runs after `ProcessDataTask`, and writes the output slots directly (on-RT). RT-only scratch (phase, center, edge flag) stays off the published block; all validation is on the launcher's `expected<>` path so the RT side has no error branch.
- **Off-RT procedures** (commutation/offset detection, auto-tuning, firmware — call a command and wait, not cycle-time-sensitive) are **not** `CyclicTask`s. They run on a cancellable background `std::jthread` calling `DeviceManager`/`Device` methods (serialized on the fieldbus mutex). The built precedent for an off-RT background thread is `MonitoringManager`'s sampler/refresher; a dedicated `FirmwareInstaller` of this shape is planned.

**Reactive mapping.** Changing AL states is the user's job (via `POST /api/state`); Motion Master *reacts* in `DeviceManager::transitionToState`. The process image is a single whole-bus layout (`ecx_config_map_group` maps the entire IOmap at once), but the reactive logic supports **partial-bus operations** so a subset can be serviced without disturbing the rest:

- *Entering SAFE-OP/OP* re-maps (`configureProcessData()`: `ecx_config_map_group` → read each device's PDO mapping → `buildProcessImage` → publish) when there is no published image yet, **or** when any targeted device is rejoining from a non-exchange state — its PDO mapping is re-read because a firmware update or manual re-map may have changed it. A device already exchanging that is merely re-commanded (SAFE-OP → OP) skips the re-map; the published image still describes it.
- *Leaving SAFE-OP/OP* tears the image down **only when no device will remain exchanging**; if other devices stay in SAFE-OP/OP they keep running and the leaving device simply drops out (its working-counter share is removed by `updateExpectedWkc()`).

So one or more devices can be taken to BOOT (firmware) or PRE-OP (re-map) while the others keep exchanging, and bringing them back re-maps the whole bus. Re-mapping briefly pauses exchange for the whole bus (the IOmap is rebuilt in one shot) — the accepted cost of bringing a device online; everything else continues.

**AL transition validity is enforced before the re-map.** `transitionToState` rejects illegal EtherCAT AL transitions up front (`kValidStateTransitions` in `device_manager.cc`: single-step climbs `INIT → PRE-OP → SAFE-OP → OP`, multi-step drops, BOOT only paired with INIT). This is not just UX — the re-map reads each device's PDO mapping over the **CoE mailbox**, which is only live from PRE-OP up, so a device commanded straight from BOOT (firmware-sized mailbox, no CoE) into an exchange state would otherwise reach that mailbox read while still in BOOT and **segfault inside SOEM** before the slave could reject it with AL status 0x0011. The guard keeps the bad jump away from the mapper entirely; it lives in the node layer because re-mapping is a Motion Master concern, not the fieldbus's (direct `FieldbusDriver` use is the caller's own risk).

**The re-map resets per-slave FMMU state first.** `configureProcessData()` runs `ecx_config_map_group` *without* an `ecx_config_init`, but SOEM's FMMU mappers start at `slavelist[i].FMMUunused` and append, trusting init's memset cleared it. So the driver zeroes each slave's `FMMU[]` + `FMMUunused` (and FPWR-clears the ESC FMMU registers) before every map — otherwise a re-map after a BOOT excursion writes a duplicate Outputs FMMU and then an out-of-bounds `ec_fmmut` past the `EC_MAXFMMU`-sized array, corrupting adjacent `ec_slavet` fields (the same memory smash behind the historical `PO2SOconfig` segfault).

**`init`/`reset`/`configureProcessData` vs `exchangeProcessData`:** the first three run on the HTTP thread and mutate `DeviceManager::driver_`/`devices_` and the IOmap; `exchangeProcessData()` runs on the RT loop. The boundary is guarded by an atomically-published process-image pointer (RT reads it lock-free; control-plane operations publish `nullptr` first so exchange becomes a no-op) plus `stopExchange()`, which drains an in-flight cycle (bounded wait on an `exchanging` flag) before re-mapping or tearing down. Published images are retained until `reset()` so the RT thread never reads a freed image.

Cycle timer: `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)` on Linux (absolute mode to prevent drift accumulation); `CreateWaitableTimerEx` with `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` on Windows.

### Networking / TLS

Motion Master binds to `127.0.0.1:61447` (HTTP API) and `127.0.0.1:62281` (WebSocket), on separate event loops/threads. The PWA at `https://motion-master.synapticon.com` connects to `https://local.motion-master.synapticon.com:61447` (HTTP API) and `wss://local.motion-master.synapticon.com:62281` (WebSocket). The WS port is configurable via `--ws-port`. The DNS record `local.motion-master.synapticon.com A 127.0.0.1` resolves to localhost. CORS is set to `Access-Control-Allow-Origin: https://motion-master.synapticon.com`.

**TLS certificate:** A real Let's Encrypt cert for `local.motion-master.synapticon.com` is bundled with every release. Renewal is automated via `cert-renewal.yml` using DNS-01 with acme-dns delegation: `_acme-challenge.local.motion-master.synapticon.com` is a permanent CNAME to `4723b93a-99f5-43d7-93f1-195dbb4168ea.auth.acme-dns.io`; acme.sh updates the challenge record there via the `dns_acmedns` plugin without touching the main DNS zone. **acme.sh reads its acme-dns credentials from the `ACMEDNS_*` environment variables, not a file** — the workflow parses the `ACMEDNS_CONFIG` secret with `jq` and exports `ACMEDNS_USERNAME`/`PASSWORD`/`SUBDOMAIN`/`BASE_URL`, guarding that the parsed subdomain matches the CNAME target before any validation attempt. The renewed cert and key are stored as GitHub Secrets `TLS_CERT`/`TLS_KEY` (bundled into release artifacts by `release.yml`) **and** published as `cert.pem`/`key.pem` assets on a rolling, fixed-tag `tls-cert` release (marked pre-release so it never shadows app releases). That gives a stable, always-current fetch URL `https://github.com/synapticon/motion-master/releases/download/tls-cert/{cert,key}.pem` — decoupled from app-release cadence, since a months-old release's bundled cert would already be expired. Publishing the keypair is safe: it only authenticates `local.motion-master.synapticon.com`, which resolves to `127.0.0.1`.

**Cert self-heal.** The binary fetches a fresh cert itself rather than relying on the HTTP API, because an expired cert blocks the PWA's cross-origin `fetch()` (no browser click-through) — the API can't fix the very failure it would address — and terminal-only users never open the UI. `cert_updater.{h,cc}` (`fetchAndSwapCert`, libcurl + the already-linked OpenSSL) downloads cert+key, validates the pair (parses, CN matches, not expired, key matches cert), then atomically installs them (temp + rename, key `0600`). At startup `main.cc` self-heals: a missing/expired cert triggers a fetch before binding TLS (missing + fetch-fail is fatal; expired + fail serves the expired cert); `--no-cert-update` opts out for air-gapped installs. `--update-cert` fetches, installs, and exits (the headless/CLI path); `--cert-url`/`--key-url` override the source. `GET /api/cert` reports the served cert's validity (`expiresSoon` within `kCertExpiryWarningDays` = 7) for the UI banner — a convenience for the still-valid proactive-refresh case only.

On developer machines, `tools/run.sh` discovers the cert in this priority order: `cert.pem`/`key.pem` next to the binary (release install) → `~/.acme.sh/local.motion-master.synapticon.com_ecc/` (acme.sh local install, renewed automatically by cron) → self-signed fallback (requires accepting a browser security exception).

**Remote/LAN deployment (Raspberry Pi appliance) — planned, not yet in code.** Everything above assumes the server is on the *same machine* as the browser (the bundled `local.motion-master.synapticon.com` cert works only because that name pins to `127.0.0.1`). Running Motion Master on a separate machine the PWA reaches over the network (e.g. a flashable Raspberry Pi image at a LAN IP) breaks that: the cert won't match the Pi's address, you can't get a public cert for a private IP or `.local`, and self-signed is dead because the PWA's cross-origin `fetch()` gives no click-through. The planned fix reuses the existing DNS-01 + acme-dns machinery: a **wildcard cert** `*.ip.motion-master.synapticon.com` (Let's Encrypt issues wildcards, DNS-01 only) over an **IP-encoding DNS responder** (sslip.io pattern — `192-168-1-50.ip.… → A 192.168.1.50`), baked into the Pi image, with the client transforming the entered IP into the dashed hostname. One wildcard covers every Pi at every address. See NEXTGEN.md "Session 2026-06-12 — Remote/LAN deployment" for the full design, the discovery (mDNS/Avahi) and arm64-packaging gaps, and rejected alternatives — read it before implementing.

### Monitoring WebSocket Protocol

Two message types are sent over the WebSocket:

```json
{"type": "monitoring", "topic": "left-leg", "data": [[1735821000123456, 39, 0, 12345], ...]}
{"type": "notification", "data": {"event": "slaves_changed"}}
```

`data` is an array of **cycle rows** (the stream is lossless — one row per recorded cycle since the monitoring's last flush). Each row is `[timestampUs, v0, v1, ...]`: epoch **microseconds** (JS-exact, distinct per sub-ms cycle) followed by one value per parameter, positionally ordered — no keys in the high-frequency path. A value is `null` while its device is not exchanging. Clients fetch the order (and how each value is sourced) once and cache it:

```
GET /api/monitorings/{topic} → { ..., "parameters": [{"devicePosition":1,"index":24676,"subindex":0,"source":"pdo"}, ...] }
```

The order is stable for the lifetime of a monitoring. `interval` is the flush **cadence** (bounded 5–2000 ms, not a sample rate): a longer interval ships more rows per message, never fewer cycles. Throughput is constant (~one row per cycle, ~450 bytes for ~40 × 32-bit values); interval only trades message size against frequency. Up to 5 simultaneous clients.

### Fieldbus Capability Surface

What the fieldbus exposes today, and what is deliberately deferred. **Bus-level** (sidebar group *Fieldbus*): Control (AL state), Configuration (static SM/FMMU/DC/mailbox/addresses), Process Image (PDO layout + WKC health, plus a recorder dump to `.mmpd` via `POST /api/process-data/dump`), Diagnostics (live ESC error counters / link / watchdog), DC Sync (live distributed-clock deviation — system-time difference 0x092C). **Per-device**: FoE, Parameters (CoE object dictionary + SDO), Registers (ESC read/write), SII (EEPROM read).

Deferred fieldbus work is catalogued in NEXTGEN.md (session 2026-06-01), ranked by value-vs-effort — read it before adding a new fieldbus view rather than re-deriving the list. Top of the queue: a **topology / cabling map** (near-pure presentation of data SOEM already caches — `topology`/`activeports`/`parent`/`parentport` + the per-port link state Diagnostics already reads) and a **master-side frame/WKC health timeline** (catches intermittent faults a point-in-time WKC reading misses). Lower priority / higher risk: CoE Diagnosis History (0x10F3), device-locate blink, DC SYNC0 activation, PDO remapping, SII write. Out of scope for SOMANET: cable redundancy and the non-CoE mailbox protocols (EoE/SoE/AoE/VoE).

### CiA402 / Somanet

Profiles are **borrowed views**, not subtypes of `Device`: the inheritance chain `ProfileDevice ← Cia402Drive ← SomanetDrive` holds only a `Device&` (the only data member permitted in the whole chain). A view binds a device for one operation and is built via the validated factories `createCia402Drive(Device&)` / `createSomanetDrive(Device&)` (offline-safe — they check the CiA402 implementation / immutable vendor ID before binding). The borrowed `Device&` must outlive the view; never cache one across a bus rescan (which rebuilds `DeviceManager`'s device vector). SOMANET specifics are SOMANET-OD access on `SomanetDrive` plus free functions in `namespace somanet`. Multi-step procedures split by whether they need the per-cycle process image (see the `CyclicTask`-membership note under *Game Loop / RT Threading*): cycle-locked target generators (SineWave, *planned*) will be fixed-membership RT `CyclicTask`s gated active/idle by a per-device control block, *launched from the view* — `Cia402Drive::startSineWave(...)`/`stopSineWave()` validate and write the control block; command-and-wait procedures (encoder calibration / offset detection, auto-tuning) run off-RT on a background `std::jthread` calling `DeviceManager`.

## Dependencies

Managed via vcpkg (`extern/vcpkg` submodule, pinned in `vcpkg.json`). To add a dependency: add it to `vcpkg.json`, then `find_package` + `target_link_libraries` in the relevant `CMakeLists.txt`.

| Package | Version | Used in | CMake target |
|---|---|---|---|
| `cli11` | 2.6.2 | `motion_master` | `CLI11::CLI11` |
| `gtest` | 1.17.0 | test targets | `GTest::gtest`, `GTest::gtest_main` |
| `neargye-semver` | 1.0.0-rc | `mm_core` | `semver::semver` |
| `nlohmann-json` | 3.12.0 | `motion_master` | `nlohmann_json::nlohmann_json` |
| `spdlog` | 1.17.0 | `motion_master` | `spdlog::spdlog` |
| `uwebsockets` | 20.77.0 | `motion_master` | `unofficial::uwebsockets::uwebsockets` |

Do not commit private keys or certificates (`*.key`, `*.pem`).

## Testing

Tests live alongside their library in a `tests/` subdirectory. The test binary is discovered automatically by CTest via `gtest_discover_tests`.

```bash
./tools/test.sh                          # run all tests
ctest --test-dir build/x64-linux-debug -R VersionTest  # run a specific test by name
```

## Hardware-in-the-Loop Tests

The `hil/` directory contains standalone binaries that run on a pre-configured RT Linux machine. These are not CTest unit tests — they exercise real OS scheduling behaviour and require elevated privileges.

### jitter_bench

Measures GameLoop scheduling jitter: how much each actual cycle interval deviates from the target period. Runs the same `CyclicTimer` loop the production `GameLoop` uses, sets `SCHED_FIFO` priority 80 + `mlockall`, and records a `clock_gettime(CLOCK_MONOTONIC)` timestamp immediately after each `waitForNextCycle()` returns.

```bash
# Build
./tools/build.sh

# Run — requires root or CAP_SYS_NICE + CAP_IPC_LOCK for valid RT results
sudo ./build/x64-linux-debug/hil/jitter_bench/jitter_bench [options]

#   --duration <s>    run duration in seconds        (default: 30)
#   --period <µs>     cycle period in microseconds   (default: 1000)
#   --workload <µs>   per-cycle busy-wait to simulate task load  (default: 0)
#   --output <file>   CSV output path                (default: jitter.csv)

# Graph results (requires matplotlib)
python3 hil/jitter_bench/plot_jitter.py jitter.csv
python3 hil/jitter_bench/plot_jitter.py jitter.csv -o report.png
```

`--workload` simulates per-cycle task execution with a CPU-bound spin-wait, so you can test whether a realistic task budget (e.g. `--workload 300` for 300 µs of work in a 1 ms cycle) causes jitter spikes or overruns on a given kernel. The CSV has columns `cycle`, `elapsed_ms`, `jitter_ns`; the plot script renders a time-series and histogram and prints min/max/mean/stddev/P50/P95/P99/P99.9.

### api

TypeScript integration tests for the HTTP API and monitoring WebSocket, using Vitest. They drive the published client library (`@synapticon/motion-master-client`, a `workspace:*` member) against a real server, so a run exercises Motion Master, the HTTP/WS contract, and the client together. The global setup manages the full Docker lifecycle automatically — no manual server startup required.

```bash
pnpm install                                 # from the repo root — first time only
pnpm --filter motion-master-api-tests test   # build image → start container → run tests → stop & remove container
```

The `motion-master` Docker image is built from the repo root and run with `--network host` (required because the server binds to `127.0.0.1`). Set `MM_SKIP_DOCKER=1` to bypass Docker and test against an already-running instance (e.g. from `./tools/run.sh`).

## Code Style

Formatting is enforced by `.clang-format` (Google layout, 100-column limit). Run `./tools/format.sh` or the CMake target:

```bash
ninja -C build/x64-linux-debug format
```

### Naming Conventions

| Category | Convention | Examples |
|---|---|---|
| Classes, structs, enums, type aliases | `PascalCase` | `NetworkAdapter`, `GameLoop`, `SoemDriver` |
| Functions (free and member) | `camelCase` | `isMacAddress()`, `addTask()`, `resolveNetworkAdapter()` |
| Variables, parameters, struct members | `camelCase` | `macLinux`, `adapterName`, `certFile` |
| Private class data members | `camelCase_` (trailing `_`) | `period_`, `running_`, `tasks_` |
| Files | `snake_case` | `game_loop.cc`, `soem_driver.h` |
| Namespaces | `snake_case` | `mm::comm`, `mm::core` |
| Macros | `SCREAMING_SNAKE_CASE` | `MAX_RETRY_COUNT` |

Headers use `.h`, sources use `.cc`. Repo/folder names use hyphens (`motion-master`) by GitHub convention. Naming conventions are enforced in code review — no automated tool checks them.

## Static Analysis

`lint`, `cppcheck`, and `format` are CMake custom targets defined in `cmake/lint.cmake`, callable via scripts or directly with ninja:

```bash
./tools/cppcheck.sh                            # or: ninja -C build/x64-linux-debug cppcheck
./tools/lint.sh                                # or: ninja -C build/x64-linux-debug lint
```

cpplint is configured via `CPPLINT.cfg` (`-legal/copyright`, `-build/c++11` suppressed; 100-column limit; `.h` treated as headers). cppcheck runs with `warning,style,performance,portability`, `--std=c++23`, exits non-zero on findings.
