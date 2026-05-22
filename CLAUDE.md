# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Context

**Motion Master v6.0.0** — next-generation motion control software. This project is a clean-sheet rewrite of the previous `motion_master` codebase; the design rationale and session notes live in `NEXTGEN.md`. Read that file for architectural decisions, class diagrams, and design rationale before making structural changes.

Key design mandates from NEXTGEN.md:
- No exceptions — use `std::expected<T, std::string>` (C++23 stdlib, no `tl::expected`)
- HTTP API + single monitoring WebSocket (no Protobuf, no dual-port setup)
- Single `Device` abstraction (replaces `VirtualDevice` + `comm::base::Device` overlap from the old codebase)
- `FieldbusDriver` interface abstracts SOEM and SPoE — `SoemFieldbusDriver` and `SpoeDriver` are the concrete implementations; `FieldbusDriver` owns the mutex that serializes SDO and PDO socket access across threads
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
./tools/clean.sh                  # remove build/<preset>

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

## CI

The workflow in `.github/workflows/build.yml` caches vcpkg binaries with `actions/cache@v5` on `~/.cache/vcpkg/archives`, keyed on OS + `vcpkg.json` hash. The `x-gha` vcpkg binary caching backend was **removed** in the pinned vcpkg version (`56bb241`) — do not use `VCPKG_BINARY_SOURCES: "clear;x-gha,readwrite"` or the `actions/github-script` workaround.

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
 │     └── init(unique_ptr<FieldbusDriver>), configure(), reset(), pdoExchange(), state transitions
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

**Thread safety caveat — `init`/`reset` vs `pdoExchange`:** `POST /api/init`, `POST /api/configure`, and `POST /api/reset` run on the HTTP server thread and mutate `DeviceManager::driver_` and `devices_`. `pdoExchange()` runs on the RT GameLoop thread and reads both. There is currently no lock guarding this boundary. This is safe only because `pdoExchange()` is not yet wired into the GameLoop. Before enabling live PDO exchange, the loop must be stopped (or at least drained for one cycle) before `init()` or `reset()` is called via the API.

Cycle timer: `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)` on Linux (absolute mode to prevent drift accumulation); `CreateWaitableTimerEx` with `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` on Windows.

### Networking / TLS

Motion Master binds to `127.0.0.1:8443`. The PWA at `https://motion-master.synapticon.com` connects to `https://local.motion-master.synapticon.com:8443` (HTTP API) and `wss://local.motion-master.synapticon.com:8443` (WebSocket). The DNS record `local.motion-master.synapticon.com A 127.0.0.1` resolves to localhost. A real CA-signed TLS cert is bundled with each release; renewal is automated via DNS-01 ACME in CI/CD. CORS is set to `Access-Control-Allow-Origin: https://motion-master.synapticon.com`.

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
