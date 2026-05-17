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

### Scripts

All common tasks have wrapper scripts in `tools/`. They default to the `x64-linux-debug` preset; pass a preset name as the first argument to override.

```bash
./tools/configure.sh              # cmake --preset
./tools/build.sh                  # cmake --build --preset
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

## Architecture

### Directory Layout

```
motion-master/
  apps/
    motion_master/     ← main executable (flat file layout)
    playground/        ← scratch binary
  libs/
    core/              ← version, seqlock, platform timers, cross-cutting utils
    comm/              ← fieldbus interfaces; soem.cc, spoe.cc, igh.cc alongside base
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

Managed via vcpkg (`extern/vcpkg` submodule, pinned in `vcpkg.json`). To add a dependency: add it to `vcpkg.json`, then `find_package` + `target_link_libraries` in the relevant `CMakeLists.txt`.

| Package | Version | Used in | CMake target |
|---|---|---|---|
| `cli11` | 2.6.2 | `motion_master` | `CLI11::CLI11` |
| `gtest` | 1.17.0 | test targets | `GTest::gtest`, `GTest::gtest_main` |
| `neargye-semver` | 1.0.0-rc | `mm_core` | `semver::semver` |
| `nlohmann-json` | 3.12.0 | `motion_master` | `nlohmann_json::nlohmann_json` |
| `spdlog` | 1.17.0 | `motion_master` | `spdlog::spdlog` |

Do not commit private keys or certificates (`*.key`, `*.pem`).

## Testing

Tests live alongside their library in a `tests/` subdirectory. The test binary is discovered automatically by CTest via `gtest_discover_tests`.

```bash
./tools/test.sh                          # run all tests
ctest --test-dir build/x64-linux-debug -R VersionTest  # run a specific test by name
```

## Code Style

Formatting: `.clang-format` (Google style, 100-column limit). Run `./tools/format.sh` to format all sources, or use the CMake target:

```bash
ninja -C build/x64-linux-debug format
```

File naming: `lowercase_with_underscores` (e.g. `soem_driver.cc` pairs with `SoemDriver`). Repo/folder name uses hyphens (`motion-master`) by GitHub convention.

## Static Analysis

`lint`, `cppcheck`, and `format` are CMake custom targets defined in `cmake/lint.cmake`, callable via scripts or directly with ninja:

```bash
./tools/cppcheck.sh                            # or: ninja -C build/x64-linux-debug cppcheck
./tools/lint.sh                                # or: ninja -C build/x64-linux-debug lint
```

cpplint is configured via `CPPLINT.cfg` (`-legal/copyright`, `-build/c++11` suppressed; 100-column limit; `.hh` treated as headers). cppcheck runs with `warning,style,performance,portability`, `--std=c++23`, exits non-zero on findings.
