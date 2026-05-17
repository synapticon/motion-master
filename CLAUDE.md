# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Context

**Motion Master v6.0.0** — next-generation motion control software. This project is a clean-sheet rewrite of the previous `motion_master` codebase; the design rationale and session notes live in `NEXTGEN.md`. Read that file for architectural decisions, class diagrams, and design rationale before making structural changes.

Key design mandates from NEXTGEN.md:
- No exceptions — use `std::expected<T, std::string>` (C++23 stdlib, no `tl::expected`)
- HTTP API + single monitoring WebSocket (no Protobuf, no dual-port setup)
- Single `Device` abstraction (replaces `VirtualDevice` + `comm::base::Device` overlap from the old codebase)
- `IFieldbusDriver` interface abstracts SOEM, SPoE, and IgH EtherCAT — `SoemDriver`, `SpoeDriver`, `IghDriver` are the concrete implementations
- `App` is the only place that instantiates concrete types (dependency injection at the composition root)
- Namespaces mirror directory layout (`mm::core`, `mm::comm::soem`, `mm::api`, `mm::devices`); do not use C++20 modules

## Build System

CMake 4.0+ with Ninja and vcpkg. Initialize the vcpkg submodule before the first build:

```bash
git submodule update --init --recursive
```

### Build Commands

```bash
# Configure
cmake --preset x64-linux-debug       # also: x64-linux-release, x64-windows-debug, x64-windows-release

# Build
cmake --build --preset x64-linux-debug

# Or directly with Ninja after configuring
ninja -C build/x64-linux-debug
```

Build output: `build/<presetName>/`. Binaries land in `build/x64-linux-debug/apps/motion_master/motion-master` and `build/x64-linux-debug/apps/playground/playground`.

Compiler requirements: C++23 required. Warnings are errors (`-Wall -Wextra -Wpedantic -Werror` on GCC/Clang; `/W4 /WX` on MSVC).

### Planned CMake Custom Targets

Replace any `.sh`/`.ps1` scripts with CMake targets: `format`, `lint`, `docs`. All helper scripts go in `tools/`.

## Architecture

### Directory Layout

```
motion-master/
  apps/
    motion-master/     ← main executable (flat file layout)
    playground/        ← scratch binary
  libs/
    core/              ← ThreadSafeQueue, seqlock, platform timers, cross-cutting utils
    comm/              ← flat layout: soem.cc, spoe.cc, igh.cc alongside base interfaces
  extern/
    vcpkg/             ← git submodule
    SOEM/              ← git submodule (planned)
  tools/               ← all helper scripts
```

Flat layout within each lib/app is intentional — navigate by filename and grep, not nested folders.

### Class Structure (from NEXTGEN.md)

```
App  (composition root, owns everything)
 ├── Config
 ├── IFieldbusDriver               ← SoemDriver, SpoeDriver, IghDriver
 ├── DeviceManager
 │     ├── owns: Device[]
 │     └── uses: IFieldbusDriver
 ├── Device                        ← single abstraction; no VirtualDevice/comm::base::Device split
 │     ├── owns: DeviceParameter[] (index/subindex → DeviceParameterValue variant)
 │     ├── owns: PdoMappings
 │     └── owns: Cia402StateMachine  (only if Cia402Drive)
 ├── GameLoop  (RT thread, SCHED_FIFO, 1ms)
 │     ├── uses: IFieldbusDriver
 │     ├── writes: Device parameters via seqlock
 │     └── runs: ICyclicTask[]  (Watchdog, MonitorPublisher)
 ├── SdoService  (dedicated thread, safe concurrent with RT loop via SOEM socket mutex)
 ├── HttpServer
 ├── WebSocketServer  (monitoring output)
 ├── NotificationBus  (observer; decouples Watchdog/DeviceManager from servers)
 ├── FirmwareInstaller
 └── NetworkScanner
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

PDO values are shared between the RT loop and HTTP/monitoring readers via a **seqlock** (odd seq = write in progress; even = stable). At ~100 PDO values / 400 bytes at 1 ms cycles, the retry path is effectively never triggered.

Cycle timer: `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)` on Linux (absolute mode to prevent drift accumulation); `CreateWaitableTimerEx` with `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` on Windows.

### Networking / TLS

Motion Master binds to `127.0.0.1:8443`. The PWA at `https://motion-master.synapticon.com` connects to `https://local.motion-master.synapticon.com:8443` (HTTP API) and `wss://local.motion-master.synapticon.com:8443` (WebSocket). The DNS record `local.motion-master.synapticon.com A 127.0.0.1` resolves to localhost. A real CA-signed TLS cert is bundled with each release; renewal is automated via DNS-01 ACME in CI/CD. CORS is set to `Access-Control-Allow-Origin: https://motion-master.synapticon.com`.

### CiA402 / Somanet

`Device → Cia402Drive` via inheritance (is-a relationship, shallow). **No** `Cia402Drive → SomanetDevice` inheritance — Somanet-specific OD access is free functions in `namespace somanet`; multi-step procedures (encoder calibration, auto-tuning) are `ICyclicTask` implementations that take a `Cia402Drive&`.

## Dependencies

Managed via vcpkg (`extern/vcpkg` submodule, pinned in `vcpkg.json`). Add to the `dependencies` array in `vcpkg.json`, then use `find_package` in the relevant `CMakeLists.txt`.

Planned additions: `loguru` (via vcpkg, not submodule), `GTest`, HTTP server library, WebSocket library.

Do not commit private keys or certificates. Add `*.key` and `*.pem` to `.gitignore`.

## Code Style

Formatting: `.clang-format` (Google style, 100-column limit).

```bash
# Format a file
clang-format -i <file>

# Format all source files
find apps libs tools -name '*.cc' -o -name '*.hh' | xargs clang-format -i
```

File naming: `lowercase_with_underscores` (e.g. `soem_driver.cc` pairs with `SoemDriver`). Repo/folder name uses hyphens (`motion-master`) by GitHub convention.

## Static Analysis

Both tools run as CMake custom targets defined in `cmake/lint.cmake`. Run them after configuring:

```bash
# cpplint — Google C++ style lint (requires: pip install cpplint)
ninja -C build/x64-linux-debug lint

# cppcheck — static analysis (already installed)
ninja -C build/x64-linux-debug cppcheck
```

cpplint is configured via `CPPLINT.cfg` at the repo root (`-legal/copyright`, `-build/c++11` suppressed; 100-column limit; `.hh` treated as headers). cppcheck runs with `warning,style,performance,portability` checks, `--std=c++23`, and exits non-zero on findings.
