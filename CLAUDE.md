# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Context

**Motion Master v6.0.0** — next-generation motion control software for SOMANET servo drives. This project is a clean-sheet rewrite of the previous `motion_master` codebase; the design rationale and session notes live in `NEXTGEN.md`. Read that file for architectural decisions, class diagrams, and design rationale before making structural changes.

Key design mandates from NEXTGEN.md:
- No exceptions — use `std::expected<T, std::string>` (C++23 stdlib, no `tl::expected`); error strings are used throughout for now — structured error types are a future improvement once it is clear which callers need to branch on error kind
- HTTP API + single monitoring WebSocket (no Protobuf, no dual-port setup)
- Single `Device` abstraction (replaces `VirtualDevice` + `comm::base::Device` overlap from the old codebase)
- `FieldbusDriver` interface abstracts SOEM and SPoE — `SoemFieldbusDriver` and `SpoeDriver` are the concrete implementations; `FieldbusDriver` owns `socketMutex_`, which serializes the **control-plane** operations (mailbox/SDO, FoE, ESC register, state access) amongst non-RT callers. The **PDO path (`exchangeProcessData`) runs lock-free** — SOEM's port layer is internally thread-safe (per-datagram index allocation + tx/rx mutexes held only for a single non-blocking poll, with cooperative frame demux) and PDO touches disjoint state (the process-data IOmap) from the control plane, so the RT cycle is never blocked by a slow SDO or object-dictionary enumeration. The lock is held for one socket transaction only — never across a sleep, a blocking wait, or a user callback (so `readObjectDictionary` and `transitionToState` lock per transaction, not for their whole multi-second duration)
- `DeviceManager` owns `FieldbusDriver` via `unique_ptr<FieldbusDriver>` (null until `init()` is called) — the driver is constructed by `main.cc` and transferred via `DeviceManager::init(unique_ptr<FieldbusDriver>)`; `DeviceManager` never references concrete driver types
- No service layer — SDO read/write, file transfer, and state control are methods on `Device` and `DeviceManager`; `HttpServer` and `GameLoop` both take a `DeviceManager&` directly
- `GameLoop` calls `deviceManager_.pdoExchange()` — it has no knowledge of `FieldbusDriver`; `pdoExchange()` is a no-op when the driver is null so the loop always starts unconditionally
- `DeviceManager` owns slave discovery and network scanning via `FieldbusDriver` — there is no separate `NetworkScanner`
- `App` is the only place that instantiates concrete types (dependency injection at the composition root); `Server::Config` carries an `InitDriverFn` callback wired in `main.cc` so `POST /api/init` can create a driver without the server knowing concrete types
- Namespaces mirror directory layout (`mm::core`, `mm::comm::soem`, `mm::node`, `mm::api`); do not use C++20 modules
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
./tools/build.sh                  # cmake --build --preset; then sudo setcap for raw socket + RT access
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

Files updated by the script: `VERSION`, `vcpkg.json`, `ui/package.json`, `ui/apps/motion-master/package.json`, `ui/packages/api-client/package.json`, `hil/api/package.json`, `apps/motion_master/swagger.yml`, the `StringConstant` assertion in `libs/core/tests/version_test.cc`, and the sidebar badge in `ui/apps/motion-master/src/layouts/RootLayout.tsx`.

`hil/api/src/mm-api.ts` is auto-generated from `swagger.yml` via `swagger-typescript-api` — regenerate it separately if the API shape changed.

After bumping, commit all changed files, then push a `v<version>` tag to trigger `release.yml`.

## CI

The workflow in `.github/workflows/build.yml` caches vcpkg binaries with `actions/cache@v5` on `~/.cache/vcpkg/archives`, keyed on OS + `vcpkg.json` hash. The `x-gha` vcpkg binary caching backend was **removed** in the pinned vcpkg version (`56bb241`) — do not use `VCPKG_BINARY_SOURCES: "clear;x-gha,readwrite"` or the `actions/github-script` workaround.

Three additional workflows exist alongside `build.yml`:

- **`cert-renewal.yml`** — runs on the 1st of every month. Uses `acme.sh` with the `dns_acmedns` plugin against `auth.acme-dns.io` to issue a fresh Let's Encrypt cert for `local.motion-master.synapticon.com` (DNS-01 via CNAME delegation — no manual DNS touch required after the one-time setup). Stores the renewed cert and key as GitHub Secrets `TLS_CERT` and `TLS_KEY`. Requires `ACMEDNS_CONFIG` (acme-dns credentials JSON) and `GH_PAT_SECRETS` (fine-grained PAT with Secrets read/write on this repo) to be set as repository secrets.
- **`release.yml`** — triggered by `v*` tags. Builds with the `x64-linux-release` preset, reads `TLS_CERT` and `TLS_KEY` from secrets, writes them as `cert.pem`/`key.pem` into the build output, then calls `tools/package.sh` to produce a `.deb` and `.rpm`, and creates a GitHub Release with all three artefacts: `motion-master-<version>-linux-x64.tar.gz`, `motion-master-<version>-amd64.deb`, and `motion-master-<version>-x86_64.rpm`. Requires `dpkg-dev` and `rpm` (both installed as CI system dependencies). All packages install to `/opt/motion-master/`; `cert.pem` and `key.pem` are marked as conffiles (deb) / `%config(noreplace)` (rpm) so upgrades never silently overwrite them. On deb, `apt remove` leaves conffiles behind — `apt purge` is required for a full uninstall. On rpm, `dnf remove` removes unmodified config files automatically; modified ones are saved as `.rpmsave`.
- **`lint.yml`** — runs clang-format and cpplint checks on every push and PR.

## Docker

The `Dockerfile` is a two-stage build (build on `ubuntu:24.04`, minimal runtime image). The binary and swagger.yml land in `/opt/motion-master/` — consistent with the deb/rpm install path. `docker-entrypoint.sh` mirrors the cert discovery order of `tools/run.sh`: CERT/KEY env vars → bundled cert baked into the image → acme.sh mount → self-signed fallback.

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
    motion_master/     ← main executable (flat file layout); swagger.yml ships here alongside binary
    playground/        ← scratch binary
  libs/
    core/              ← version, seqlock, platform timers, cross-cutting utils
    comm/              ← fieldbus interfaces; soem.cc, spoe.cc, igh.cc alongside base
    node/              ← Device, DeviceManager, CiA402, profiles (depends on mm::comm)
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
App  (composition root, owns everything)
 ├── Config
 ├── mm::node::DeviceManager      (owns FieldbusDriver + Device[]; drives scanning)
 │     ├── unique_ptr<FieldbusDriver>   ← SoemFieldbusDriver | SpoeDriver; owns mutex
 │     │                                  null until init(); set via init(unique_ptr<FieldbusDriver>)
 │     ├── owns: mm::node::Device[] (each Device holds FieldbusDriver& + immutable SlaveInfo)
 │     │     ├── slavePosition, name, vendorId, productCode, revisionNumber, serialNumber
 │     │     ├── owns: DeviceParameter[] (index/subindex → DeviceParameterValue variant)
 │     │     ├── owns: PdoMappings
 │     │     └── owns: Cia402StateMachine  (only if Cia402Drive)
 │     └── init(unique_ptr<FieldbusDriver>), scan(), reset(), pdoExchange(), transitionToState()
 ├── GameLoop  (RT thread, SCHED_FIFO, 1ms)
 │     ├── uses: DeviceManager    (calls pdoExchange each cycle; no-op when driver is null)
 │     ├── writes: Device parameters via seqlock
 │     └── runs: ICyclicTask[]  (Watchdog, MonitorPublisher)
 ├── HttpServer
 │     ├── uses: DeviceManager    (SDO read/write, file transfer, state control)
 │     └── Config.InitDriverFn    (callback to main.cc; creates concrete driver for POST /api/init)
 ├── WebSocketServer  (monitoring output)
 ├── NotificationBus  (observer; decouples Watchdog/DeviceManager from servers)
 └── FirmwareInstaller
       └── uses: DeviceManager
```

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

`GameLoop` calls `deviceManager_.pdoExchange()` each cycle — it has no direct knowledge of `FieldbusDriver`. HTTP handlers call SDO methods on `DeviceManager`/`Device` from their own threads; `FieldbusDriver` serializes all socket access via its internal mutex.

PDO values are shared between the RT loop and HTTP/monitoring readers via a **seqlock** (odd seq = write in progress; even = stable). At ~100 PDO values / 400 bytes at 1 ms cycles, the retry path is effectively never triggered.

**Thread safety caveat — `init`/`reset` vs `pdoExchange`:** `POST /api/init`, `POST /api/scan`, and `POST /api/reset` run on the HTTP server thread and mutate `DeviceManager::driver_` and `devices_`. `pdoExchange()` runs on the RT GameLoop thread and reads both. There is currently no lock guarding this boundary. This is safe only because `pdoExchange()` is not yet wired into the GameLoop. Before enabling live PDO exchange, the loop must be stopped (or at least drained for one cycle) before `init()` or `reset()` is called via the API.

Cycle timer: `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)` on Linux (absolute mode to prevent drift accumulation); `CreateWaitableTimerEx` with `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` on Windows.

### Networking / TLS

Motion Master binds to `127.0.0.1:8443`. The PWA at `https://motion-master.synapticon.com` connects to `https://local.motion-master.synapticon.com:8443` (HTTP API) and `wss://local.motion-master.synapticon.com:8443` (WebSocket). The DNS record `local.motion-master.synapticon.com A 127.0.0.1` resolves to localhost. CORS is set to `Access-Control-Allow-Origin: https://motion-master.synapticon.com`.

**TLS certificate:** A real Let's Encrypt cert for `local.motion-master.synapticon.com` is bundled with every release. Renewal is automated via `cert-renewal.yml` using DNS-01 with acme-dns delegation: `_acme-challenge.local.motion-master.synapticon.com` is a permanent CNAME to `4723b93a-99f5-43d7-93f1-195dbb4168ea.auth.acme-dns.io`; acme.sh updates the challenge record there via the `dns_acmedns` plugin without touching the main DNS zone. The renewed cert and key are stored as GitHub Secrets `TLS_CERT` and `TLS_KEY` and bundled into release artifacts by `release.yml`.

On developer machines, `tools/run.sh` discovers the cert in this priority order: `cert.pem`/`key.pem` next to the binary (release install) → `~/.acme.sh/local.motion-master.synapticon.com_ecc/` (acme.sh local install, renewed automatically by cron) → self-signed fallback (requires accepting a browser security exception).

### Monitoring WebSocket Protocol

Two message types are sent over the WebSocket:

```json
{"type": "monitoring", "topic": "pdos", "data": [1234567890, 39, 0, 12345]}
{"type": "notification", "data": {"event": "slaves_changed"}}
```

`data` is a positionally-ordered array of numbers — no keys in the high-frequency path. Clients fetch the schema once via HTTP and cache it:

```
GET /api/monitoring/pdos → [{"index": "6064:00", "name": "actual_position"}, ...]
```

The array order is stable for the lifetime of a monitoring session. Up to 5 simultaneous clients; ~40 × 32-bit values per message ≈ 450 bytes at 1 ms cycles.

### CiA402 / Somanet

`Device → Cia402Drive` via inheritance (is-a relationship, shallow). **No** `Cia402Drive → SomanetDevice` inheritance — Somanet-specific OD access is free functions in `namespace somanet`; multi-step procedures (encoder calibration, auto-tuning) are `ICyclicTask` implementations that take a `Cia402Drive&`.

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

TypeScript integration tests for the HTTP API and monitoring WebSocket, using Vitest. The global setup manages the full Docker lifecycle automatically — no manual server startup required.

```bash
cd hil/api
npm install          # first time only
npm test             # build image → start container → run tests → stop & remove container
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
| Variables, parameters, struct members | `camelCase` | `macLinux`, `adapterName`, `swaggerFile` |
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
