# NEXTGEN

## Feature List

- Start the process without initializing devices. This enables API endpoints that return the Motion Master version, scan the network for available devices, and serve the OpenAPI/Swagger specification.
- HTTP API handles simple requests such as version info and network scanning, but also device initialization, device listing, and SDO read/write operations including file transfer.
- A single WebSocket connection for monitoring: streams values only, plus notifications (e.g. number of slaves changed, devices leaving OP state due to a watchdog timeout). Monitoring configuration is exposed through the HTTP API. Multi-axis monitoring is required.
- No complex Protobuf message structures as in the current implementation — SDO values are plain buffers, numbers, or strings; monitoring payloads are arrays of numbers.
- Careful, safe multi-threaded programming and memory management throughout.
- Motion Composer integrated into Motion Master.
- All helper/utility functions included.
- HTTP API compatible with the existing Motion Master API where possible.
- Real-time support on Linux.
- No exceptions — use `std::expected<T, std::string>` instead. The error type is `std::string` for now — structured error types (e.g. a `SdoError` with `Kind` enum and abort code) are a deliberate future step, deferred until it is clear which callers need to branch on error kind rather than just log or forward the message.
- Full device control through the API: reading registers, setting EtherCAT state, SII access, etc.
- Configurable via a JSONC config file (JSON with `//` and `/* */` comments) — timeouts, buffer sizes, and similar parameters. Parsed via nlohmann-json with `ignore_comments = true`; no extra dependency needed.
- All public functions fully documented.
- Clients are responsible for managing EtherCAT state transitions. Motion Master handles PDO configuration, decides when to start and stop process-data exchange, and rejects operations that are invalid in the current state (e.g. SDO reads or file transfers in INIT state).
- Targets x86-64 and ARM (Linux and Windows).
- Fieldbus drivers (SOEM, SPoE, IgH EtherCAT) abstracted behind a common interface.
- Structured, leveled logging throughout — verbose debug logs available without cluttering normal operation. A companion web application for viewing and analyzing logs.
- A Claude-generated ReactJS application with access to the full API surface, used initially for testing state transitions, register reads/writes, etc.
- Transition a device to INIT or BOOT state without reinitializing the entire stack.
- Runs on real-time Linux with a fixed 1 ms cycle time.
- Game loop for process-data exchange, extensible so external code can inject a class that runs each cycle.
- A single in-memory object dictionary per device with thread-safe value access. Each game-loop iteration reads all process-data values from the dictionary before the EtherCAT exchange and writes them back afterward.
- Monitoring runs in the game loop so it can emit data immediately after each exchange.
- Firmware installation supported on a single device at a time.
- Offline mode: devices remain in memory but SDO requests are rejected; previously read (or user-set) values are returned instead. Process data is not exchanged.

---

## Class Diagram UML

Monitoring
Watchdog
DeviceManager / Master
Device / Slave
DeviceParameter / Object Dictionary Entry
GameLoop
HttpServer
WebSocketServer

---

## Session 2026-05-16 — HTTPS, WebSocket security, and PWA connectivity

**Deployment model**

Motion Master is distributed as a Windows installer and Linux `.deb`/`.rpm` packages. There is no Electron wrapper. The UI is a PWA served from `https://motion-master.synapticon.com`. The PWA connects to the locally-running Motion Master process over HTTPS and WSS.

**TLS — subdomain resolving to localhost**

The DNS record `local.motion-master.synapticon.com A 127.0.0.1` is maintained by Synapticon. Motion Master bundles a real TLS certificate for that hostname issued by Let's Encrypt (or a paid CA). Because the cert is signed by a real CA, browsers trust it with no warnings and no install steps required from the customer.

Motion Master binds exclusively to `127.0.0.1:8443`. Traffic never leaves the machine — the DNS lookup returns `127.0.0.1` and the connection is loopback-only.

Cert renewal must be automated in the CI/CD pipeline via a DNS-01 ACME challenge and the renewed cert bundled into each release. With Let's Encrypt the cert expires every 90 days; a paid CA (DigiCert, Sectigo) provides 1-year validity.

This cert is completely independent of Synapticon's existing `*.synapticon.com` wildcard cert. That cert lives on Synapticon's web servers and is used to serve `motion-master.synapticon.com`; its private key never leaves Synapticon's infrastructure. The two certs share the `synapticon.com` domain name only by convention. Note also that the existing wildcard cannot be reused here — `*.synapticon.com` matches only a single label deep (e.g. `foo.synapticon.com`) and does not cover `local.motion-master.synapticon.com`.

**CORS**

Motion Master sets `Access-Control-Allow-Origin: https://motion-master.synapticon.com` on all responses. This prevents any other website from making browser-based requests to the local API. CORS is a browser-enforced mechanism — it does not block non-browser clients (curl, local scripts, other processes).

**PWA connectivity**

The PWA at `https://motion-master.synapticon.com` connects to:

- `https://local.motion-master.synapticon.com:8443` — HTTP API
- `wss://local.motion-master.synapticon.com:8443` — monitoring WebSocket

Port 8443 is fixed and well-known; no discovery mechanism is needed.

**Bearer token (optional — not implemented in v1)**

CORS covers the primary threat (other websites controlling drives via the browser). A bearer token would additionally block non-browser local processes (malware, rogue scripts) from talking to the API — relevant because Motion Master controls physical hardware.

If added: Motion Master generates a random token on first install and persists it in the user config directory (`%APPDATA%\MotionMaster\token` on Windows, `~/.config/motion-master/token` on Linux). A system tray "Open Web UI" action opens the browser to `https://motion-master.synapticon.com/?token=<token>`. The PWA stores the token in `localStorage` and sends it as `Authorization: Bearer <token>` on all requests. Subsequent visits read the token from `localStorage` automatically.

---

## Session 2026-05-16 — Class diagram, device hierarchy, naming

**Revised class diagram**

Based on reviewing the current source (`comm::base::Device`, `VirtualDevice`, `EthercatMaster`, `Cia402Drive`, `DeviceParameterRefresher`, etc.) the main structural problem is two overlapping device abstractions. NEXTGEN has one.

```
App  (composition root, owns everything)
 ├── Config
 ├── DeviceManager                (owns FieldbusDriver + Device[]; drives scanning)
 │     ├── unique_ptr<FieldbusDriver>  ← SoemDriver | SpoeDriver; owns mutex
 │     │                                 null until init(); set via init(unique_ptr<FieldbusDriver>)
 │     ├── owns: Device[]         (each Device holds FieldbusDriver&)
 │     │     ├── DeviceType  { Cia402Drive, DigitalIo }
 │     │     ├── owns: DeviceParameter[] (index/subindex → DeviceParameterValue variant)
 │     │     ├── owns: PdoMappings
 │     │     └── owns: Cia402StateMachine  (only if Cia402Drive)
 │     └── init(unique_ptr<FieldbusDriver>), scan(), reset(), pdoExchange(), state transitions
 ├── GameLoop  (RT thread, SCHED_FIFO, 1ms)
 │     ├── uses: DeviceManager    (calls pdoExchange each cycle; no-op when driver is null)
 │     ├── writes: Device parameters via seqlock
 │     └── runs: ICyclicTask[]
 │           ├── Watchdog           → NotificationBus
 │           └── MonitorPublisher   → WebSocketServer
 ├── HttpServer
 │     ├── uses: DeviceManager    (SDO read/write, file transfer, state control)
 │     └── Config.InitDriverFn    (callback to main.cc; creates concrete driver for POST /api/init)
 ├── WebSocketServer  (monitoring output)
 ├── NotificationBus  (observer; decouples Watchdog/DeviceManager from servers)
 └── FirmwareInstaller
       └── uses: DeviceManager
```

**What carries over from current code**

- `DeviceParameter` with index, subindex, and a `DeviceParameterValue` — solid design, keep it
- `PdoMappings` with `rxPdos`/`txPdos` and `PdoMappingEntry` — keep as-is
- Socket-level mutex per slave — SDO calls from any thread are safe as long as FieldbusDriver owns and holds the mutex

**What changes**

- `IEthercatDriver` → `IFieldbusDriver` — SPoE is not EtherCAT; the name was inaccurate
- `MainTimer` singleton → removed; `GameLoop` owns the timer directly
- Static `slaveMap_` in `EthernetMaster` → removed; `DeviceManager` owns `Device[]` directly
- `Cia402Drive` (currently just static maps/enums) → `Cia402StateMachine`, a proper owned component of `Device`

**Device hierarchy — inheritance vs composition**

`Device → Cia402Drive` via inheritance: correct. The relationship is genuinely "is-a" — CiA402 is a standard and every such device has the same state machine, op modes, and control/status word bits. Shallow inheritance is appropriate.

`Cia402Drive → SomanetDevice` via inheritance: no. Somanet-specific features are not what the device _is_, they are what you _do with it_ using knowledge of Somanet's OD layout. Subclassing here would force `DeviceManager` to know about `SomanetDevice` or require downcasting — both are signs the abstraction is wrong.

Instead:

- Simple Somanet-specific OD access → free functions in `namespace somanet`
- Complex multi-step procedures (encoder calibration, auto-tuning) → separate `ICyclicTask` implementations that take a `Cia402Drive&`

**Naming**

`DeviceParameter` over `OdEntry` — `OdEntry` is CoE jargon; `DeviceParameter` is self-explanatory and consistent with the existing codebase (`parametersMap_`, `VirtualParameter`). `OdEntry` can remain as an internal term inside driver implementations where object dictionary mechanics are explicit.

`DeviceParameterValue` is a type alias, not a class:

```cpp
using DeviceParameterValue = std::variant<
  int8_t, int16_t, int32_t, int64_t,
  uint8_t, uint16_t, uint32_t, uint64_t,
  float, double,
  std::string, std::vector<uint8_t>
>;
```

`DeviceParameter` holds an index, subindex, and a `DeviceParameterValue`. Type dispatch at call sites uses `std::visit`.

**Dependency injection**

`App` is the only place that instantiates concrete types. `FieldbusDriver` is injected into `DeviceManager` and `NetworkScanner`; `DeviceManager` passes a `FieldbusDriver&` into each `Device` it creates. `GameLoop` and `HttpServer` both receive a `DeviceManager&` — `GameLoop` calls `pdoExchange`, `HttpServer` calls SDO/file/state methods. Inject `NotificationBus` into `Watchdog`, `DeviceManager`, `WebSocketServer`.

---

## Session 2026-05-18 — Monitoring WebSocket protocol

**Message format**

Two message types flow over the monitoring WebSocket:

```json
{"type": "monitoring", "topic": "pdos", "data": [1234567890, 39, 0, 12345]}
{"type": "notification", "data": {"event": "slaves_changed"}}
```

`data` in a monitoring message is a positionally-ordered array of numbers. The array order is stable for the lifetime of a monitoring session. Clients fetch the schema once via HTTP (e.g. `GET /api/monitoring/pdos`) and use the positional index to look up meaning — no keys are repeated in every 1 ms message.

```
GET /api/monitoring/pdos → [{"index": "6064:00", "name": "actual_position"}, ...]
```

**Throughput**

At ~40 × 32-bit PDO values per message, a monitoring message is approximately 450 bytes of JSON. At 1 ms cycles with up to 5 simultaneous clients, total loopback throughput is ~2.25 MB/s — negligible on loopback. Using an array rather than a key-value map halves the message size and keeps the high-frequency path minimal.

---

## Session 2026-05-18 — Configuration file format

**JSONC over plain JSON**

Config files use JSONC — JSON with `//` line comments and `/* */` block comments. This is the right choice for a C++ app that already depends on nlohmann-json: no new dependency, and operators can annotate their config files naturally.

Parsed with the `ignore_comments` flag:

```cpp
std::ifstream f(configPath);
auto cfg = nlohmann::json::parse(f, nullptr, /*exceptions=*/true, /*ignore_comments=*/true);
```

Config files use the `.jsonc` extension by convention. Example:

```jsonc
{
  // EtherCAT cycle time in microseconds
  "cycle_time_us": 1000,

  /* SDO timeout; increase for slow devices or long cables */
  "sdo_timeout_ms": 100
}
```

TOML was considered (clean syntax, native comment support, `toml++` available in vcpkg) but rejected because the config schema is flat and nlohmann-json is already in the dependency graph — adding a library for the same job has no payoff here.

---

## Session 2026-05-16 — Design review and approach

**Viable.** The design reflects clear lessons from the current codebase. Key sound decisions: `std::expected` over exceptions, HTTP + single monitoring WebSocket instead of dual-port Protobuf, object dictionary as single source of truth updated each cycle, and clients owning EtherCAT state transitions (removes significant complexity from Motion Master).

**Watch out for:**

- ARM: SOEM has known quirks (alignment, NIC driver support) — validate early.
- Offline mode: returning stale values silently can confuse clients; tag values with a freshness flag or timestamp.
- ReactJS test app: useful for driving the API surface, but don't let it influence API design decisions.

**Recommended phases:**

1. **Skeleton** — CMake + vcpkg, HTTP server, JSON config, `std::expected` error type, stub object dictionary, monitoring WebSocket with fake data. Nail the API shape before any hardware.
2. **SOEM + RT loop** — 1 ms game loop, PDO exchange, object dictionary wired up, SDO thread. Validate concurrency under load.
3. **API completeness** — client-driven state management, SII access, firmware install, offline mode.
4. **Additional drivers** — SPoE, then IgH. The driver abstraction pays off here.

---

## Session 2026-05-16 — RT loop, SDO threading, object dictionary

**HTTP API + real-time loop**

The RT loop runs on a `SCHED_FIFO` thread and owns the EtherCAT context. SDO operations (CoE mailbox) and PDO exchange (LRW datagrams) are separate EtherCAT mechanisms. `FieldbusDriver` owns `socketMutex_`, which serializes the **control-plane** operations (mailbox/SDO, FoE, ESC register, state access) amongst non-RT callers. The **PDO path (`exchangeProcessData`) is deliberately lock-free**: SOEM's port layer is internally thread-safe — each datagram gets a unique index (`ecx_getindex` under `getindex_mutex`), `tx_mutex`/`rx_mutex` are held only around a single non-blocking poll (`ecx_inframe`), and a frame received for another waiter's index is cooperatively routed to its buffer. PDO and the control plane also touch disjoint high-level state (process-data IOmap vs mailbox pool / slave state). So a PDO thread and an SDO/mailbox call can share one socket without our mutex, and the RT cycle is never stalled by a slow SDO or a multi-second object-dictionary enumeration. HTTP handlers call `device.readSdo()` / `device.writeSdo()` directly on their thread; each call takes `socketMutex_` for the duration of one transaction — never across a sleep, a blocking wait, or a user callback — so `readObjectDictionary` and `transitionToState` lock per transaction rather than for their whole run. The remaining contention is at SOEM's port layer (tx/rx mutex, ~µs per frame), an acceptable sub-cycle cost.

**Object dictionary threading**

PDO values are written by the RT loop after each exchange and read by HTTP handlers and the monitoring subsystem. With ~100 values of 32 bits each (≈400 bytes), a seqlock is the right pattern: simple, lock-free on the write path, and the retry path on reads is effectively never triggered at 1 ms cycle times.

```cpp
// Writer (RT loop):
dict.seq.fetch_add(1, memory_order_relaxed);  // odd = write in progress
memcpy(&dict.values, &fresh_values, sizeof(PdoValues));
dict.seq.fetch_add(1, memory_order_release);  // even = done

// Reader (HTTP / monitoring):
PdoValues snap;
uint32_t s1, s2;
do {
    s1 = dict.seq.load(memory_order_acquire);
    if (s1 & 1) continue;
    memcpy(&snap, &dict.values, sizeof(PdoValues));
    s2 = dict.seq.load(memory_order_acquire);
} while (s1 != s2);
```

Triple buffering was considered but is unnecessary at this data size. `memcpy` is preferred over struct assignment for the copy to allow vectorization and avoid padding surprises.

---

## Session 2026-05-16 — Game loop timing and startup wiring

**Main thread is the game loop**

`GameLoop::run()` blocks the calling thread. The main thread becomes the RT thread — no artificial sleep, no extra thread to manage. All other subsystems start their own threads before `run()` is called:

```cpp
wsServer.start();     // spawns WS thread
httpServer.start();   // spawns HTTP thread(s)

gameLoop.run();       // main thread IS the loop — blocks until stopped

httpServer.stop();
wsServer.stop();
```

Signal handling sets an atomic flag that `run()` checks after each cycle:

```cpp
std::signal(SIGINT,  [](int) { running = false; });
std::signal(SIGTERM, [](int) { running = false; });
```

**Cycle timing — platform-specific behind a common interface**

```cpp
class CyclicTimer {
public:
    explicit CyclicTimer(std::chrono::microseconds period);
    void waitForNextCycle();  // blocks until next absolute deadline
};
```

Linux — `clock_nanosleep` with `CLOCK_MONOTONIC` in absolute mode (`TIMER_ABSTIME`). Absolute mode is critical: sleep target is always the next fixed deadline so drift never accumulates. Works on standard kernels; becomes hard real-time with `SCHED_FIFO` + `CONFIG_PREEMPT_RT`.

```cpp
// cyclic_timer_linux.cc
CyclicTimer::CyclicTimer(std::chrono::microseconds period)
    : periodNs_(period.count() * 1000) {
    clock_gettime(CLOCK_MONOTONIC, &next_);
}

void CyclicTimer::waitForNextCycle() {
    next_.tv_nsec += periodNs_;
    if (next_.tv_nsec >= 1'000'000'000) {
        next_.tv_nsec -= 1'000'000'000;
        next_.tv_sec++;
    }
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_, nullptr) == EINTR) {}
}
```

Windows — `CreateWaitableTimerEx` with `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` (Win10 2004+). Sub-millisecond resolution without busy-waiting. RT not required on Windows.

```cpp
// cyclic_timer_windows.cc
CyclicTimer::CyclicTimer(std::chrono::microseconds period) {
    handle_ = CreateWaitableTimerEx(nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due{.QuadPart = -period.count() * 10};  // 100ns units
    SetWaitableTimer(handle_, &due, period.count() / 1000, nullptr, nullptr, FALSE);
}

void CyclicTimer::waitForNextCycle() {
    WaitForSingleObject(handle_, INFINITE);
}
```

**GameLoop::run()**

```cpp
void GameLoop::run() {
    setRealtimePriority();  // SCHED_FIFO on Linux, no-op on Windows

    CyclicTimer timer(periodUs_);

    while (running) {
        timer.waitForNextCycle();

        deviceManager_.pdoExchange();
        updateDeviceParameters();   // write fresh PDO values into devices (seqlock)

        for (auto* task : tasks_) {
            task->execute();        // Watchdog, MonitorPublisher, etc.
        }
    }

    deviceManager_.stop();
}
```

`EINTR` from the signal interrupts `clock_nanosleep`, the retry loop re-enters, then on the next iteration `running` is false and the loop exits cleanly.

---

## Session 2026-05-16 — Project structure, toolchain, namespaces

**Toolchain — keep from current project**

- CMake + CMakePresets — modern, consistent builds across machines and CI; keep all four presets (`x64-linux-debug`, `x64-linux-release`, `x64-windows-debug`, `x64-windows-release`)
- vcpkg — right choice for C++ dependency management; `vcpkg_installed/` in `.gitignore`
- clang-format + cpplint — automated style enforcement checked in alongside code
- Doxygen — documentation generation
- GTest — unit testing
- CI split into separate workflows: lint, test, release, docker

**Directory layout**

```
motion-master/
  apps/
    motion-master/        ← main executable; flat file layout within
    playground/           ← scratch binary
  libs/
    core/                 ← ThreadSafeQueue, seqlock, util, platform timers
    comm/                 ← flat layout; soem.cc, spoe.cc, igh.cc alongside base.h
  extern/
    SOEM/                 ← git submodule
  tools/                  ← all helper scripts; nothing operational at repo root
  .github/workflows/
  CMakeLists.txt
  CMakePresets.json
  vcpkg.json
  .clang-format
  .cpplintrc
  Doxyfile
  .gitignore
```

Flat layout within each app and lib is intentional — files are navigated by name and grep, not by folder hierarchy. Keep the number of files manageable by keeping each library focused.

**Naming convention — hyphens vs underscores**

- Repo name and top-level folder: `motion-master` (hyphen) — follows git/GitHub convention and matches the domain name
- Everything inside the repo (source files, directories): `lowercase_with_underscores` — hyphens cannot appear in C++ identifiers, so `soem_driver.cc` pairs naturally with `SoemDriver`; `soem-driver.cc` breaks that relationship. Consistent with the Google C++ Style Guide.

**What to avoid from the current codebase**

- Private keys or certificates committed to the repo — add `*.key` and `*.pem` to `.gitignore` immediately; never commit cryptographic material
- Bloated `.gitignore` based on a Visual Studio template — write a lean, project-specific file covering only what this project actually produces
- Paired `.sh`/`.ps1` scripts duplicated at root — replace with CMake custom targets (`format`, `lint`, `docs`) so there is one way to do things on both platforms; put any remaining scripts in `tools/`
- Two `util` files in different locations — all cross-cutting utilities live in `libs/core/`; nothing utility-shaped goes into an app directory

**Dependencies — changes from current**

- `loguru` — move from `extern/loguru` git submodule to vcpkg; cleaner dependency management, no manual submodule updates
- `std::expected` — use from the standard library (C++23); no third-party `tl::expected` needed; bump the CMake `CMAKE_CXX_STANDARD` to 23

**Namespaces vs modules**

Use namespaces. Do not use C++20 modules yet.

Modules and namespaces are orthogonal — a module exports symbols inside a namespace — but adopting modules now creates problems with no payoff:

- All vcpkg dependencies (Boost, uWebSockets, protobuf, GTest) ship header interfaces, not module interfaces. The codebase would mix `import mm.core;` with `#include <boost/asio.hpp>` throughout — the messy middle ground with none of the clean-break benefits.
- CMake module support landed in 3.28 (late 2023), requires the Ninja generator, and still has rough edges.
- Clang module support has been historically fragile; cross-compiler consistency between GCC, Clang, and MSVC adds build risk.

Namespace structure mirrors the directory layout:

```cpp
namespace mm::core { }
namespace mm::comm::soem { }
namespace mm::comm::spoe { }
namespace mm::node { }
namespace mm::api { }
```

---

## Session 2026-05-19 — RT jitter benchmark (`hil/jitter_bench`)

**Why a standalone jitter bench**

The `GameLoop` uses `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` and `SCHED_FIFO` priority 80 for its cycle timer. Characterising actual OS scheduling latency on a given kernel (standard vs `PREEMPT_RT`) cannot be done in a unit test — it requires real RT privileges, a real kernel, and enough cycles to observe the tail distribution. The `hil/` folder was created for exactly this class of test.

**Measurement: cycle-to-cycle jitter**

The metric is `(t[i] - t[i-1]) - period_ns`, where `t[i]` is a `clock_gettime(CLOCK_MONOTONIC)` timestamp taken immediately after `waitForNextCycle()` returns. This is cycle-to-cycle jitter: how much each actual interval deviated from the nominal period.

Absolute jitter (each sample vs a fixed baseline `t[0] + i * period`) was considered. It is more sensitive to cumulative effects but has a practical problem: a single large spike early in the run shifts the "expected" timeline for all subsequent samples, inflating apparent jitter throughout. Cycle-to-cycle is the standard metric in RT scheduling analysis and is immune to this artifact.

With `CLOCK_MONOTONIC + TIMER_ABSTIME`, jitter should always be ≥ 0 — `clock_nanosleep` never returns before the deadline. Negative values would indicate clock aliasing or measurement overhead that rounds negative; in practice they do not appear.

**Implementation: `CyclicTimer` directly, not `GameLoop`**

`GameLoop` lives in `apps/motion_master/` (not a library) and depends on `spdlog`. Linking `jitter_bench` against it would require either extracting `GameLoop` + `ICyclicTask` into a shared library (`mm_rt`) or copying source files into `hil/`. Neither is warranted for a single-file bench tool.

The bench re-implements the same loop: construct `mm::core::CyclicTimer`, call `setRealtimePriority()` (identical to `GameLoop::run()`), then loop on `waitForNextCycle()`. The timer path is byte-for-byte identical to production. If future HIL tests need to run real `ICyclicTask` implementations, extracting a `mm_rt` library is the right move at that point.

**`--workload` option**

A CPU-bound spin-wait (spinning on `clock_gettime`) simulates task execution time without yielding the CPU. This is realistic: the actual per-cycle work (PDO exchange, state machine updates, monitoring publish) is CPU-bound. The option lets you answer: does 300 µs of work in a 1 ms cycle cause overruns on this kernel? Overruns are defined as `|jitter| > one period` — the next `waitForNextCycle()` returned immediately because we had already passed its absolute deadline.

**Output**

CSV: `cycle`, `elapsed_ms`, `jitter_ns`. Python plot script (`plot_jitter.py`): two-panel figure with a time-series (line chart with P99/P99.9 reference lines) and a histogram clipped at 1.5× P99.9 so extreme spikes do not compress the main distribution. Both the bench and the plot script print a statistics table: min, max, mean, stddev, P50, P95, P99, P99.9.

---

## Session 2026-05-19 — HTTP API integration tests (`hil/api`) and Docker lifecycle

**Why `hil/api`**

Unit tests verify individual C++ components in isolation. The `hil/api` suite exercises the full running server — HTTP response shapes, status codes, semver-valid version strings, and WebSocket message framing — things that only manifest when the complete stack is assembled. These tests live in `hil/` because they, like `jitter_bench`, cannot run in CTest: they require a live server process and, optionally, real OS primitives.

**Docker as the test fixture**

The server is started as a Docker container managed entirely by the Vitest global setup (`src/global-setup.ts`). On `setup()`: remove any stale container by name, `docker build` from the repo root, `docker run -d --rm --network host`. On `teardown()`: `docker stop` (the `--rm` flag handles removal). This means `npm test` is self-contained — no manual server startup, no leaked processes.

`--network host` is the correct flag. Motion Master binds to `127.0.0.1:8443` by design (loopback only); Docker's default bridge NAT routes through `eth0` and would never reach a loopback listener. Host networking is the minimal footprint that makes the connection work, and it is appropriate for a local-only dev server.

**`MM_SKIP_DOCKER=1`**

Bypasses the Docker lifecycle and polls an already-running instance. Useful when iterating locally with `./tools/run.sh` in a separate terminal — avoids the rebuild cost on every test run.

**TLS**

The container entrypoint generates a self-signed certificate for `local.motion-master.synapticon.com` on each start. The test suite sets `NODE_TLS_REJECT_UNAUTHORIZED=0` to accept it. This is acceptable because the tests run against localhost and certificate pinning is not a testing concern here.

Revisit modules when vcpkg packages start shipping module interfaces and CMake support matures. The namespace-to-module rename is mechanical at that point.

---

## Session 2026-05-22 — Device, DeviceManager, SlaveInfo, and fieldbus driver selection

**No NetworkScanner**

There will be no separate `NetworkScanner` class. `DeviceManager` owns slave discovery and network scanning directly via `FieldbusDriver`. This keeps the dependency graph flat — one fewer class, no forwarding methods.

**FieldbusDriver startup sequence**

Three calls in order:

1. `init()` — opens the NIC (`ecx_init`).
2. `scan()` — discovers slaves and configures SM/FMMU (`ecx_config_init`). Sets `manualstatechange = 1` so slaves remain in INIT; all EtherCAT state transitions are left entirely to the caller (HTTP API or test code). Returns the slave count on success.
3. `exchangeProcessData()` — called each game loop cycle once slaves are in OP.

**SlaveInfo — immutable identity from EEPROM**

After `scan()`, SOEM has read each slave's SII EEPROM. These fields do not change for the lifetime of the session and are captured immediately into `SlaveInfo`:

```cpp
struct SlaveInfo {
  std::string name;
  uint32_t vendorId;       // eep_man
  uint32_t productCode;    // eep_id
  uint32_t revisionNumber; // eep_rev
  uint32_t serialNumber;   // eep_ser
};
```

`Device` reads `SlaveInfo` from the driver in its constructor and stores the values as plain members. This avoids repeated SOEM lookups and makes identity always available without driver access.

**Device**

`Device` holds a 1-based `slavePosition` (SOEM's slave array index; 0 is the master) and a `FieldbusDriver&` for SDO and state operations. Immutable identity fields (`name`, `vendorId`, `productCode`, `revisionNumber`, `serialNumber`) are populated from `SlaveInfo` at construction.

**DeviceManager::scan() populates devices_**

`DeviceManager::scan()` calls `driver_.scan()`, then constructs one `Device` per slave (positions 1..n) and stores them in `devices_`. This is the single place where the device list is created.

**Driver selection and deferred initialisation**

`--driver` and `--adapter` are optional at startup. If `--driver` is given, `main.cc` constructs the concrete driver and immediately calls `deviceManager.init(std::move(driver))` + `scan()`; the app starts with devices ready. If omitted, the app starts in an uninitialised state and the HTTP API is used to initialise later.

`DeviceManager` owns the driver via `unique_ptr<FieldbusDriver>` (null until `init()` is called). Adding a new driver type is an `else if` in `main.cc`'s `makeDriver` lambda; `DeviceManager` has no knowledge of the concrete type.

**Linux capabilities**

`motion-master` requires `cap_net_raw` (raw EtherCAT socket) and `cap_sys_nice` (SCHED_FIFO). `tools/build.sh` runs `sudo setcap cap_sys_nice,cap_net_admin,cap_net_raw=eip` on the binary after linking so developers do not need to run the binary as root.

---

## Session 2026-05-22 — libs/node and the mm::node library

**Why a separate library layer**

`mm::comm` is the transport/protocol layer: `FieldbusDriver` (abstract interface), `SoemFieldbusDriver`, `SpoeDriver`. It is stable and protocol-focused. `Device` and `DeviceManager` are the device model — and as CiA402, drive profiles, and encoder calibration are added, they accumulate application-layer logic that does not belong in a transport library.

`libs/node/` (`mm::node`) is introduced as the distributable device-model layer above `mm::comm`. External users who want to build their own tooling on top of Motion Master receive `mm::comm` + `mm::node` as the SDK. They get `Device`, `DeviceManager`, CiA402 state machines, profiles, and encoder support without reimplementing any of it.

**Why `mm::node` and not `mm::devices`, `mm::ethercat`, or `mm::drives`**

- `mm::devices` — too generic; could mean anything.
- `mm::ethercat` — excludes SPoE, which is not EtherCAT but works over the same `FieldbusDriver` abstraction.
- `mm::drives` — accurate for CiA402 servo drives but breaks when I/O modules, sensors, or other non-drive nodes are added.
- `mm::node` — the standard protocol-agnostic term for any addressable device on a fieldbus. Scales to drives, I/O modules, sensors equally.

**Layer summary**

```
libs/comm/   mm::comm   ← EtherCAT/SPoE transport: FieldbusDriver interface + concrete drivers
libs/node/   mm::node   ← Device model: Device, DeviceManager, CiA402, profiles, encoder
apps/        (app layer) ← GameLoop, HttpServer, WebSocket, CLI — thin shell, not distributable
```

`mm::node` links `mm::comm` as a PUBLIC dependency so its include paths propagate to any target that links `mm::node`.

---

## Session 2026-05-22 — Deferred fieldbus initialisation and HTTP lifecycle API

**Motivation**

Previously the app required `--driver` and could not start without a functioning EtherCAT adapter. This prevented headless deployment scenarios and made the app harder to test without hardware. The fieldbus lifecycle is now fully controllable via the HTTP API.

**Ownership change: DeviceManager owns FieldbusDriver**

`DeviceManager` changed from holding a `FieldbusDriver&` (non-owning reference) to owning a `unique_ptr<FieldbusDriver>` (null until initialised). The driver is never constructed inside `DeviceManager` — `main.cc` creates the concrete type and transfers ownership via `DeviceManager::init(unique_ptr<FieldbusDriver>)`. This keeps the composition-root rule intact.

`reset()` ordering is now mechanically enforced: `devices_.clear()` first (so `Device` objects drop their `FieldbusDriver&` before the driver stops), then `driver_->stop()`, then `driver_.reset()`. With the old reference-based design this ordering was a soft contract enforced only by convention.

**Server::Config::InitDriverFn**

`Server::Config` carries an `InitDriverFn` callback (`std::function<std::expected<void, std::string>(std::string driver, std::string adapter)>`). The lambda is wired in `main.cc` and creates the concrete driver, then calls `deviceManager.init()`. The server only knows the abstract callback — no concrete driver type leaks past the composition root.

**New HTTP lifecycle endpoints**

| Endpoint | Body | Effect |
|---|---|---|
| `POST /api/init` | `{"driver":"soem","adapter":"eth0"}` (adapter optional) | Creates driver, calls `DeviceManager::init()` |
| `POST /api/scan` | — | Calls `DeviceManager::scan()`; returns `{"slaves": N}` |
| `POST /api/reset` | — | Calls `DeviceManager::reset()`; releases driver |
| `POST /api/state` | `{"state":8,"positions":[1,2],"timeout":5000}` (positions/timeout optional) | Calls `DeviceManager::transitionToState()` |

`GET /api/devices` returns an empty array when uninitialised; all other behaviour is unchanged.

**GameLoop start**

The GameLoop starts unconditionally regardless of whether a driver is present. `pdoExchange()` is a no-op when `driver_` is null, so the loop runs safely in the uninitialised state.

**Thread safety — open issue**

`POST /api/init`, `POST /api/scan`, and `POST /api/reset` run on the HTTP server thread and mutate `driver_` and `devices_`. `pdoExchange()` runs on the RT GameLoop thread and reads both. There is currently no lock guarding this boundary. This is safe only because `pdoExchange()` is not yet wired into the GameLoop. Before enabling live PDO exchange, the loop must be stopped (or drained for one cycle) before `init()` or `reset()` is called via the API.

---

## Session 2026-05-23 — TLS certificate automation

**Problem**

The PWA at `https://motion-master.synapticon.com` targets `https://local.motion-master.synapticon.com:8443`. Because the PWA is served over a real HTTPS origin, browsers enforce strict certificate validation. The previous `tools/run.sh` generated a self-signed cert on every start, causing `ERR_CERT_AUTHORITY_INVALID` in the browser.

**Solution: Let's Encrypt via DNS-01 + acme-dns delegation**

A real Let's Encrypt cert is issued for `local.motion-master.synapticon.com` using DNS-01. HTTP-01 is not viable because the domain resolves to `127.0.0.1` and Let's Encrypt's validators cannot reach localhost. DNS-01 only requires a publicly visible TXT record — no inbound connectivity.

Automating the DNS-01 challenge without direct DNS API access uses **acme-dns**: a small service that holds ACME challenge TXT records and exposes a simple update API. A one-time permanent CNAME is added to the main zone:

```
_acme-challenge.local.motion-master.synapticon.com
  → CNAME → 4723b93a-99f5-43d7-93f1-195dbb4168ea.auth.acme-dns.io
```

When Let's Encrypt validates, it follows the CNAME and reads the TXT record from `auth.acme-dns.io`. The acme-dns account credentials are stored as the GitHub Secret `ACMEDNS_CONFIG` (JSON). The `acme.sh` tool with its `dns_acmedns` plugin updates the challenge record over the acme-dns REST API automatically — the main DNS zone (`synapticon.com`) is never touched again.

**cert-renewal.yml**

Runs on the 1st of every month via `schedule`. Installs `acme.sh`, writes `~/.acmedns.json` from the `ACMEDNS_CONFIG` secret, issues a fresh cert with `--issue --force --dns dns_acmedns --server letsencrypt`, then updates two repository secrets via `gh secret set` using a PAT (`GH_PAT_SECRETS`) with Secrets read/write permission:

- `TLS_CERT` — full-chain PEM (renewed cert + Let's Encrypt intermediate)
- `TLS_KEY` — EC private key

**release.yml**

Triggered by `v*` tag pushes. Builds with the `x64-linux-release` CMake preset, reads `TLS_CERT` and `TLS_KEY` from secrets, writes them as `cert.pem`/`key.pem` into the build output directory, then packages `motion-master`, `swagger.yml`, `cert.pem`, and `key.pem` into `motion-master-<version>-linux-x64.tar.gz` and publishes a GitHub Release.

**tools/run.sh cert discovery order**

1. `cert.pem` / `key.pem` next to the binary — present in release installs
2. `~/.acme.sh/local.motion-master.synapticon.com_ecc/fullchain.cer` + `.key` — present on developer machines with `acme.sh` installed; renewed automatically by the cron job `acme.sh` registers on install
3. Self-signed fallback — generated fresh each run; browsers require a one-time exception

**Private key in release artifact**

The key is bundled alongside the binary in every release (effectively public). This is acceptable: the domain always resolves to `127.0.0.1`, so an attacker with the key can only serve HTTPS on their own loopback interface — not intercept traffic between a user's PWA and their own Motion Master instance.

---

## Session 2026-05-23 — DeviceManager::transitionToState and POST /api/state

**DeviceManager::transitionToState**

`DeviceManager` now exposes `transitionToState` as a thin delegation to `FieldbusDriver::transitionToState`. Requires both `init()` and `scan()` to have been called — `init()` opens the NIC and `scan()` configures slave addresses and the io map; neither is meaningful without the other.

```cpp
std::expected<void, std::string> transitionToState(
    const std::vector<uint16_t>& positions,
    mm::comm::EtherCatState targetState,
    std::chrono::steady_clock::duration timeout);
```

- If `positions` is empty, all entries in `devices_` are targeted.
- Returns an error string if no driver is initialised (`driver_` is null) or no devices have been discovered (`devices_` is empty).
- Devices that do not reach the target state within `timeout` are logged at error level; the call still returns successfully — the caller can inspect device state via `GET /api/devices` if needed.
- `requiredState` (pre-filter) and `tick` (watchdog keepalive) are not exposed at the `DeviceManager` level for now; they are implementation details of the driver that will be wired in when live PDO exchange is enabled.

**POST /api/state**

```
POST /api/state
{"state": 8, "positions": [1, 2], "timeout": 5000}
```

`state` uses the standard ETG.1000.6 AL control register encoding: 1 (Init), 2 (PreOp), 3 (Boot), 4 (SafeOp), 8 (Op). Numbers were chosen over strings because these values are well-known to EtherCAT engineers and map directly to the wire protocol. `positions` and `timeout` are optional; omitting `positions` targets all discovered devices, `timeout` defaults to 5000 ms.

**Firmware update lifecycle — future work**

The planned firmware update flow is:
1. Transition target device to BOOT (other devices continue PDO exchange normally).
2. Flash firmware over the Boot mailbox.
3. Device returns to INIT — at this point it is stale: potentially a new PDO layout and updated object dictionary.
4. Future `DeviceManager::reintegrate(slavePosition)` will: re-run `ec_config_map()` for that slave's io map slot, refresh the `Device`'s parameter list from the updated OD, then call `transitionToState` to bring it back to Op.

Step 4 is not yet implemented. `transitionToState` as shipped covers steps 1 and 3.
