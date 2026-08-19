# NEXTGEN

## Feature List

- Start the process without initializing devices. This enables API endpoints that return the Motion Master version and scan the network for available devices.
- HTTP API handles simple requests such as version info and network scanning, but also device initialization, device listing, and SDO read/write operations including file transfer.
- A single WebSocket connection for monitoring: streams values only, plus notifications (e.g. number of slaves changed, devices leaving OP state due to a watchdog timeout). Monitoring configuration is exposed through the HTTP API. Multi-axis monitoring is required.
- No complex Protobuf message structures as in the current implementation — SDO values are plain buffers, numbers, or strings; monitoring payloads are arrays of numbers.
- Careful, safe multi-threaded programming and memory management throughout.
- Motion Composer integrated into Motion Master.
- All helper/utility functions included.
- HTTP API compatible with the existing Motion Master API where possible.
- Real-time support on Linux.
- No exceptions — use `std::expected<T, std::string>` instead. `std::string` is the **default** error type, kept everywhere a caller only logs, forwards, or shows the error. Structured errors are introduced **per API surface, not globally**, and only once a caller demonstrably needs to *branch* on the failure reason — the trigger is concrete (someone reaching for `error.contains("timeout")` to decide control flow), not a guess made up front. When that happens, promote just that function's error type to a small POD `struct Error { ErrorKind kind; std::string message; }` (an enum + message — **never** a class hierarchy, e.g. a `SdoError` whose `kind` distinguishes NotFound / Timeout / AbortCode / WrongState and whose `message` still carries the abort code for logs). Design `Error` to stay interchangeable with `std::string` at call sites: an `operator<<` plus `.message`/`.what()` mean the existing `ASSERT_TRUE(r) << r.error()` test idiom and `sendError(r.error())` HTTP path keep working, so promoting one function never ripples through its callers. A deliberate global sweep to a shared `Error` type is **rejected**: it forces either one giant cross-layer error enum or a conversion shim at every boundary, and it breaks `.and_then()` composition (mismatched error types) everywhere for the benefit of the few surfaces that actually branch. Uniform `std::string` elsewhere is the feature, not tech debt.
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

> This was the initial brainstorm list. The **as-built** class diagram and threading model now live in
> `CLAUDE.md` ("Class Structure" + "Game Loop / RT Threading") and are reconciled against the code in the
> *Session 2026-06-08 — Diagram & threading reconciled with code* note below. The detailed design diagram
> in *Session 2026-05-16* is kept as a historical reasoning trail; read the reconciliation note for what
> actually shipped vs. what is still planned.

Monitoring · DeviceManager / Master · Device / Slave · DeviceParameter / Object Dictionary Entry ·
GameLoop · HttpServer · WebSocketServer
*(Original list also named a `Watchdog` class — that became the ESC hardware sync-manager watchdog
diagnostics on `DeviceManager`/`FieldbusDriver`, not a standalone RT task.)*

---

## Session 2026-05-16 — HTTPS, WebSocket security, and PWA connectivity

### Deployment model

Motion Master is distributed as a Windows installer and Linux `.deb`/`.rpm` packages. There is no Electron wrapper. The UI is a PWA served from `https://motion-master.synapticon.com`. The PWA connects to the locally-running Motion Master process over HTTPS and WSS.

### TLS — subdomain resolving to localhost

The DNS record `local.motion-master.synapticon.com A 127.0.0.1` is maintained by Synapticon. Motion Master bundles a real TLS certificate for that hostname issued by Let's Encrypt (or a paid CA). Because the cert is signed by a real CA, browsers trust it with no warnings and no install steps required from the customer.

Motion Master binds exclusively to `127.0.0.1:8443`. Traffic never leaves the machine — the DNS lookup returns `127.0.0.1` and the connection is loopback-only.

Cert renewal must be automated in the CI/CD pipeline via a DNS-01 ACME challenge and the renewed cert bundled into each release. With Let's Encrypt the cert expires every 90 days; a paid CA (DigiCert, Sectigo) provides 1-year validity.

This cert is completely independent of Synapticon's existing `*.synapticon.com` wildcard cert. That cert lives on Synapticon's web servers and is used to serve `motion-master.synapticon.com`; its private key never leaves Synapticon's infrastructure. The two certs share the `synapticon.com` domain name only by convention. Note also that the existing wildcard cannot be reused here — `*.synapticon.com` matches only a single label deep (e.g. `foo.synapticon.com`) and does not cover `local.motion-master.synapticon.com`.

### CORS

Motion Master sets `Access-Control-Allow-Origin: https://motion-master.synapticon.com` on all responses. This prevents any other website from making browser-based requests to the local API. CORS is a browser-enforced mechanism — it does not block non-browser clients (curl, local scripts, other processes).

### PWA connectivity

The PWA at `https://motion-master.synapticon.com` connects to:

- `https://local.motion-master.synapticon.com:8443` — HTTP API
- `wss://local.motion-master.synapticon.com:8443` — monitoring WebSocket

Port 8443 is fixed and well-known; no discovery mechanism is needed.

> **Superseded by Session 2026-06-06 — HTTP and WebSocket on separate ports/loops.** The WebSocket now runs on its own port (`wss://…:62281`, `--ws-port`) and event loop so a blocking HTTP handler can't stall it; the HTTP API is on `61447`. (Defaults moved off 8443/8444 — see that session.)

### Bearer token (optional — not implemented in v1)

CORS covers the primary threat (other websites controlling drives via the browser). A bearer token would additionally block non-browser local processes (malware, rogue scripts) from talking to the API — relevant because Motion Master controls physical hardware.

If added: Motion Master generates a random token on first install and persists it in the user config directory (`%APPDATA%\MotionMaster\token` on Windows, `~/.config/motion-master/token` on Linux). A system tray "Open Web UI" action opens the browser to `https://motion-master.synapticon.com/?token=<token>`. The PWA stores the token in `localStorage` and sends it as `Authorization: Bearer <token>` on all requests. Subsequent visits read the token from `localStorage` automatically.

---

## Session 2026-05-16 — Class diagram, device hierarchy, naming

### Revised class diagram

Based on reviewing the current source (`comm::base::Device`, `VirtualDevice`, `EthercatMaster`, `Cia402Drive`, `DeviceParameterRefresher`, etc.) the main structural problem is two overlapping device abstractions. NEXTGEN has one.

```text
App  (composition root, owns everything)
 ├── Config
 ├── DeviceManager                (owns FieldbusDriver + Device[]; drives scanning)
 │     ├── unique_ptr<FieldbusDriver>  ← SoemFieldbusDriver | SpoeFieldbusDriver; owns mutex
 │     │                                 null until init(); set via init(unique_ptr<FieldbusDriver>)
 │     ├── owns: Device[]         (each Device holds FieldbusDriver&)
 │     │     ├── DeviceType  { Cia402Drive, DigitalIo }
 │     │     ├── owns: DeviceParameter[] (index/subindex → DeviceParameterValue variant)
 │     │     ├── owns: PdoMappings
 │     │     └── owns: Cia402StateMachine  (only if Cia402Drive)
 │     └── init(unique_ptr<FieldbusDriver>), scan(), reset(), exchangeProcessData(), state transitions
 ├── GameLoop  (RT thread, SCHED_FIFO, 1ms)
 │     ├── uses: DeviceManager    (calls exchangeProcessData each cycle; no-op when driver is null)
 │     ├── writes: Device parameters via seqlock
 │     └── runs: ICyclicTask[]
 │           ├── Watchdog           → NotificationBus
 │           └── MonitorPublisher   → WebSocketServer
 ├── HttpServer  (own port 61447 + loop/thread)
 │     ├── uses: DeviceManager    (SDO read/write, file transfer, state control)
 │     └── Config.InitDriverFn    (callback to main.cc; creates concrete driver for POST /api/init)
 ├── WebSocketServer  (own port 62281 + loop/thread; realtime channel — monitoring/notifications/progress out, subscribe + output staging in)
 ├── NotificationBus  (observer; decouples Watchdog/DeviceManager from servers)
 └── FirmwareInstaller
       └── uses: DeviceManager
```

### What carries over from current code

- `DeviceParameter` with index, subindex, and a `DeviceParameterValue` — solid design, keep it
- `PdoMappings` with `rxPdos`/`txPdos` and `PdoMappingEntry` — keep as-is
- Socket-level mutex per slave — SDO calls from any thread are safe as long as FieldbusDriver owns and holds the mutex

### What changes

- `IEthercatDriver` → `IFieldbusDriver` — SPoE is not EtherCAT; the name was inaccurate
- `MainTimer` singleton → removed; `GameLoop` owns the timer directly
- Static `slaveMap_` in `EthernetMaster` → removed; `DeviceManager` owns `Device[]` directly
- `Cia402Drive` (currently just static maps/enums) → `Cia402StateMachine`, a proper owned component of `Device`

### Device hierarchy — inheritance vs composition

> **Superseded by Session 2026-06-05 — Device profiles as borrowed views.** The conclusion below (CiA402 as a subtype-or-component *of* `Device`; Somanet relegated to free functions because subclassing would force downcasting) was reasoned about the wrong ownership direction. The resolved model inverts it: profiles *borrow* a `Device&` rather than being owned *by* `Device`, which makes a full `SomanetDrive → Cia402Drive → ProfileDevice` inheritance chain correct. Read the later session; the notes here are kept only for the reasoning trail.

`Device → Cia402Drive` via inheritance: correct. The relationship is genuinely "is-a" — CiA402 is a standard and every such device has the same state machine, op modes, and control/status word bits. Shallow inheritance is appropriate.

`Cia402Drive → SomanetDevice` via inheritance: no. Somanet-specific features are not what the device *is*, they are what you *do with it* using knowledge of Somanet's OD layout. Subclassing here would force `DeviceManager` to know about `SomanetDevice` or require downcasting — both are signs the abstraction is wrong.

Instead:

- Simple Somanet-specific OD access → free functions in `namespace somanet`
- Complex multi-step procedures (encoder calibration, auto-tuning) → separate `ICyclicTask` implementations that take a `Cia402Drive&`

### Naming

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

### Dependency injection

`App` is the only place that instantiates concrete types. `FieldbusDriver` is injected into `DeviceManager` and `NetworkScanner`; `DeviceManager` passes a `FieldbusDriver&` into each `Device` it creates. `GameLoop` and `HttpServer` both receive a `DeviceManager&` — `GameLoop` calls `exchangeProcessData`, `HttpServer` calls SDO/file/state methods. Inject `NotificationBus` into `Watchdog`, `DeviceManager`, `WebSocketServer`.

---

## Session 2026-05-18 — Monitoring WebSocket protocol

### Message format

Two message types flow over the monitoring WebSocket:

```json
{"type": "monitoring", "topic": "pdos", "data": [1234567890, 39, 0, 12345]}
{"type": "notification", "data": {"event": "slaves_changed"}}
```

`data` in a monitoring message is a positionally-ordered array of numbers. The array order is stable for the lifetime of a monitoring session. Clients fetch the schema once via HTTP (e.g. `GET /api/monitoring/pdos`) and use the positional index to look up meaning — no keys are repeated in every 1 ms message.

```text
GET /api/monitoring/pdos → [{"index": "6064:00", "name": "actual_position"}, ...]
```

### Throughput

At ~40 × 32-bit PDO values per message, a monitoring message is approximately 450 bytes of JSON. At 1 ms cycles with up to 5 simultaneous clients, total loopback throughput is ~2.25 MB/s — negligible on loopback. Using an array rather than a key-value map halves the message size and keeps the high-frequency path minimal.

---

## Session 2026-05-18 — Configuration file format

### JSONC over plain JSON

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

### HTTP API + real-time loop

The RT loop runs on a `SCHED_FIFO` thread and owns the EtherCAT context. SDO operations (CoE mailbox) and PDO exchange (LRW datagrams) are separate EtherCAT mechanisms. `FieldbusDriver` owns `controlPlaneMutex_`, which serializes the **control-plane** operations (mailbox/SDO, FoE, ESC register, state access) amongst non-RT callers. The **PDO path (`exchangeProcessData`) is deliberately lock-free**: SOEM's port layer is internally thread-safe — each datagram gets a unique index (`ecx_getindex` under `getindex_mutex`), `tx_mutex`/`rx_mutex` are held only around a single non-blocking poll (`ecx_inframe`), and a frame received for another waiter's index is cooperatively routed to its buffer. PDO and the control plane also touch disjoint high-level state (process-data IOmap vs mailbox pool / slave state). So a PDO thread and an SDO/mailbox call can share one socket without our mutex, and the RT cycle is never stalled by a slow SDO or a multi-second object-dictionary enumeration. HTTP handlers call `device.readSdo()` / `device.writeSdo()` directly on their thread; each call takes `controlPlaneMutex_` for the duration of one transaction — never across a sleep, a blocking wait, or a user callback — so `readObjectDictionary` and `transitionToState` lock per transaction rather than for their whole run. The remaining contention is at SOEM's port layer (tx/rx mutex, ~µs per frame), an acceptable sub-cycle cost.

### Object dictionary threading

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

### Main thread is the game loop

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

### Cycle timing — platform-specific behind a common interface

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

### GameLoop::run()

```cpp
void GameLoop::run() {
    setRealtimePriority();  // SCHED_FIFO on Linux, no-op on Windows

    CyclicTimer timer(periodUs_);

    while (running) {
        timer.waitForNextCycle();

        deviceManager_.exchangeProcessData();
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

### Toolchain — keep from current project

- CMake + CMakePresets — modern, consistent builds across machines and CI; keep all four presets (`x64-linux-debug`, `x64-linux-release`, `x64-windows-debug`, `x64-windows-release`)
- vcpkg — right choice for C++ dependency management; `vcpkg_installed/` in `.gitignore`
- clang-format + cpplint — automated style enforcement checked in alongside code
- Doxygen — documentation generation
- GTest — unit testing
- CI split into separate workflows: lint, test, release, docker

### Directory layout

```text
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

### Naming convention — hyphens vs underscores

- Repo name and top-level folder: `motion-master` (hyphen) — follows git/GitHub convention and matches the domain name
- Everything inside the repo (source files, directories): `lowercase_with_underscores` — hyphens cannot appear in C++ identifiers, so `soem_driver.cc` pairs naturally with `SoemFieldbusDriver`; `soem-driver.cc` breaks that relationship. Consistent with the Google C++ Style Guide.

### What to avoid from the current codebase

- Private keys or certificates committed to the repo — add `*.key` and `*.pem` to `.gitignore` immediately; never commit cryptographic material
- Bloated `.gitignore` based on a Visual Studio template — write a lean, project-specific file covering only what this project actually produces
- Paired `.sh`/`.ps1` scripts duplicated at root — replace with CMake custom targets (`format`, `lint`, `docs`) so there is one way to do things on both platforms; put any remaining scripts in `tools/`
- Two `util` files in different locations — all cross-cutting utilities live in `libs/core/`; nothing utility-shaped goes into an app directory

### Dependencies — changes from current

- `loguru` — move from `extern/loguru` git submodule to vcpkg; cleaner dependency management, no manual submodule updates
- `std::expected` — use from the standard library (C++23); no third-party `tl::expected` needed; bump the CMake `CMAKE_CXX_STANDARD` to 23

### Namespaces vs modules

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

### Why a standalone jitter bench

The `GameLoop` uses `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` and `SCHED_FIFO` priority 80 for its cycle timer. Characterising actual OS scheduling latency on a given kernel (standard vs `PREEMPT_RT`) cannot be done in a unit test — it requires real RT privileges, a real kernel, and enough cycles to observe the tail distribution. The `hil/` folder was created for exactly this class of test.

### Measurement: cycle-to-cycle jitter

The metric is `(t[i] - t[i-1]) - period_ns`, where `t[i]` is a `clock_gettime(CLOCK_MONOTONIC)` timestamp taken immediately after `waitForNextCycle()` returns. This is cycle-to-cycle jitter: how much each actual interval deviated from the nominal period.

Absolute jitter (each sample vs a fixed baseline `t[0] + i * period`) was considered. It is more sensitive to cumulative effects but has a practical problem: a single large spike early in the run shifts the "expected" timeline for all subsequent samples, inflating apparent jitter throughout. Cycle-to-cycle is the standard metric in RT scheduling analysis and is immune to this artifact.

With `CLOCK_MONOTONIC + TIMER_ABSTIME`, jitter should always be ≥ 0 — `clock_nanosleep` never returns before the deadline. Negative values would indicate clock aliasing or measurement overhead that rounds negative; in practice they do not appear.

**Implementation: `CyclicTimer` directly, not `GameLoop`**

`GameLoop` lives in `apps/motion_master/` (not a library) and depends on `spdlog`. Linking `jitter_bench` against it would require either extracting `GameLoop` + `ICyclicTask` into a shared library (`mm_rt`) or copying source files into `hil/`. Neither is warranted for a single-file bench tool.

The bench re-implements the same loop: construct `mm::core::CyclicTimer`, call `setRealtimePriority()` (identical to `GameLoop::run()`), then loop on `waitForNextCycle()`. The timer path is byte-for-byte identical to production. If future HIL tests need to run real `ICyclicTask` implementations, extracting a `mm_rt` library is the right move at that point.

**`--workload` option**

A CPU-bound spin-wait (spinning on `clock_gettime`) simulates task execution time without yielding the CPU. This is realistic: the actual per-cycle work (PDO exchange, state machine updates, monitoring publish) is CPU-bound. The option lets you answer: does 300 µs of work in a 1 ms cycle cause overruns on this kernel? Overruns are defined as `|jitter| > one period` — the next `waitForNextCycle()` returned immediately because we had already passed its absolute deadline.

### Output

CSV: `cycle`, `elapsed_ms`, `jitter_ns`. Python plot script (`plot_jitter.py`): two-panel figure with a time-series (line chart with P99/P99.9 reference lines) and a histogram clipped at 1.5× P99.9 so extreme spikes do not compress the main distribution. Both the bench and the plot script print a statistics table: min, max, mean, stddev, P50, P95, P99, P99.9.

---

## Session 2026-05-19 — HTTP API integration tests (`hil/api`) and Docker lifecycle

**Why `hil/api`**

Unit tests verify individual C++ components in isolation. The `hil/api` suite exercises the full running server — HTTP response shapes, status codes, semver-valid version strings, and WebSocket message framing — things that only manifest when the complete stack is assembled. These tests live in `hil/` because they, like `jitter_bench`, cannot run in CTest: they require a live server process and, optionally, real OS primitives.

### Docker as the test fixture

The server is started as a Docker container managed entirely by the Vitest global setup (`src/global-setup.ts`). On `setup()`: remove any stale container by name, `docker build` from the repo root, `docker run -d --rm --network host`. On `teardown()`: `docker stop` (the `--rm` flag handles removal). This means `npm test` is self-contained — no manual server startup, no leaked processes.

`--network host` is the correct flag. Motion Master binds to `127.0.0.1:8443` by design (loopback only); Docker's default bridge NAT routes through `eth0` and would never reach a loopback listener. Host networking is the minimal footprint that makes the connection work, and it is appropriate for a local-only dev server.

**`MM_SKIP_DOCKER=1`**

Bypasses the Docker lifecycle and polls an already-running instance. Useful when iterating locally with `./tools/run.sh` in a separate terminal — avoids the rebuild cost on every test run.

### TLS

The container entrypoint generates a self-signed certificate for `local.motion-master.synapticon.com` on each start. The test suite sets `NODE_TLS_REJECT_UNAUTHORIZED=0` to accept it. This is acceptable because the tests run against localhost and certificate pinning is not a testing concern here.

Revisit modules when vcpkg packages start shipping module interfaces and CMake support matures. The namespace-to-module rename is mechanical at that point.

---

## Session 2026-05-22 — Device, DeviceManager, SlaveInfo, and fieldbus driver selection

### No NetworkScanner

There will be no separate `NetworkScanner` class. `DeviceManager` owns slave discovery and network scanning directly via `FieldbusDriver`. This keeps the dependency graph flat — one fewer class, no forwarding methods.

### FieldbusDriver startup sequence

Three calls in order:

1. `init()` — opens the NIC (`ecx_init`).
2. `scan()` — discovers slaves and configures SM/FMMU (`ecx_config_init`). Sets `manualstatechange = 1` so slaves remain in INIT; all EtherCAT state transitions are left entirely to the caller (HTTP API or test code). Returns the slave count on success.
3. `exchangeProcessData()` — called each game loop cycle once slaves are in OP.

### SlaveInfo — immutable identity from EEPROM

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

### Device

`Device` holds a 1-based `slavePosition` (SOEM's slave array index; 0 is the master) and a `FieldbusDriver&` for SDO and state operations. Immutable identity fields (`name`, `vendorId`, `productCode`, `revisionNumber`, `serialNumber`) are populated from `SlaveInfo` at construction.

### DeviceManager::scan() populates devices_

`DeviceManager::scan()` calls `driver_.scan()`, then constructs one `Device` per slave (positions 1..n) and stores them in `devices_`. This is the single place where the device list is created.

### Driver selection and deferred initialisation

`--driver` and `--adapter` are optional at startup. If `--driver` is given, `main.cc` constructs the concrete driver and immediately calls `deviceManager.init(std::move(driver))` + `scan()`; the app starts with devices ready. If omitted, the app starts in an uninitialised state and the HTTP API is used to initialise later.

`DeviceManager` owns the driver via `unique_ptr<FieldbusDriver>` (null until `init()` is called). Adding a new driver type is an `else if` in `main.cc`'s `makeDriver` lambda; `DeviceManager` has no knowledge of the concrete type.

### Linux capabilities

`motion-master` requires `cap_net_raw` (raw EtherCAT socket), `cap_sys_nice` (SCHED_FIFO), and `cap_ipc_lock` (`mlockall` memory pinning). `tools/build.sh --setcap` runs `sudo setcap cap_sys_nice,cap_net_admin,cap_net_raw,cap_ipc_lock=eip` on the binary after linking so developers do not need to run the binary as root.

---

## Session 2026-05-22 — libs/node and the mm::node library

### Why a separate library layer

`mm::comm` is the transport/protocol layer: `FieldbusDriver` (abstract interface), `SoemFieldbusDriver`, `SpoeFieldbusDriver`. It is stable and protocol-focused. `Device` and `DeviceManager` are the device model — and as CiA402, drive profiles, and encoder calibration are added, they accumulate application-layer logic that does not belong in a transport library.

`libs/node/` (`mm::node`) is introduced as the distributable device-model layer above `mm::comm`. External users who want to build their own tooling on top of Motion Master receive `mm::comm` + `mm::node` as the SDK. They get `Device`, `DeviceManager`, CiA402 state machines, profiles, and encoder support without reimplementing any of it.

**Why `mm::node` and not `mm::devices`, `mm::ethercat`, or `mm::drives`**

- `mm::devices` — too generic; could mean anything.
- `mm::ethercat` — excludes SPoE, which is not EtherCAT but works over the same `FieldbusDriver` abstraction.
- `mm::drives` — accurate for CiA402 servo drives but breaks when I/O modules, sensors, or other non-drive nodes are added.
- `mm::node` — the standard protocol-agnostic term for any addressable device on a fieldbus. Scales to drives, I/O modules, sensors equally.

### Layer summary

```text
libs/comm/   mm::comm   ← EtherCAT/SPoE transport: FieldbusDriver interface + concrete drivers
libs/node/   mm::node   ← Device model: Device, DeviceManager, CiA402, profiles, encoder
apps/        (app layer) ← GameLoop, HttpServer, WebSocket, CLI — thin shell, not distributable
```

`mm::node` links `mm::comm` as a PUBLIC dependency so its include paths propagate to any target that links `mm::node`.

---

## Session 2026-05-22 — Deferred fieldbus initialisation and HTTP lifecycle API

### Motivation

Previously the app required `--driver` and could not start without a functioning EtherCAT adapter. This prevented headless deployment scenarios and made the app harder to test without hardware. The fieldbus lifecycle is now fully controllable via the HTTP API.

### Ownership change: DeviceManager owns FieldbusDriver

`DeviceManager` changed from holding a `FieldbusDriver&` (non-owning reference) to owning a `unique_ptr<FieldbusDriver>` (null until initialised). The driver is never constructed inside `DeviceManager` — `main.cc` creates the concrete type and transfers ownership via `DeviceManager::init(unique_ptr<FieldbusDriver>)`. This keeps the composition-root rule intact.

`reset()` ordering is now mechanically enforced: `devices_.clear()` first (so `Device` objects drop their `FieldbusDriver&` before the driver stops), then `driver_->stop()`, then `driver_.reset()`. With the old reference-based design this ordering was a soft contract enforced only by convention.

### Server::Config::InitDriverFn

`Server::Config` carries an `InitDriverFn` callback (`std::function<std::expected<void, std::string>(std::string driver, std::string adapter)>`). The lambda is wired in `main.cc` and creates the concrete driver, then calls `deviceManager.init()`. The server only knows the abstract callback — no concrete driver type leaks past the composition root.

### New HTTP lifecycle endpoints

| Endpoint | Body | Effect |
| --- | --- | --- |
| `POST /api/init` | `{"driver":"soem","adapter":"eth0"}` (adapter optional) | Creates driver, calls `DeviceManager::init()` |
| `POST /api/scan` | — | Calls `DeviceManager::scan()`; returns `{"slaves": N}` |
| `POST /api/reset` | — | Calls `DeviceManager::reset()`; releases driver |
| `POST /api/state` | `{"state":8,"positions":[1,2],"timeout":5000}` (positions/timeout optional) | Calls `DeviceManager::transitionToState()` |

`GET /api/devices` returns an empty array when uninitialised; all other behaviour is unchanged.

### GameLoop start

The GameLoop starts unconditionally regardless of whether a driver is present. `exchangeProcessData()` is a no-op when `driver_` is null, so the loop runs safely in the uninitialised state.

### Thread safety — open issue

`POST /api/init`, `POST /api/scan`, and `POST /api/reset` run on the HTTP server thread and mutate `driver_` and `devices_`. `exchangeProcessData()` runs on the RT GameLoop thread and reads both. There is currently no lock guarding this boundary. This is safe only because `exchangeProcessData()` is not yet wired into the GameLoop. Before enabling live PDO exchange, the loop must be stopped (or drained for one cycle) before `init()` or `reset()` is called via the API.

---

## Session 2026-05-23 — TLS certificate automation

### Problem

The PWA at `https://motion-master.synapticon.com` targets `https://local.motion-master.synapticon.com:8443`. Because the PWA is served over a real HTTPS origin, browsers enforce strict certificate validation. The previous `tools/run.sh` generated a self-signed cert on every start, causing `ERR_CERT_AUTHORITY_INVALID` in the browser.

### Solution: Let's Encrypt via DNS-01 + acme-dns delegation

A real Let's Encrypt cert is issued for `local.motion-master.synapticon.com` using DNS-01. HTTP-01 is not viable because the domain resolves to `127.0.0.1` and Let's Encrypt's validators cannot reach localhost. DNS-01 only requires a publicly visible TXT record — no inbound connectivity.

Automating the DNS-01 challenge without direct DNS API access uses **acme-dns**: a small service that holds ACME challenge TXT records and exposes a simple update API. A one-time permanent CNAME is added to the main zone:

```text
_acme-challenge.local.motion-master.synapticon.com
  → CNAME → 4723b93a-99f5-43d7-93f1-195dbb4168ea.auth.acme-dns.io
```

When Let's Encrypt validates, it follows the CNAME and reads the TXT record from `auth.acme-dns.io`. The acme-dns account credentials are stored as the GitHub Secret `ACMEDNS_CONFIG` (JSON). The `acme.sh` tool with its `dns_acmedns` plugin updates the challenge record over the acme-dns REST API automatically — the main DNS zone (`synapticon.com`) is never touched again.

### cert-renewal.yml

*(The `~/.acmedns.json` step described here was a no-op — acme.sh reads `ACMEDNS_*` env vars, not that file. See Session 2026-06-06 — TLS cert auto-update for the fix and the rolling-release/self-heal additions.)*

*(The `gh secret set` / `TLS_CERT` / `TLS_KEY` / `GH_PAT_SECRETS` mechanism below was retired — see Session 2026-07-08. `cert-renewal.yml` now only publishes to the rolling `tls-cert` release, which is the single source of truth.)*

Runs on the 1st of every month via `schedule`. Installs `acme.sh`, writes `~/.acmedns.json` from the `ACMEDNS_CONFIG` secret, issues a fresh cert with `--issue --force --dns dns_acmedns --server letsencrypt`, then updates two repository secrets via `gh secret set` using a PAT (`GH_PAT_SECRETS`) with Secrets read/write permission:

- `TLS_CERT` — full-chain PEM (renewed cert + Let's Encrypt intermediate)
- `TLS_KEY` — EC private key

### release.yml

Triggered by `v*` tag pushes. Builds with the `x64-linux-release` CMake preset, reads `TLS_CERT` and `TLS_KEY` from secrets, writes them as `cert.pem`/`key.pem` into the build output directory, then packages `motion-master`, `cert.pem`, and `key.pem` into `motion-master-<version>-linux-x64.tar.gz` and publishes a GitHub Release. *(Superseded — since Session 2026-07-08 each leg `curl`s the cert/key from the rolling `tls-cert` release instead of reading secrets; the release is now multi-platform, see that session and the README CI table.)*

### tools/run.sh cert discovery order

1. `cert.pem` / `key.pem` next to the binary — present in release installs
2. `~/.acme.sh/local.motion-master.synapticon.com_ecc/fullchain.cer` + `.key` — present on developer machines with `acme.sh` installed; renewed automatically by the cron job `acme.sh` registers on install
3. Self-signed fallback — generated fresh each run; browsers require a one-time exception

### Private key in release artifact

The key is bundled alongside the binary in every release (effectively public). This is acceptable: the domain always resolves to `127.0.0.1`, so an attacker with the key can only serve HTTPS on their own loopback interface — not intercept traffic between a user's PWA and their own Motion Master instance.

---

## Session 2026-05-23 — DeviceManager::transitionToState and POST /api/state

### DeviceManager::transitionToState

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

### POST /api/state

```text
POST /api/state
{"state": 8, "positions": [1, 2], "timeout": 5000}
```

`state` uses the standard ETG.1000.6 AL control register encoding: 1 (Init), 2 (PreOp), 3 (Boot), 4 (SafeOp), 8 (Op). Numbers were chosen over strings because these values are well-known to EtherCAT engineers and map directly to the wire protocol. `positions` and `timeout` are optional; omitting `positions` targets all discovered devices, `timeout` defaults to 5000 ms.

### Firmware update lifecycle — future work

The planned firmware update flow is:

1. Transition target device to BOOT (other devices continue PDO exchange normally).
2. Flash firmware over the Boot mailbox.
3. Device returns to INIT — at this point it is stale: potentially a new PDO layout and updated object dictionary.
4. Future `DeviceManager::reintegrate(slavePosition)` will: re-run `ec_config_map()` for that slave's io map slot, refresh the `Device`'s parameter list from the updated OD, then call `transitionToState` to bring it back to Op.

Step 4 is not yet implemented. `transitionToState` as shipped covers steps 1 and 3.

## Session 2026-05-30 — Module ident reconcile (CoE Modular Device Profile)

**Problem.** A modular EtherCAT slave exposes two CoE Modular Device Profile lists (ETG.5001): the **Detected Module Ident List** (`0xF050`, what is physically plugged into each slot) and the **Configured Module Ident List** (`0xF030`, what the master expects). When the two disagree the slave reports a module mismatch and refuses to leave PRE-OP. With no ENI and no pre-engineered expected configuration, Motion Master's job is "talk to whatever drive is on the bus", so the configured list is meaningless to us — we clear the mismatch by copying detected into configured.

**`FieldbusDriver::writeSdo`.** SDO download was missing from the driver interface (only `readSdo` existed). Added:

```cpp
virtual std::expected<void, std::string> writeSdo(
    uint16_t slavePosition, uint16_t index, uint8_t subindex,
    std::span<const uint8_t> data) = 0;
```

`std::span<const uint8_t>` for the payload (read-only view, zero-copy, binds to vector/array/slice — matching `writeFile`/`writeRegister`), versus `readSdo` returning an owned `std::vector<uint8_t>`. `SoemFieldbusDriver::writeSdo` wraps `ecx_SDOwrite` under `controlPlaneMutex_` with the same SDO/mailbox/packet error decoding as `readSdo`. `Device::download(index, subindex, data)` is the per-device wrapper, mirroring `upload`.

**`reconcileDetectedModules(const Device&)`** — a free function in `mm::node` (declared in `device.h`):

1. Read `0xF050:00` (slot count). A slave with no detected-module list is simply not modular → return `0`, not an error.
2. For each slot `1..N`: read the detected ident `0xF050:sub`; skip empty (all-zero) and malformed (non-4-byte) entries; skip slots whose `0xF030:sub` already matches; otherwise write the detected ident into `0xF030:sub`.
3. Return the number of slots written, or an error string naming the slot(s) whose write failed.

Idempotent (the already-matches skip means re-running is a no-op) and **vendor-neutral**. This is the one notable departure from the "Somanet specifics live in `namespace somanet`" rule: the function is keyed purely on the standard MDP objects and never branches on vendor ID, so it belongs in the generic `mm::node` layer, not in `somanet`. It generalises the older single-slot, Synapticon-labelled approach to any modular EtherCAT device while still covering the Somanet case.

**Where it runs.** Wired into `DeviceManager::transitionToState`: when the target is PRE-OP, after the transition settles, the reconcile runs for every device that actually reached PRE-OP (`!error && alState == PreOp`). PRE-OP is the earliest point the write is possible — `0xF030`/`0xF050` are SDO mailbox objects, unavailable in INIT, and the mismatch must be cleared before SAFE-OP. Decided to make it **always-on** (no config flag, no separate HTTP route) and **best-effort**: failures are logged at warn level but never fail the transition.

**Open question — persistence.** The `0xF030` write is volatile on some firmwares (they require a `0x1010` store, or re-evaluate the list every boot). The current design re-runs on every PRE-OP transition, so it is self-healing regardless. If a mismatch is observed to reappear after a power cycle on real hardware, add an explicit store; until then the per-transition reconcile is sufficient and avoids unnecessary EEPROM wear.

## Session 2026-06-01 — DC sync diagnostics, and the remaining fieldbus surface

**DC sync health page.** Added a distributed-clock synchronisation diagnostic alongside the existing bus-health (ESC error-counter) one. `FieldbusDriver::readDcSync(positions)` (default "unsupported" for ESC-less transports; SOEM override) reads each DC-capable slave's **system-time delay (0x0928)** and **system-time difference (0x092C)** in one 8-byte FPRD. The reference clock is the first `hasdc` slave (SOEM elects it in `ecx_configdc`); its own difference is zero. 0x092C decodes as bits 0–30 magnitude + bit 31 sign → a signed-nanosecond deviation (positive = local clock ahead of the reference). Surfaced end-to-end: `mm::comm::DcSyncDiagnostics` → `DeviceManager::dcSync` + `DcSyncInfo`(+`to_json`) → `GET /api/dc-sync?positions=` → a polling **"DC Sync"** page under the **Fieldbus** sidebar group.

The figures are meaningful only while exchanging in SAFE-OP/OP: this stack runs DC in **free-run** (`ecx_configdc` measures and elects a reference, but `ecx_dcsync0` is deliberately *not* called — no SYNC0 pulse), yet `ecx_send_processdata` still distributes the reference system time via the cyclic FRMW, so the slaves' drift-compensation loops run and 0x092C converges toward zero. A value that stays large or grows means a slave is not locked.

**Roadmap — fieldbus capabilities not yet exposed.** The exposed surface is now bus-level Control / Configuration / Process Image / Diagnostics / DC Sync, plus per-device FoE / Parameters (CoE OD + SDO) / PDO Mapping (read + write, shipped 2026-07-06 — see that session) / Registers (ESC) / SII (EEPROM read). What remains, ranked by value-vs-effort, deferred for a later session:

*Tier 1 — high value, mostly presentation of data the driver already caches (read-only, no RT):*

1. **Topology / cabling map.** SOEM already caches per-slave `topology`, `activeports`, `consumedports`, `parent`, `parentport`, `entryport`, and the `DCnext`/`DCprevious` chain (`extern/.../soem/ec_main.h`). Combined with the per-port link state already read in Diagnostics (DL Status 0x0110), this renders the physical bus tree — line/ring/branch, hot-connect groups, which port connects to which neighbour. The view a field engineer reaches for first; spots a miscabled port instantly. Shape: a `busTopology()` driver method (cached read) + a tree/graph UI.
2. **Frame / WKC health timeline (master-side).** Process Image shows `lastWkc`/`expectedWkc` as a point value; the GameLoop gets a WKC every cycle. Accumulate master-side stats over time — WKC-mismatch count, lost frames, longest cycle overrun, "drops in the last minute" — to catch *intermittent* faults a point-in-time reading walks past. Distinct from the slave-side ESC counters. Pairs with the delta-tracking follow-up already noted for the Diagnostics page.

*Tier 2 — genuinely new information, moderate effort, read-mostly:*
3. **Diagnosis History — CoE 0x10F3 (ETG.1020).** The standardised per-slave event log: a ring buffer of timestamped diagnostic messages the slave itself recorded (error/warning/info + parameters) — the slave's own words, categorically different from the master-side counters. Built entirely on the existing SDO read; the work is decoding the message format. Confirm SOMANET firmware populates 0x10F3 before committing to it.
4. **Explicit device identification ("locate"/blink).** Command a slave to flash its ID LED so a tech can physically find it in a rack. Small, installer-friendly.

*Tier 3 — real capability, but write/RT/risk; deliberate actions, not toggles:*
5. **DC SYNC0 activation** (`ecx_dcsync0`) — turn on true DC-synchronous operation with configurable cycle/shift. The natural *control* counterpart to the DC Sync diagnostic above; what you'd do for tight coordinated multi-axis motion. RT implications. **Planned for ~Sept–Oct 2026 on a PREEMPT_RT Linux host** (the DC-locked-timer requirement spelled out at the end of this item is the game-loop-facing part of that work). Why it matters and why the timer alone isn't enough: the fixed absolute-deadline cyclic timer (Session 2026-07-13) supplies master *cadence* (a fresh frame on a drift-free 1 ms grid) but **not hardware synchronisation**. In today's free-run mode a drive acts on **frame arrival**, so the master's wake jitter *is* the actuation jitter. SYNC0 makes each drive latch process data on a hardware pulse derived from the synced DC clock instead — the master then only has to deliver the frame *before* the pulse with margin, and the pulse (not the timer's jitter) defines when the axis acts. It is the missing tier for hard coordinated multi-axis. **Necessary-but-not-sufficient — three pieces, not one:** (a) `ecx_dcsync0` to raise the pulse, (b) a **DC-locked cycle timer**, and (c) a PREEMPT_RT host. Piece (b) is the part that touches the game loop directly and is genuinely *new work*: today's `CyclicTimer` (Session 2026-07-13) anchors its deadline grid to the **local monotonic clock** (`CLOCK_MONOTONIC`/QPC/mach) — a *different crystal* from the elected DC reference clock. Free-run tolerates the two clocks drifting apart; SYNC0 does **not**. Under SYNC0 the process-data frame must arrive at each slave *before* that slave's pulse **every** cycle, so a free-running local grid — which slowly slides in phase against the DC pulse grid — will eventually deliver the frame on the wrong side of a pulse, and the drive latches stale data (sync error, AL 0x001A/0x001B, or SM watchdog). The fix is to make the timer a **servo on the local clock**: a per-cycle PI controller that reads the DC phase (the reference system time, or the 0x092C system-time difference the DC Sync page already surfaces) and nudges the next wake earlier/later to hold the frame at a fixed offset ahead of SYNC0 — the standard EtherCAT master-sync loop (TwinCAT/IgH do the same). The point to internalise: **enabling SYNC0 without the DC-locked timer + RT host is a regression, not an improvement** — on a jittery or free-running master it converts today's benign setpoint-*arrival* jitter into missed-window stale latches and outright drive faults. The period was never the missing piece (the fixed-deadline timer already nails drift-free cadence); the missing piece is *phase-locking that cadence to a second clock*. That servo, plus SYNC0 activation itself, is the ~Sept–Oct 2026 PREEMPT_RT-Linux work.
6. ~~**PDO remapping** — let the user change *which* objects are in the cyclic image (write `0x1C12`/`0x1C13` + the `0x160x`/`0x1A0x` mapping objects in PRE-OP), not just view the existing mapping. Most involved.~~ **SHIPPED 2026-07-06** — see the session note below.
7. **SII / station-alias write** (`ecx_siiwrite`) — assign station aliases / reflash EEPROM. Higher risk; SII is read-only today.

*Out of scope for SOMANET (absence is correct, not a gap):* cable redundancy, and the non-CoE mailbox protocols (EoE / SoE / AoE / VoE) — SOMANET is CoE-only.

Top pick when revisited: the **topology map** (#1) — near-pure presentation of already-cached data; **frame-health timeline** (#2) a close second, as it closes the one operational blind spot (intermittent faults).

## Session 2026-06-05 — Parameter access routing in `Device`, and a lock-free output path (Design B)

**What landed.** The PDO↔SDO routing was moved out of `DeviceManager` and into `Device`, so a single accessor — `device->readParameter` / `writeParameter` (and the typed `readValue`/`writeValue` wrappers) — serves the live value transparently and identically from an HTTP handler or an RT task. The motivating goal: a fork author writes their own `ICyclicTask`, gets a `Device` from `DeviceManager`, and just calls `device->readValue<int32_t>(0x6064, 0)` — that value comes from the IOmap when the device is exchanging, and from SDO otherwise, with the caller never choosing the transport.

The mechanism is composition, not an interface. `ProcessData` (the live process-data runtime — published image pointer + exchange seqlocks + working-counter health) was promoted from a `device_manager.cc`-local pimpl to a proper component in `node/process_data.h`, and given the two object-level accessors a `Device` needs:

```cpp
std::optional<std::vector<uint8_t>> ProcessData::readPdo(pos, index, subindex) const;  // health-gated
bool ProcessData::writePdo(pos, index, subindex, std::span<const uint8_t> bytes);
```

`DeviceManager` *owns* one `ProcessData` and hands each `Device` a `ProcessData*` at `scan()` (`nullptr` ⇒ SDO-only, so direct-construction unit tests are unchanged). `Device::readParameter` prefers `readPdo` when `exchangesProcessData()` (it returns `nullopt` when the object isn't PDO-mapped *or* the bus is unhealthy — a short working counter means the snapshot is stale — and the read falls through to the authoritative SDO upload); `Device::writeParameter` stages via `writePdo` when exchanging and output-mapped, else SDO. `DeviceManager::read/writeDeviceParameter` shrank to "resolve position + shared-lock + delegate" — the routing lives in one place. **No interface, no inheritance**: a single-implementation `ProcessDataAccess` abstraction was prototyped and deliberately discarded as over-engineering — `ProcessData` already *has* the access, so the concrete type is passed directly.

**The remaining wart: `writePdo` takes `stagingMutex`.** The output-staging seqlock has *many* writers (every non-RT HTTP thread, and — once step 2 lands RT tasks calling `setTargetPosition`) the RT thread too. `writePdo` is a read-modify-write (`load` the whole packed buffer → `insertBits` one object → `store`), and a `SeqLock` is single-producer / multi-consumer. The mutex serialises the producers, covering both the lost-update race and the seqlock's single-writer invariant. It is *not* yet on the RT path (today only non-RT threads write), but the moment an RT task writes a setpoint, that mutex becomes an unbounded block / priority-inversion risk on the RT cycle. The contrast is right there in the same struct: `inputSnapshot` has exactly **one** writer (the RT loop) and is already fully lock-free — that is the shape the output path must take.

**Decision: Design B — per-object lock-free slots, RT is the sole composer.** The root cause of the mutex is multiple writers editing one shared bit-packed buffer. Bit-packing composition *must* happen on one thread anyway (sub-byte objects share a byte), so we make the RT thread the only assembler and give every output object its own lock-free slot for writers to drop a value into independently.

`ProcessData` loses `outputStaging` + `stagingMutex` and gains one atomic slot per output object (the RT-owned `outScratch` is already the compose target):

```cpp
// Rebuilt at each remap, sized to image->outputs.size(); slot i pairs with image->outputs[i]
// (which carries bitOffset/bitLength). Each slot holds the object's latest encoded wire bytes
// packed into a u64 — type-agnostic, it's just bytes.
std::vector<std::atomic<uint64_t>> outputSlots;
```

- **Write** (`writePdo`, any thread): resolve the object to its slot index, `packLE(bytes)` → `outputSlots[i].store(packed, relaxed)`. Lock-free; no load, no RMW. Writers to *different* objects never contend; the *same* object is a clean last-writer-wins (a newer setpoint supersedes an older one — correct semantics).
- **Compose** (RT, once per cycle, replacing the `outputStaging.load` in `exchangeProcessData`): for each output object, `load` its slot, `unpackLE` to wire bytes, `insertBits` into `outScratch`, then exchange. Single thread writing `outScratch` → no lock, exactly like `inputSnapshot` in the other direction.
- **Output read-back** (`readPdo` for an output): load the slot — that's the current setpoint. **Inputs** are unchanged (`inputSnapshot` seqlock).

*Worked example (the CiA402 test image — controlword `0x6040` u16 @ bit 0, target position `0x607A` i32 @ bit 16):* an HTTP thread does `writeValue(0x6040, 0, 0x000F)` → `slot[0].store(0x000F)` while an RT task does `setTargetPosition(0x00405060)` → `slot[1].store(0x00405060)`. Different atomics, simultaneous, zero contention. Next cycle the RT composer emits `[0F 00 60 50 40 00]` — identical wire bytes to today's mutex version, no mutex anywhere.

*Why a single composer is required, not just nice:* two boolean outputs packed into one byte (`flagA` bit 0, `flagB` bit 1) written from two threads land in separate slots (no write race); the RT composer `insertBits` both into byte 0 → `0b11`. Letting the writers RMW the shared byte directly would clobber a bit. Routing all packing through one thread makes that class of bug *structurally impossible* rather than lock-prevented.

**Design A, considered and not chosen.** The alternative single-composer scheme: non-RT writers post `{bitOffset, bitLength, ≤8 bytes}` deltas into a lock-free MPSC ring the RT loop drains each cycle. It applies only deltas (less per-cycle work), but adds a genuinely new, easy-to-get-subtly-wrong concurrency primitive (a bounded MPSC ring) with an overflow policy to define. Design B needs no such primitive, matches the existing "the `DeviceParameter` cache is the source of truth" model (the slot is the RT-readable projection of the cached setpoint), and its per-cycle recompose is negligible — O(#output objects), ~tens of objects / a few hundred bytes, essentially the work `remapProcessImage` already does once when it seeds staging. Design A becomes the better choice only if that recompose ever turns measurable or PDO outputs routinely exceed 64 bits — neither true for SOMANET.

**Practical notes.** (1) `std::vector<std::atomic<uint64_t>>` can't be resized (atomics aren't movable), so it is *constructed* at the needed size on each remap — fine, since remap rebuilds and re-seeds everything anyway. (2) Slot lookup `(pos,index,subindex) → i`: either extend `ProcessImage::find` to return the output index, or `ProcessData` keeps a small `unordered_map<key,size_t>` built at remap. (3) The `uint64_t` slot caps an object at 64 bits — true for every SOMANET CiA402 output; a hypothetical wider PDO would need a per-object `SeqLock<array>` (and would reintroduce single-producer concerns for concurrent writers to *that* object), so document the constraint rather than build for a case that doesn't exist here.

**Status.** Implemented (2026-06-05), ahead of step 2. `ProcessData` now holds one `std::atomic<uint64_t>` slot per output object (rebuilt and seeded at each re-map) in place of the `outputStaging` seqlock + `stagingMutex`; `writePdo` is a lock-free per-slot store, and `exchangeProcessData` composes the wire image from the slots each cycle and publishes both an `outputSnapshot` (RT-written, for monitoring read-back) and the `inputSnapshot`. `ProcessImage::Location`/`find` gained `entryIndex` so an output object addresses its slot directly. One deliberate behaviour change: an output read-back on an *unhealthy* bus now returns the staged setpoint (always our own valid value, lock-free) rather than falling back to a blocking SDO upload — inputs are still health-gated and fall back to SDO. Tests cover independent-slot composition, sub-byte packing without clobber, and the output read-back. The `uint64_t`-slot ≤64-bit constraint stands as documented.

## Session 2026-06-05 — Device profiles as borrowed views (`ProfileDevice` ← `Cia402Drive` ← `SomanetDrive`)

**Supersedes** the device-hierarchy notes in *Session 2026-05-16 — Class diagram, device hierarchy, naming*. That section argued `Device → Cia402Drive` by inheritance with Somanet pushed out to free functions; the diagram alongside it argued the opposite (a `DeviceType` discriminator + a conditionally-owned `Cia402StateMachine`, i.e. composition). The two never agreed, and the code had silently picked a side: `DeviceManager` owns `std::vector<Device> devices_` **by value** (the whole reason `Device::parametersMutex_` is a `unique_ptr<std::mutex>` — see `device.h` — is to keep `Device` move-constructible into that vector). Value storage is flatly incompatible with `Cia402Drive : Device` — you'd slice, or you'd be forced to `vector<unique_ptr<Device>>` and downcast everywhere. So the prose's inheritance was unbuildable and the diagram's composition was the fallback.

**The reframe that resolves it: invert the ownership.** The profile object does not live *on* `Device` (neither as a base nor as an owned member). It *borrows* a `Device&` and is constructed on demand. Once profiles are **borrowed views, never stored**, the entire objection evaporates and a real is-a chain becomes correct:

```cpp
class ProfileDevice {                       // base: just the borrowed reference + generic OD helpers
 public:
  explicit ProfileDevice(Device& device) : device_(device) {}
 protected:
  Device& device_;                          // the ONLY data member in the whole chain
};

class Cia402Drive : public ProfileDevice {  // is-a: every CiA402 device shares this state machine
 public:
  std::expected<void, std::string> enable();                 // walks the CiA402 transitions
  std::expected<void, std::string> setOperationMode(OperationMode);
  Cia402State state() const;                                 // decodes statusword
};

class SomanetDrive : public Cia402Drive {   // is-a: a SOMANET drive genuinely implements CiA402 + extras
  // SOMANET-specific OD access (encoder config, motor config, ...)
};
```

This is *legitimate* inheritance because nothing is ever stored base-typed: views are stack-locals for a synchronous operation, or members of a task scoped to that task's lifetime. No `vector<ProfileDevice>`, so no slicing; no polymorphic storage, so no downcasting. `Device` stays value-stored and untouched. Profile is no longer "discovered at scan and baked into a type" — you construct the view you want at the moment you know what you want to do.

**Load-bearing rule: the views are data-free except `Device&`.** No persistent per-drive profile state. CiA402 doesn't need any — the drive's `statusword` *is* the state machine's state; an `enable()` just loops `read statusword → write controlword` until OperationEnabled (bounded, sub-second, synchronous, no memory between calls). The day a profile needs state that outlives a call is the day this model breaks; until then, give the views no members beyond the borrowed reference. (Procedure state that *does* persist lives in a task — below — which *is* stored.)

**Construction: checked factory free functions, not a `somanet::`/`cia402::` namespace, not a `DeviceManager` method.**

```cpp
// In mm::node, alongside reconcileDetectedModules. Validate the profile, then bind the view.
std::expected<Cia402Drive,  std::string> createCia402Drive(Device& device);
std::expected<SomanetDrive, std::string> createSomanetDrive(Device& device);
```

The `create…` prefix is honest — the view is returned **by value** (it's a `Device&` wrapper; trivially movable), nothing is allocated or owned heap-side. The factory validates at the boundary (`0x1000` device type / vendor id) so an HTTP handler can return a clean 400 when someone aims a drive-only operation at an I/O module. Rejected alternatives: `somanet::drive()` reads as a noun/accessor, not construction; `DeviceManager::somanetDevice()` would drag every profile/vendor onto the generic device-registry's surface, against the standing rule that `DeviceManager` references no concrete driver/profile types. The unchecked constructor `SomanetDrive{dev}` still exists for tests or already-validated paths; `createSomanetDrive` is the checked front door.

```cpp
auto* dev = dm.findDevice(pos);
if (!dev) {
  return badRequest("no device at position");
}
auto sd = createSomanetDrive(*dev);          // expected<SomanetDrive, string>
if (!sd) {
  return badRequest(sd.error());             // "device 3 is not a SOMANET drive"
}
// ... synchronous single-device OD work via sd-> ...
```

**Multi-cycle procedures: `ICyclicTask` takes `DeviceManager&` + its own task-specific targets, and re-resolves every cycle.** This corrects the old note's "ICyclicTask … take a `Cia402Drive&`". A task must **not** cache a `Device&`/view across cycles: `devices_` is a `vector<Device>` rebuilt on `scan()`/`reset()`, so a cached reference dangles the instant a rescan reallocates it — and a long-lived task is the most likely thing to still be alive across a rescan. So the universal task dependency is `DeviceManager&` (reference-stable access + the ability to re-resolve), and the task re-derives its view from a fresh `findDevice(pos)` each cycle:

```cpp
class CommutationOffsetTask : public ICyclicTask {
 public:
  CommutationOffsetTask(DeviceManager& dm, uint16_t axis) : dm_(dm), axis_(axis) {}
  void cycle() override {
    Device* dev = dm_.findDevice(axis_);     // re-resolve; nullptr ⇒ it left the bus → abort
    if (!dev) { /* abort the procedure */ return; }
    auto drive = createSomanetDrive(*dev);   // cheap view, reconstructed per cycle, never cached
    // ... step the procedure via PDO (controlword/statusword/target) ...
  }
 private:
  DeviceManager& dm_;
  uint16_t axis_;
};
```

**No generic targets parameter.** `DeviceManager&` is universal; *what* a task acts on is task-specific and must not be flattened into a one-size `vector<uint16_t> axes`. Offset detection is inherently single-axis → one `uint16_t`. A gantry/dual-axis sync task takes two positions *with roles* (leader/follower), not an anonymous list. An "enable all drives" task takes none and discovers its targets from `DeviceManager`. Each constructor takes exactly what its job defines.

**RT-safety of what a task touches.** Tasks run on the GameLoop (RT) thread, so per-cycle work goes through the lock-free PDO path — `readParameter`/`writeParameter` already route to the process-image seqlock/slots while the device is exchanging (see the Design B session). A task must **not** do blocking SDO inside `cycle()`: that takes the socket mutex and stalls the RT loop. SDO setup/teardown (precondition checks, writing the detected offset back to the OD) happens at **schedule time and completion time** on the control-plane thread.

**Where launching lives.** *(Partly superseded — see Session 2026-06-05 "RT tasks are fixed-membership" below.)* This section assumed the dynamic-scheduling model where a launch *constructs and schedules* a task: that genuinely needs both `DeviceManager&` *and* the scheduler (`GameLoop&`), which a thin view (holding only `Device&`) does not have, so the launch could not be a method on the view.

```cpp
// Handler/launcher — has DeviceManager& and GameLoop&:
auto* dev = dm.findDevice(pos);
if (!dev || !createSomanetDrive(*dev)) {
  return badRequest("position is not a SOMANET drive");
}
return gameLoop.schedule(std::make_unique<CommutationOffsetTask>(dm, pos));  // returns a handle; status via notification/poll
```

That reasoning holds **only for dynamically-scheduled tasks**, which the later session removes. Once RT tasks are *fixed-membership* (registered before `run()`), an RT generator's "launch" is no longer a scheduling op — it is a control-block write, a synchronous single-device state change, which is exactly the view's job. So for that class the rule inverts: the launch **does** live on the view (`Cia402Drive::startSineWave`). Off-RT command-and-wait procedures still launch as background jobs at the layer that owns them. See the next session for the resolved placement.

**`ProfileDevice` — keep or collapse.** If `Cia402Drive` is the only direct subclass for now, `ProfileDevice` is nearly empty (the `Device&` + a couple of OD-access shortcuts) and could be folded into `Cia402Drive`, reintroducing the base when a second profile (CiA401 I/O, a generic profile) actually appears — YAGNI. Kept as a named base only if the profile-agnostic glue is worth a stable seam today. Cheap either way; doesn't affect the rest of the model.

**The model in one rule.** *Views borrow a `Device&` for synchronous, single-device, here-and-now operations and carry no other state; tasks borrow `DeviceManager&` plus whatever target identifiers their specific job needs, re-resolve every cycle, and touch only the lock-free PDO path; both view types are constructed through checked `create…` factories in `mm::node`; launching a procedure lives at the layer that owns the scheduler, never on the view.*

---

## Session 2026-06-05 — RT tasks are fixed-membership; off-RT procedures are background jobs

**Revises** the launch model from *Session 2026-06-05 — Device profiles as borrowed views*: that session showed `gameLoop.schedule(std::make_unique<CommutationOffsetTask>(dm, pos))` — dynamic, per-request scheduling that adds a `CyclicTask` to a running loop and returns a handle. That requires machinery the RT loop does not have and should not grow: `GameLoop::addTask` is documented as before-`run()`-only and mutates a plain `std::vector<CyclicTask*>` with no synchronisation (`game_loop.{h,cc}`). Adding/removing tasks at runtime would need a lock-free add queue, a completion signal out of `execute()`, a retire queue, and off-RT destruction (never `delete` on the RT thread). All of that is avoidable.

**The deciding question is not "is the procedure ephemeral?" but "does it need to touch the process image on a per-cycle deadline?"** That splits every runtime procedure into two categories with two entirely different mechanisms.

**Category 1 — RT cyclic procedures (SineWave / profile / ramp generators).** Must write a fresh target into the output region *every* cycle, phase-locked to the bus. These are `CyclicTask`s — but with **fixed membership**: registered once before `run()`, exactly like `ProcessDataCyclicTask`. They are **activated/deactivated at runtime via a control block** (an atomic active-flag + seqlock'd parameters: target position/axis, frequency, amplitude, waveform), written from the HTTP thread. This mirrors the pattern already in production — `ProcessDataCyclicTask` is registered unconditionally and is a *no-op until a process image is published*. An idle generator is the same: registered always, computes nothing until HTTP flips it active. So runtime variability is in task *behaviour*, never task *membership* — which deletes the add/retire/destruction problem wholesale.

- **Concurrency = a fixed pool, not runtime spawning.** Several devices running waveforms at once is served by registering a small fixed pool of generator slots at startup, each idle until claimed by a device. Membership stays static.
- **Output writes are direct, not via the staging seqlock.** A generator runs *on* the RT thread, so it writes the output PDO slots directly each cycle. The output-staging seqlock exists only to serialise *non-RT* (HTTP) setpoint writers; an on-RT generator does not use it. (Note this is the same producer the Design B output path must stay lock-free for — see that session's `writePdo`/`stagingMutex` wart.)
- **Target ownership / arbitration.** While a generator owns a device's target word, a concurrent HTTP setpoint to that same word would fight it. Activating a generator must claim authority over that target; HTTP target writes for that device are rejected (or redirected) until it is deactivated.
- **Ordering.** Generators run *after* `ProcessDataCyclicTask` in registration order: read this cycle's inputs, compute, stage the output for the next exchange.

**Category 2 — off-RT procedures (commutation/offset detection, auto-tuning, firmware).** These drive the device through SDO writes / state transitions and then *wait* for the drive's firmware to do the work — they call a command and poll for completion, not cycle-time-sensitive. They have **no business on the RT thread at all** and are **not `CyclicTask`s**. The right shape is a background `std::jthread` calling reference-stable `DeviceManager`/`Device` methods (already serialised on the fieldbus socket mutex), polling and reporting progress, cancellable via `stop_token`. **Precedent: `FirmwareInstaller`** — already exactly this (long-running, `DeviceManager`-driven, off-RT); offset detection follows it rather than inventing scheduling.

This reclassifies commutation/offset detection from the RT, PDO-stepped `CommutationOffsetTask` sketched in the earlier session to a Category-2 off-RT job — on SOMANET the drive firmware performs the detection internally in response to an OD command, so the master writes the command and polls, which is off-RT by nature.

**The control block: a per-device params seqlock, written by the launcher, read lock-free by RT.** The transport is `mm::core::SeqLock<T>` — single-writer (serialise the non-RT producers with a mutex), any number of lock-free readers (the RT loop), `T` trivially copyable. A waveform param set is all scalars, so it fits:

```cpp
enum class WaveMode : uint8_t { CSP, CSV, CST };   // which target object the wave drives

struct SineWaveParams {                            // trivially copyable — no std::string/vector
  WaveMode mode;        // "controller" → CiA402 op mode (position/velocity/torque)
  double   amplitude;
  double   frequency;   // Hz
  double   velocityLimit;
  double   accelLimit;
  bool     active;      // the activation handshake rides inside the snapshot
};
```

A **seqlock, not per-field atomics**, precisely because the snapshot must be consistent — the RT loop must never read a new `amplitude` with an old `frequency`. One `load()` yields a coherent set. (Contrast the output PDO slots, where per-field atomics are right because each object is independent.)

**The control block lives on `Device`; the start/stop method lives on `Cia402Drive`.** This is the placement correction to the previous session's "launch lives at the scheduler layer" rule — see *Where launching lives* above. With fixed membership the generator task already exists in the loop, so a "launch" is just a control-block write — a synchronous single-device state change, which is the view's role. Concretely:

- `Device` holds the params seqlock as `unique_ptr<SeqLock<SineWaveParams>>` — `SeqLock` is non-movable and `Device` is moved into `vector<Device>`, so it takes the same `unique_ptr` indirection already used for `parametersMutex_`. One slot per device *is* the fixed pool — naturally sized and indexed by device, no abstract pool object.
- `Cia402Drive::startSineWave(amplitude, frequency, mode, …) → expected<void, string>`: (1) validates CiA402 preconditions — holding a `Cia402Drive` already proves it is a CiA402 drive; (2) does the op-mode handshake (write `0x6060`, confirm `0x6061`) synchronously on the caller's off-RT thread — the view's "synchronous single-device work"; (3) `device_.sineParams_->store({…, .active = true})`. `stopSineWave()` stores `active = false`. The HTTP handler shrinks to: resolve the view, call `startSineWave`, translate the `expected<>`.
- The single fixed-membership `SineWaveTask` (owned by App, holds `DeviceManager&`) iterates devices each cycle, `load()`s each control block, and for active ones computes `center + amplitude·sin(phase)` and writes the output slot directly (on-RT). It runs after `ProcessDataCyclicTask`. Bad params never reach it — all validation is on the launcher's `expected<>` path, so the RT side has no error branch.

**RT-only scratch state stays off the seqlock.** `phase_`, `center_` (latched on the rising edge of `active` so the wave starts from the current position), and `wasActive_` (edge detection) are written *and* read only by the RT task — they must not go in the control block (HTTP→RT only; readers must not see RT scribbling phase back). Phase is *accumulated* (`phase_ += 2π·f·dt`), not `2π·f·t`, so a mid-run frequency change stays continuous. Open question for implementation: this scratch lives either in a per-axis map inside `SineWaveTask` or as RT-only members on `Device`, and either way the task's per-cycle `findDevice()` re-resolution must sit behind the same `scan()`/`reset()` drain (`stopExchange()` + published-image pointer) that `ProcessDataCyclicTask` relies on today — iterating `devices_` while a rescan reallocates it is a data race that only the process-image path is currently guarded against.

**The model in one rule.** *If a procedure must hit the process image every cycle it is a fixed-membership `CyclicTask` registered before `run()`, gated active/idle by a per-device `SeqLock` control block that the corresponding view writes (`Cia402Drive::startSineWave`), with the fixed pool being one slot per `Device`; otherwise it is a cancellable background `std::jthread` calling `DeviceManager` (like `FirmwareInstaller`). `GameLoop` never gains runtime add/remove, and the RT side never sees an invalid param set.*

---

## Session 2026-06-06 — TLS cert auto-update (self-heal + rolling release)

**Extends** *Session 2026-05-23 — TLS certificate automation*, and corrects one bug in it.

**The acme-dns credential bug.** That session said `cert-renewal.yml` "writes `~/.acmedns.json` from the `ACMEDNS_CONFIG` secret." It does, but acme.sh's `dns_acmedns` plugin **does not read that file** — it reads `ACMEDNS_USERNAME`/`ACMEDNS_PASSWORD`/`ACMEDNS_SUBDOMAIN`/`ACMEDNS_BASE_URL` from the environment (the `~/.acmedns.json` format is certbot's). With no env vars set, acme.sh self-registered a throwaway acme-dns account each run and posted the challenge TXT to a subdomain the permanent CNAME doesn't point at, so validation spun ~20 min and failed (the 2026-06-01 cron and a manual re-run both did). Fixed: parse `ACMEDNS_CONFIG` with `jq` and export the `ACMEDNS_*` vars, with a guard that aborts before any validation attempt unless the parsed subdomain equals the public CNAME target (`4723b93a-…`) — so a bad config can't burn a Let's Encrypt failed-validation slot.

**Rolling release as a stable fetch source.** Releases bundle the cert, but they're tied to `v*` tags — a 4-month release gap means the bundled cert is already expired, useless as a refresh source. So `cert-renewal.yml` also publishes the monthly cert/key as assets on a fixed-tag `tls-cert` release (marked **pre-release** so it never becomes the repo's "Latest" and shadows app releases — `--latest=false` was not enough once it was the only non-prerelease). Stable URL, decoupled from app cadence: `https://github.com/synapticon/motion-master/releases/download/tls-cert/{cert,key}.pem`. Publishing the keypair is safe: it only authenticates `local.motion-master.synapticon.com`, which resolves to `127.0.0.1`.

**Self-heal in the binary, not (only) the API.** The load-bearing refresh path is in the binary, because an expired cert blocks the very API call that would fix it: the PWA reaches the local server via cross-origin `fetch()`, which a browser refuses (with no click-through) once the cert is invalid — and terminal-only users never open the UI. So:

- `cert_updater.{h,cc}` (`fetchAndSwapCert`, libcurl + the already-linked OpenSSL) downloads cert+key, validates the pair (parses, CN matches, not expired, key matches cert), then atomically installs them (temp + rename; key `0600`).
- `main.cc` self-heals at startup: a missing/expired cert triggers a fetch before binding TLS (missing + fail is fatal; expired + fail serves the expired cert). `--no-cert-update` opts out (air-gapped); `--update-cert` fetches and exits (headless/CLI); `--cert-url`/`--key-url` override the source.
- `GET /api/cert` reports validity (`expiresSoon` within 7 days); `POST /api/cert/refresh` is the still-valid proactive path, surfaced as a button on the PWA Connection page. It returns `restartRequired: true` — the TLS listener loads the cert once at listen, so a restart applies it (chosen over hairy in-process listener reload). It runs synchronously on the HTTP loop; tolerable because it's rare and ~1 s, and after the split below it no longer touches the WebSocket.

---

## Session 2026-06-06 — HTTP and WebSocket on separate ports/loops

**Supersedes** the single-port model in *Session 2026-05-16 — HTTPS, WebSocket security, and PWA connectivity* (the "binds exclusively to `127.0.0.1:8443`" / "`wss://…:8443`" / "Port 8443 is fixed" claims).

**Problem.** HTTP and the monitoring WebSocket shared one uWS app, one event loop, one thread. uWS runs handlers inline on the loop thread, so a blocking handler (FoE transfer, an SDO, the cert fetch) froze the loop — and since `publish()` marshals monitoring frames onto that same loop via `defer()`, the live stream stalled and inbound WS messages went unread along with it.

**Why not just give the WebSocket its own loop on the same port.** Can't. A WebSocket is not a separate socket — the `101 Switching Protocols` upgrade reuses the *same* TCP connection (same fd), and in uWS an fd belongs to the loop that accepted it (the loop isn't thread-safe to mutate from elsewhere; there's no socket-to-loop migration). One port ⇒ one accepting loop ⇒ the WS is stuck on it. The only one-port fix is to never block the loop (offload slow handler work to a worker, respond via `defer`) — the worker-offload option. A *physically* separate WS loop needs its own port.

**Decision: second port.** Split the merged `Server` into `HttpServer` (61447) and `WebSocketServer` (62281, `--ws-port`), each its own uWS SSLApp + loop + thread — realising the `HttpServer`/`WebSocketServer` split already in the class diagram. (Defaults are 61447/62281, not 8443/8444: chosen high in the IANA dynamic range and deliberately **above** Linux's default ephemeral range 32768–60999, so a long-running listener never collides with an OS-assigned outbound port, and with no known association to common services.) The original "no dual-port setup" mandate was a reaction to the old `motion_master`'s ZeroMQ request + pub/sub channels; a second TLS port for the *same* WebSocket is far milder, and the hard isolation is worth it. Trade-off accepted: this isolates the WebSocket (the latency-critical 1 ms path) but does **not** stop HTTP handlers from head-of-line-blocking *each other* (e.g. a long FoE delaying `GET /api/version`, which the PWA health-poll reads as "offline"); that's the separate worker-offload fix, deferred until it bites.

**The WebSocket is the bidirectional realtime channel**, not monitoring-only: server→client monitoring batches, notifications (slaves changed, watchdog), and procedure progress (firmware, calibration); client→server topic subscribe/unsubscribe and (planned) process-data **output** values staged for the RT loop's output seqlock. Today only subscribe/unsubscribe and monitoring publishes are wired; the rest plug into the same `WebSocketServer` as they land. The inbound output-staging path will need `WebSocketServer` to reach `DeviceManager` (write the staging seqlock) with its own validation design — not built yet.

## Session 2026-06-07 — Two BOOT-excursion re-map crashes

Cycling a device INIT → BOOT → INIT → PRE-OP → SAFE-OP (and the deliberate illegal BOOT → SAFE-OP) on real hardware crashed Motion Master two ways, both rooted in `configureProcessData()` re-mapping the bus with `ecx_config_map_group` *without* a fresh `ecx_config_init`. Both fixed and hardware-confirmed.

**Crash 1 — duplicate Outputs FMMU + out-of-bounds write.** SOEM's FMMU mappers (`ecx_config_create_{output,input}_mappings` + the mailbox-status mapper) start at `slavelist[i].FMMUunused` and *append*, trusting that `ecx_config_init`'s memset zeroed `FMMUunused` and the `FMMU[]` array. A re-map never memsets, so `FMMUunused` stayed at 3 from the prior map: the output mapper wrote a byte-identical duplicate Outputs FMMU at `FMMU[3]`, then the input/mailbox mappers ran at index 4. With `EC_MAXFMMU == 4` that wrote a 16-byte `ec_fmmut` past the end of the array, smashing the adjacent `ec_slavet` fields (`FMMU*func`, `mbx_*`, … and on repeated re-maps marching toward `PO2SOconfig`). This is the long-unconfirmed root corruptor behind the earlier `PO2SOconfig` segfault. **Fix:** in `configureProcessData()`'s pre-map reset loop, `memset` each slave's `FMMU[]` + `FMMUunused = 0` so SOEM re-derives FMMU0/1/2 from scratch like a clean scan, and FPWR-clear all `EC_MAXFMMU` ESC FMMU registers so a slave already carrying a stale duplicate self-heals on the next map. (Sits alongside the existing `outputs`/`inputs`/`PO2SOconfig` resets, all there for the same "a re-map doesn't memset the slavelist" reason.)

**Crash 2 — illegal AL transition reads SDO over a dead CoE mailbox.** Entering SAFE-OP/OP re-maps, and the re-map reads each slave's PDO mapping over the **CoE mailbox** (`ecx_readPDOmap → ecx_SDOread → ecx_mbxreceive → memcpy`), which is only live from PRE-OP up. A device commanded straight from BOOT (firmware-sized mailbox SMs, no CoE) reached that mailbox read while still in BOOT and segfaulted in `ecx_mbxreceive` — *before* the slave could reject the illegal jump with AL status 0x0011. **Fix:** enforce the EtherCAT AL state machine in `DeviceManager::transitionToState` before any re-map, via a `kValidStateTransitions` map (current state → allowed targets: single-step climbs `INIT → PRE-OP → SAFE-OP → OP`, multi-step drops, BOOT only paired with INIT; self-transitions allowed). Validated against a fresh `readStates` (authoritative; an AL-status register read works in any state). Lives in the node layer on purpose — re-mapping is a Motion Master concern, not the fieldbus's, so direct `FieldbusDriver` use is the caller's own risk. Added `toString(EtherCatState)` next to `alState`/`alHasError` for the messages. The PWA's Control page already taught these rules in its inline AL-state hints; the backend now actually enforces them rather than trusting the slave (which it never reaches in the crashing case).

---

## Session 2026-06-08 — Diagram & threading reconciled with code

The "Class Diagram UML" stub and the *Session 2026-05-16* detailed diagram had drifted from what was actually built. Walked the source and brought the canonical references in `CLAUDE.md` ("Class Structure" + "Game Loop / RT Threading") back in line; this note records the as-built shape and what is still only designed. The historical session entries are left intact as the reasoning trail.

**Built and load-bearing today:**

- **Composition root is `main.cc`**, not an `App` class — `main.cc` is the one place concrete types are instantiated and wired (the DI seam `Server::Config::InitDriverFn` is the exception that lets `POST /api/init` build a driver without the server knowing concrete types).
- **`DeviceManager` owns `unique_ptr<FieldbusDriver>` + `std::vector<Device>` + `unique_ptr<ProcessData>`.** It hands each `Device` a `FieldbusDriver&` and a raw `ProcessData*` (non-owning; the manager outlives every device). The only concrete driver in the tree is `SoemFieldbusDriver`; `SpoeFieldbusDriver`/IgH remain planned.
- **Profiles are borrowed views, confirmed in code:** `ProfileDevice ← Cia402Drive ← SomanetDrive`, each holding only a `Device&`, built via `createCia402Drive` / `createSomanetDrive`. There is **no** `Cia402StateMachine` owned by `Device` — that 2026-05-16 ownership model was already inverted by the 2026-06-05 borrowed-views session; the older diagram just hadn't caught up.
- **One RT task is registered:** `ProcessDataCyclicTask` → `DeviceManager::exchangeProcessData()`. `GameLoop` keeps fixed `CyclicTask` membership.
- **Output path is lock-free per-object slots, not a shared seqlock.** `ProcessData` carries `inputSnapshot`/`outputSnapshot` (`SeqLock<ProcessBuffer>`, RT writes), plus `outputSlots` — one `std::atomic<uint64_t>` per output object that any non-RT writer stores into independently; the RT loop alone composes them into the wire image (Design B). This replaced the earlier single shared output-staging seqlock + mutex that the threading doc still described.
- **Monitoring is off-RT.** `MonitoringManager` (constructed in `main.cc` over `DeviceManager`, reached by `HttpServer` for `/api/monitorings`) owns a sampler thread and a `ParameterRefresher` thread; it publishes batches via a `setPublish` callback wired to `WebSocketServer::publish`. It is deliberately **not** a `CyclicTask`.

**Designed but not yet in code (kept in the diagram, flagged `planned`):**

- `SineWaveTask` and the per-device `SeqLock<SineWaveParams>` control block (the RT cyclic-procedure pattern from 2026-06-05).
- `NotificationBus` (the observer that would decouple producers from the servers) — notifications currently go straight through `WebSocketServer::broadcast`.
- `FirmwareInstaller` — the off-RT `std::jthread` procedure shape now has a real precedent in `MonitoringManager`'s threads rather than `FirmwareInstaller` itself.
- The original `Watchdog` *class* never materialised; "watchdog" in the code is the ESC sync-manager hardware watchdog (config + diagnostics on `DeviceManager`/`FieldbusDriver`), not an RT task.

## Session 2026-06-08 — Lossless process-data capture: a big circular recorder (design)

**The gap.** The monitoring path is structurally *lossy* and always will be, by two independent mechanisms, and that is correct for what it does — live display:

1. `ProcessData`'s `inputSnapshot`/`outputSnapshot` are `SeqLock<ProcessBuffer>` — a **single slot, last-writer-wins**. The RT loop overwrites it every cycle; a reader gets *whatever is there now*. The seqlock guarantees a coherent (non-torn) read, not *every* read.
2. `MonitoringManager` samples off-RT on its own `interval` (`Entry::nextDue`). Even at `interval = 1 ms`, an off-RT thread cannot observe every distinct 1 ms RT cycle — scheduling jitter alone skips cycles. It reads the *current* snapshot, not a queue.

For "the user must get **every** cycle, no skips" (trajectory recording, offline analysis, post-mortem after a fault) neither layer can deliver. This is the same lossless-capture need that the *previous, unrecorded* session raised — writing it down this time.

**Not a drain queue — a continuous circular recorder, and the source for the live stream too.** The decided shape (not an SPSC producer→consumer where the consumer advances a tail and frees slots) is a flight-data-recorder ring in master RAM. Crucially, **monitoring must deliver *every* cycle for plotting** — the live WebSocket path has to be lossless, not just the bulk pull — so the ring is the source for *both*:

- **RT loop writes one record every cycle, forever**, advancing a head and overwriting the oldest only on wrap. "Very big" ⇒ a long rolling history is always resident.
- **One ring, two non-destructive readers, both reading every cycle.** (a) The **live streamer** (`MonitoringManager`'s thread) holds a per-monitoring **read cursor** — a read index, *not* a tail. Each flush it reads `ring[cursor..head]`, decodes that monitoring's parameters for **every record in the span**, packs them as rows, publishes the batch, advances *its own* cursor, frees nothing. `interval` stops meaning "sample one value per interval" and becomes the **flush cadence** (flush every 50 ms ⇒ ~50 cycle-rows/batch at 1 ms); the client plot gets a contiguous, gap-free series. (b) The **dump** is a second non-destructive reader — **deferred, see the parked section at the end of this note**. The existing WebSocket protocol already carries "one inner array per sample" — a batch now holds every cycle-row since the last flush instead of interval-samples, **no protocol change**.
- **The `SeqLock<ProcessBuffer>` snapshots are dropped entirely (settled 2026-06-09).** Earlier this note kept them "for point reads"; that's redundant — **`ring[head-1]` *is* the latest coherent snapshot.** A point read (`readPdo`, current-value lookups) loads `head`, targets `seq = head-1`, reads `ring[seq % capacity]`, and re-checks the slot's sequence (the same per-record seqlock guard the live cursor/dump use) — identical coherence to the old seqlock. Wins: **the RT loop writes only `ring[head]` once per cycle** (not ring + input snapshot + output snapshot); one source of truth; and a single record holds inputs *and* outputs from the *same* cycle, so a combined read can't straddle cycles the way two separately-written seqlocks could. Health gating is unchanged (WKC atomic + published-image pointer, independent of the seqlock; `head == 0` ⇒ no cycle yet ⇒ same SDO fallback). Output *staging* is unaffected (`outputSlots` atomics, Design B); the output read-back just comes from `head-1` too. `ProcessData` stops holding `SeqLock<ProcessBuffer>` members — the generic `SeqLock` in `libs/core` stays (a ring slot is morally one). The ring is the single RT-written structure and the source for the full history, the live plot, *and* point reads.

**Two decisions taken (2026-06-08):**

- **Sizing: configurable *seconds*, in the JSONC config — not a CLI flag** (settled 2026-06-08). Ring depth is a persistent, per-machine RAM budget set once per install, which is what the config file is for — and the config (added but unused so far) gets its first real consumer, setting the pattern. A `recorder` block, **`historySeconds` only for now**: `{ "historySeconds": 300 }`. (`dumpDir` is **not** added yet — it belongs to the deferred dump, and a `/tmp` default is wrong cross-platform: Windows has no `/tmp`. When the dump returns, derive the default from `std::filesystem::temp_directory_path()`, not a hardcoded path.) `capacity = historySeconds × 1000 cycles/s`; the byte allocation can't be computed until the process image exists (record size = whole-image size, bus-dependent), so the **ring is allocated and `mlock`'d at `configureProcessData` time, not at startup** — its lifecycle is tied to the process image. A per-cycle record is 28 B fixed (20-byte header + 8-byte publication word) plus the whole-bus IOmap (~82 B per SOMANET drive, budget ~100 B ⇒ ~128 B/cycle for a single drive). At ~128 B/cycle: 1 min ≈ 7.7 MB, 10 min ≈ 77 MB, 1 hr ≈ 460 MB. Default leaning ~300 s ≈ 38 MB per drive; it scales with drive count.
- **Scope: one global recorder of the *whole raw process image* per cycle**, not per-monitoring projections. The RT push is one `memcpy` of the input (and output) IOmap; **decoding to specific parameters happens at read time**. Consequence — the full history is available for parameters never declared in a monitoring, which is what you want after an unexpected fault. (Per-monitoring rings were rejected: N RT writes, and you only capture what you declared up front.)

**The ring is gapless; only a slow *reader* can "gap".** The RT producer writes *every* cycle, contiguously — the recorded data never has holes. A "gap" can only mean a *reader* fell so far behind that the producer overwrote its data before it was read. That is a property of a slow consumer, not of the recording, and it can only happen to the **live cursor** (a pathologically stalled client, > the whole ring depth), never to a bulk read of a frozen span. Mechanism — **per-record sequence numbers**, not a global seqlock over the whole ring:

- Each slot carries a `seq` (the absolute cycle count, monotonic, never reused; `slot = seq % capacity`), written *after* the payload with **release** ordering. `head` is the producer's atomic next-sequence counter. RT write stays wait-free: one `memcpy` + one atomic release store — no lock, never blocks on a reader.
- The **oldest still-valid record** is `max(nextReadSeq, head − capacity)` — valid sequences are always the window `[head − capacity, head)`.
- A live reader keeps `nextReadSeq`. Each wake: load `head` (acquire). If `nextReadSeq < head − capacity` it was lapped → emit a **gap notification** over the WebSocket (`missed [nextReadSeq, head − capacity)`) and jump `nextReadSeq = head − capacity`. Otherwise read `[nextReadSeq, head)`, re-checking each slot's `seq` after the copy (seqlock-style) to catch a mid-copy overwrite; then `nextReadSeq = head`.
- So the gap is a **stream-side notification, never a stored artifact**. Lossless-or-explicitly-flagged on the live path; the user always *knows* if a read raced a wrap (vanishingly rare with a seconds-deep ring, but never silent).

**Monitoring config: interval only, no `bufferSize` (settled 2026-06-08).** Configure a monitoring by **interval** (refresh/flush cadence), not buffer size. Rationale: users think in refresh rate not sample counts; interval is invariant to cycle rate (`bufferSize` of N means N ms at 1 kHz but 2N ms at 500 Hz); and `bufferSize` is now redundant since the producer writes every cycle regardless — the only thing left to choose is how often the reader wakes. **Drop the `bufferSize` field from `Monitoring`** and its `>= 16` validation in `create`.

- **Interval bounds: [10 ms, 1000 ms], default 20 ms (50 Hz).** Throughput is constant regardless of interval (lossless — all data flows either way); interval only trades message size vs frequency (`rows_per_msg ≈ interval / cycle`, ~450 B/row). Lower bound 10 ms (≈4.5 KB/msg, 100/s) avoids a message storm and sub-cycle pointlessness; upper bound 1000 ms (≈450 KB/msg, 1/s) supports a slow-trend view and is the accepted big-bursty-message case.
- **The reader loop (settled):** each wake, **snapshot `head` once** into a local, ship decoded rows for the half-open span `[cursor, head)` as **one WebSocket message**, then `cursor = head`. `head` is the *next* write slot, so `head-1` is the newest completed record. Snapshotting `head` once is what bounds the message (don't re-read live `head` mid-copy). A jitter-delayed wake just produces one occasionally-bigger message — still drains to `head`, so it **never falls behind and never loses data**. No drain-loop / per-message row cap needed because the bounded interval already bounds `head - cursor`.
- **Two delivery modes — lossless stream vs. last-N telemetry (added 2026-06-09, design only; lives ONLY in this ring redesign, not the current interval-sampler).** A monitoring picks one:
  - *Lossless stream* (default, the loop above): deliver **every** cycle-row in `[cursor, head)`. For plotting. Interval is the flush cadence and stays bounded **[10 ms, 1000 ms]** because the message size scales with `interval / cycle`.
  - *Last-N telemetry* (new): deliver only the **most recent N** samples per flush (e.g. an optional `lastN` field; `N=1` ⇒ just the latest value `[[ts, val]]`), then `cursor = head` — the in-between cycles are **dropped**. Lossy by design; for slow, low-volume telemetry (e.g. *temperature every 5 s*). The use case that motivated it.
  - **This mode relaxes the interval upper bound.** A last-N message is bounded by **N**, not by `interval × cycleRate`, so the interval may be much larger here (seconds → ~minutes) without a big message — the `1000 ms` cap only needs to hold for the lossless mode. (Decide the last-N interval ceiling when implementing — e.g. 60 s.) No protocol change: it is still an array of `[ts, …]` rows, just at most N of them.

### ✅ DEFERRED → SHIPPED (2026-06-10) — file dump of the recorder

**Implemented in the 2026-06-10 session below** (`POST /api/process-data/dump`, `process_data_dump.{h,cc}`, the `.mmpd` format). The design notes below are the spec it was built from; two things changed on the way in — see that session for the deltas: (1) the **trigger gate was dropped** — the dump runs tail→head in any state (OP included), not only when a device leaves the exchange states; (2) `dumpDir` became a real `recorder` config key. The "hard open problem" (ring lifecycle vs. teardown & re-map) needed no new work — the live recorder already retains the ring across teardown and resets it on re-map.

The design worked out at parking time, kept for reference:

- **What it is:** a *global* dump holding **exactly what the recorder keeps** — full raw inputs + outputs for every cycle in the ring span — **plus the process image embedded as a header** so the file decodes offline with no running Motion Master and no live bus (analyse a run after the device powered off). Supersedes the earlier "decode one monitoring's params to CSV" idea: monitorings are the *live* path, the dump is the recorder's raw archive — two distinct things.
- **Trigger:** there is **no explicit pause**. The dump-worthy moment is *implicit* — when a device leaves the exchange states (OP/SAFE-OP → PRE-OP/INIT/BOOT), at which point its PDO production has stopped, which is exactly what makes a dump trivially safe (producer not advancing, no lapping possible).
- **Endpoint:** `POST /api/process-data/dump` → serializes the valid span to a file, returns `{ "path": "<dumpDir>/dump-<timestamp>.mmpd" }`. Not scoped to a monitoring topic. MM binds `127.0.0.1`, so the file is on the user's own machine — a path suffices; **no download/list/delete endpoints, no retention machinery** (the OS owns the dir). `dumpDir` becomes a `recorder` config key *when this is built* (not added yet). **Its default must be cross-platform** — derive from `std::filesystem::temp_directory_path()` (→ `/tmp/...` on Linux, `%TEMP%\...` on Windows), never a hardcoded `/tmp`.
- **Format: self-describing binary** (CSV can't carry "whole raw image + the schema to decode it" without being huge and lossy about types/layout):
  `[magic + format-version]` · `[header: cycle period; start sequence; row count; input/output region sizes; process image = per device {slavePosition, name, vendorId, productCode, PDO entries[{index, subindex, name, dataType, bitOffset, bitLength, direction}]}]` · `[rows: per cycle {sequence, timestamp, raw input bytes, raw output bytes}]`. **No gap markers** — a bulk read of a frozen/contiguous span finishes ~300× faster than the ring wraps and the producer has stopped anyway, so the dump cannot be lapped; gaps are a *live-stream* concept only. The header **is** the `ProcessImage` from `configureProcessData()`/`buildProcessImage`; a `format-version` lets it evolve. A planned **UI tool** loads one file → per-device / per-parameter / per-window views.
- **Synchronous write** off-RT on the HTTP thread (300 s ring ≈ ~38 MB per drive, sub-second). Background job + progress only if rings ever get huge (1 h ≈ 460 MB per drive, more with many drives).
- **The hard open problem to solve on return — ring lifecycle vs. teardown & re-map:** the ring + its process-image header **must survive image teardown**, because tearing down the published image happens when the *last* device leaves OP/SAFE-OP — precisely the dump moment. So the ring is owned durably by `ProcessData` (which `DeviceManager` owns), retained across image republish *and* teardown; only `reset()`/`scan()` frees it. And because a **re-map changes the byte layout**, old records become undecodable under a new header — so a layout-changing re-map must **reset the recording** (clear the valid span, capture the new header); one span can't straddle two layouts. Lifecycle: *allocated & header captured at map → fills every cycle → frozen-but-retained when the device drops → reset & re-headered on the next re-map.*

**IMPLEMENTED (2026-06-09) — the live recorder.** `ProcessDataRing` (`libs/node/process_data_ring.{h,cc}`) is a lock-free circular recorder held as a member of `ProcessData`. The RT loop appends one record per cycle in `exchangeProcessData` (raw input + output IOmap, wall-clock epoch-ns timestamp, working counter) via a wait-free `write()` (a few `memcpy` + a per-slot release-stored absolute sequence number; readers re-check the sequence after copying to detect a write that raced the copy). It is allocated and best-effort `mlock`'d at `configureProcessData` for `recorder.historySeconds × (1e6 / gameLoop.periodUs)` cycles (a per-cycle record is 28 B fixed + the whole-bus IOmap, ~82 B per SOMANET drive → ~128 B/cycle for a single drive, so the default 300 s ≈ 38 MB per drive), re-allocated on a layout-changing re-map (the recording restarts — old records are undecodable under a new layout), retained across image teardown, and freed only by `reset()`/`scan()`. The pair of whole-image snapshots that used to carry inputs/outputs across the RT boundary is gone — `readPdo` and point reads now read the newest record (`head()-1`); `DeviceManager` exposes `recorderHead()`/`recorderOldestSeq()`/`readRecord()` as the off-RT read surface. `MonitoringManager` is rewired to the cursor model: each monitoring holds a read cursor and, on each flush, ships every recorded cycle in `[cursor, head)` as one batch (lossless), advancing the cursor; `bufferSize` was dropped and `interval` is now the flush cadence; a cursor lapped by more than a whole ring is logged (not notified) and resynced. Wire row timestamps are **epoch microseconds** (JS-exact to ~2255, distinct per cycle at sub-ms periods). Config gained a `recorder` block (`historySeconds`, default 300); `DeviceManager::init` takes the history depth and cycle period. The follow-ups are also done: `swagger.yml`, the regenerated `hil/api/src/mm-api.ts`, and the UI (`MonitoringsPage.tsx`) all reflect the new contract; `CLAUDE.md`/`README.md` updated.

*Portability gotcha (fixed 2026-06-09, commit 243d849):* the ring's seq array is a `std::vector<std::atomic<uint64_t>>`, and `std::atomic` is non-movable/non-copyable, so it must never **reallocate** — `clear()` releases it by swapping in an empty vector, not `shrink_to_fit()`. `shrink_to_fit`/per-element reallocation compiles on Linux libstdc++ but is **rejected by libc++ (macOS) and MSVC (Windows)**, so it passed local Linux builds and only broke the macOS/Windows CI legs. Whole-vector move-assignment (sizing it once) is fine; per-element reallocation is not.

**Interval bounds widened (2026-06-09):** the flush cadence is bounded **5–2000 ms** (was 10–1000 ms in the original design above), default **16 ms** (≈ one batch per 60 Hz display frame — the rate the plot actually repaints). Rationale: interval governs message size, not resolution (lossless either way); 5 ms allows lower-latency plots without a message storm, 2000 ms keeps a heavy 40-param @ 1 ms monitoring under ~760 KB / ~11 ms to serialize+parse. Note the bound is cycle-period-dependent — at a sub-ms cycle every message scales up proportionally — so a byte-budget cap (or the deferred last-N mode) is the more robust long-term guard for the slow/fast extremes.

**Open / next:** the deferred dump **shipped 2026-06-10** (see that session); the two-delivery-mode *last-N telemetry* path is still a fast-follow (only the lossless mode shipped), as is the offline `.mmpd` viewer UI.

## Session 2026-06-08 — Config file: optional, code-owned defaults (the `recorder` block is its first consumer)

The JSONC config (`nlohmann::json::parse(stream, nullptr, true, true)`, `.jsonc`) has existed but had no consumer; the recorder's `historySeconds` is the first. Settled the config posture generally, since this sets the pattern:

- **Optional, with defaults in code.** A missing config file is **not an error** — it means "all defaults." Motion Master runs zero-config (terminal-only users, Docker, first-run all just work, like the server already binds default ports without a file). Code is the single source of truth for defaults.
- **Ship a fully-commented `motion-master.example.jsonc` in the package — *not loaded*, pure documentation.** Every key present at its default with a `//` comment. This is the one real benefit of "distribute a config with every instance" — *discoverability* of what's tunable — without making the file load-bearing or letting defaults drift into being required. JSONC comments are the reason this works. Because it is not loaded it needn't be a conffile (fine to overwrite on upgrade); only an *active* default config installed to `/etc` would need `%config(noreplace)` like `cert.pem` — deliberately avoided for now (active config stays purely user-created).
- **Partial override, not all-or-nothing.** A real config need only contain the keys it changes — set `recorder.historySeconds` alone and everything else stays default. The loader **merges the parsed file over a code-defaults object** (nlohmann: start from defaults, `merge_patch` the file on top), never requires a complete document.
- **Explicit `--config` only — no default search path.** ~~Config is loaded *only* when `--config <path>` is passed; there is no implicit `/etc/...` or next-to-binary lookup. Absent `--config`, all defaults apply.~~ **Superseded 2026-07-17** (see that session below): a `motion-master.jsonc` **next to the executable** is now auto-discovered when no `--config` is given, so the Windows release can bundle a working config. The `/etc` (system-wide) lookup stays rejected — the only implicit path is next-to-binary, which is scoped to the install directory, not a machine location the user didn't name.
- **No `/tmp` defaults in config.** Followed through when the dump shipped (2026-06-10): `recorder.dumpDir` defaults to `""`, resolved at dump time to `std::filesystem::temp_directory_path() / "motion-master"`, never a hardcoded `/tmp` (Windows has none).

**Settings are config-file-ONLY — they have no CLI flags (settled & implemented 2026-06-09; reverses the earlier "CLI overrides config" plan).** A tunable setting is set *only* through the JSONC file; the corresponding `--port`/`--ws-port`/`--cert`/`--key`/`--driver`/`--adapter`/`--log-level`/`--cors-origin`/`--no-cert-update` flags were **removed**. The CLI surface is now just actions + cert-fetch sources: `--help`, `--version`, `--list-adapters`, `--config`, `--open`, `--update-cert`, `--cert-url`, `--key-url`. This dropped the whole CLI-override layer (no precedence merge, no two-phase pre-pass) — `parseOptions` parses the CLI actions, then loads the file into `opts.config` and resolves the adapter. The settings schema (mapped 1-1 to the `Config` struct tree, `config.h`):

  ```jsonc
  {
    "server":   { "httpPort": 61447, "wsPort": 62281, "corsOrigin": "https://motion-master.synapticon.com" },
    "fieldbus": { "driver": "soem", "adapter": "eth0" },   // spoe → "ipAddresses": [...] instead of adapter
    "logLevel": "info",
    "tls":      { "certPath": "cert.pem", "keyPath": "key.pem", "autoUpdate": true },
    "recorder": { "historySeconds": 300, "dumpDir": "" }   // dumpDir "" ⇒ <temp>/motion-master (added 2026-06-10)
  }

  ```

  Notes: keys are **`httpPort`/`wsPort`** (symmetry) and **`certPath`/`keyPath`** (explicit they are paths). **`fieldbus` empty/omitted ⇒ no startup auto-init** — the fieldbus waits for `POST /api/init`; set `driver`+`adapter` to auto-init at startup. The `fieldbus` block is partly **driver-specific** (`soem` → `adapter`; `spoe` → `ipAddresses`). **`tls.autoUpdate` defaults `true`** (air-gapped sets `false`). **`certUrl`/`keyUrl` stay CLI-only** (`--cert-url`/`--key-url`), excluded from the config/example. Implementation: `Config` uses nlohmann `NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT` so absent keys fall back to defaults; `parseConfig` adds the enum checks nlohmann can't (`logLevel`, `fieldbus.driver`). Callers that used the removed flags now **generate a config file**: `tools/run.sh` (cert/cors/logLevel via `CORS_ORIGIN`/`LOG_LEVEL` env), `tools/run-dev.sh`, and `docker-entrypoint.sh` (or mount one and set `MM_CONFIG`).

## Session 2026-06-10 — Recorder dump shipped (`POST /api/process-data/dump`, the `.mmpd` file)

**IMPLEMENTED — the deferred recorder dump (the parked section under "Lossless process-data capture" above).** The hard open problem that parked it — ring lifecycle vs. teardown & re-map — was already solved by the live recorder: the ring is owned by `ProcessData`, retained across image republish *and* teardown, re-allocated (recording restarts) only on a layout-changing re-map, and freed only by `reset()`/`scan()`. So the dump just freezes the current span and serializes it; `generations.back()` is always the layout the records in the ring were written under.

- **`libs/node/process_data_dump.{h,cc}`** — the binary format, kept as a pure, testable seam (no `DeviceManager`/filesystem coupling). `DumpHeader`/`DumpDevice`/`DumpPdoEntry` plus `writeProcessDataDump(out, header, startSeq, endSeq, read)`. Magic `MMPD` + `kDumpFormatVersion` (1), little-endian throughout. Layout: a fixed 40-byte prefix (`magic, formatVersion, flags, cyclePeriodUs, startSequence, rowCount, inputBytes, outputBytes, deviceCount`) — `rowCount` sits at the **fixed offset `kDumpRowCountOffset` = 20** so the writer streams the rows first, then `seekp`s back to patch the real count; then the variable device table (per device: ids + name + PDO entries `{index, subindex, direction, dataType, bitLength, bitOffset, name}`); then fixed-stride rows `{u64 sequence, u64 timestampNs, inputs[inputBytes], outputs[outputBytes]}`. Each row's regions are padded/truncated to the header sizes for a fixed stride. Full epoch-**ns** precision is kept in the file (the µs reduction is a live-monitoring/JS concern only).
- **`DeviceManager::dumpProcessData()`** — takes `busMutex_` shared (serialise against the device-set rebuild like the other off-thread read surfaces; the RT producer is never blocked), picks the header image (`image` live, else `generations.back()`), builds the header from `devices_` + the image entries (names/data types from each device's parameter map, empty/0 when the OD wasn't enumerated), then writes `dumpDir/dump-<UTC>-<endSeq>.mmpd` via the pure writer with a lambda over `pd_->ring.readRecord`. Returns the path.
- **Trigger relaxed from the parked design (user call, 2026-06-10):** the parked note tied the dump to the *implicit* moment a device leaves the exchange states (producer stopped ⇒ trivially no lapping). The shipped contract is simpler and more useful — **no state gate at all: dump tail→head at the instant of the call, in any state, OP included.** `start = oldestValidSeq()`, `end = head()` are snapshotted up front; cycles the producer records mid-write are simply not in this file. A record the still-running producer laps mid-read is skipped by `readRecord` and self-describes via its per-row sequence (so a gap needs no marker — and at a seconds-deep ring vs. a sub-second write it never actually happens). This makes the dump a plain "save what's in the recorder now" action rather than something you have to stop the bus to do.
- **`POST /api/process-data/dump`** → `{ "path": "…" }` on success; **409** when nothing has been mapped, the recorder is empty, or the file couldn't be written. No download/list/delete, no retention (MM binds `127.0.0.1` — the file is on the user's own machine; the OS owns the dir).
- **`dumpDir` is now a real `recorder` config key** (it was explicitly *not added* with the live recorder). Default `""` ⇒ resolved at dump time to `std::filesystem::temp_directory_path() / "motion-master"` (cross-platform — never a hardcoded `/tmp`, per the standing config rule). Threaded through `RecorderConfig` → `DeviceManagerConfig` → `config_` (captured at `init`, like `historySeconds`/`cyclePeriodUs`); the example JSONC now shows the whole `recorder` block.
- **UI:** a minimal "Dump recorder ring" button on the Process Image page (`ProcessImagePage.tsx`), with an action description (what it captures, that it works while exchanging, and that the returned path is local to the MM machine) per the UI-explains-actions rule. Shows the written path on success, the server error on 409. A full `.mmpd` file viewer (per-device/parameter/window decode) is still a separate follow-up.
- **Follow-ups done:** `swagger.yml` documents the endpoint; both generated clients regenerated (`ui/packages/api-client`, `hil/api/src/mm-api.ts`); a `hil/api` system test asserts the no-image 409; `process_data_dump_test.cc` covers round-trip, empty span, pad/truncate, and skipped-record handling. **Open / next:** the offline `.mmpd` viewer UI; the two-delivery-mode *last-N telemetry* path is still the other recorder fast-follow.

---

## Session 2026-06-12 — Remote/LAN deployment (Raspberry Pi appliance) and the wildcard cert (design)

**DESIGN ONLY — not yet in code.** Captures the deployment shape and the TLS solution for running Motion Master on a separate machine the PWA reaches over the network, rather than on the same host as the browser. **Extends** *Session 2026-05-23 — TLS certificate automation* and *Session 2026-06-06 — TLS cert auto-update*; the localhost-only model both of those assume is the thing this session generalises.

**The scenario.** Ship Motion Master as a flashable Raspberry Pi image (a headless EtherCAT-master appliance). Users flash it, plug the Pi into their network, and point the hosted PWA (`https://motion-master.synapticon.com`) at the Pi by changing the host it connects to. The Pi sits at some LAN address — `192.168.1.50`, not known ahead of time.

**Why the current cert breaks the moment the server leaves loopback.** Everything shipping today leans on one trick: the bundled Let's Encrypt cert is for `local.motion-master.synapticon.com`, and that name has a *public* A record pinned to `127.0.0.1`. It works only because the server is on the same machine as the browser. A Pi on the LAN fails all three ways at once: (1) the browser connects to the Pi's IP/`.local` name, which doesn't match the cert's SAN → name mismatch; (2) you can't get a publicly-trusted cert for a private IP or the reserved `.local` TLD anyway; (3) self-signed is a non-starter because the PWA reaches the server via **cross-origin `fetch()`**, which a browser refuses with *no* click-through once the cert is untrusted (the same constraint that forced cert self-heal into the binary — see 2026-06-06).

**Decision: a wildcard cert over an IP-encoding subdomain (the `sslip.io`/`nip.io` pattern).** Generalise the localhost-DNS trick from one fixed IP to all of them, reusing the DNS-01 + acme-dns machinery already built:

1. Stand up a tiny authoritative DNS responder (sslip.io is open-source Go, effectively one file) for a delegated subdomain, e.g. `*.ip.motion-master.synapticon.com`. It parses the IP out of the leftmost label and answers with it: `192-168-1-50.ip.motion-master.synapticon.com → A 192.168.1.50`. Works for **any** LAN address with no per-device registration.
2. Issue a **wildcard** cert `*.ip.motion-master.synapticon.com` via DNS-01 — Let's Encrypt issues wildcards, but *only* over DNS-01 (never HTTP-01/TLS-ALPN-01), which is exactly the challenge type `cert-renewal.yml` already runs. Needs a *second* acme-dns CNAME delegation, parallel to the existing one, for `_acme-challenge.ip.motion-master.synapticon.com`. **One** wildcard cert covers every Pi at every address — far easier on the 50-certs-per-domain-per-week rate limit than per-device certs, which it sidesteps entirely.
3. Bake that wildcard cert+key into the Pi image, same mechanism as the bundled localhost cert today.
4. The PWA host-change field targets `https://192-168-1-50.ip.motion-master.synapticon.com:61447` (client transforms the entered IP → the dashed hostname). Name matches the wildcard, public CA, no browser exception.

**Wildcard scope caveat.** A wildcard covers exactly one label: `*.ip.…` matches `192-168-1-50.ip.…` but not `a.b.ip.…` nor the bare apex `ip.…`. Fine here — every device name is exactly one label below the wildcard. (Optionally add the apex as a second SAN if it's ever wanted.)

**Key-distribution threat posture — unchanged, still acceptable.** The wildcard key ships in every image, so anyone can extract it and impersonate `*.ip.motion-master.synapticon.com`. But that only enables MITM of connections to *that* domain, which always resolve to a LAN address the attacker must already be on-path for. Identical posture to the localhost keypair already published on the `tls-cert` rolling release — consistent, not a regression.

**Discovery is a separate problem from the cert.** The wildcard scheme still needs the user to know the Pi's IP to build the hostname. Ladder by effort: manual IP entry (client transforms it to the dashed name) → mDNS/Avahi (`motion-master.local` advertised by the Pi; resolve, then still connect via the `.ip.…` name so the cert matches). Raspberry Pi OS ships Avahi, so mDNS is cheap.

**OS / packaging.** Raspberry Pi OS Lite (Bookworm, **64-bit / arm64**) is the pragmatic base — it *is* Debian, so the existing `.deb` (with its `setcap` postinst for the EtherCAT raw socket + RT caps) installs cleanly, and it has the best Pi hardware + Avahi support. Stock upstream Debian arm64 images for the Pi also exist if pure Debian is preferred. **Gap:** release artifacts are `linux-x64`/`amd64` today; `build-linux-arm64.yml` already builds the arm64 binary, but `tools/package.sh` / `release.yml` would need an arm64 `.deb` (and, eventually, an image-build) leg added.

**Alternatives rejected:** self-signed + click-through (dead — cross-origin `fetch()` gives no click-through); a private CA users import into their trust store (works, but a per-customer root-install support burden); `.local` + public cert (impossible — reserved TLD, no CA will issue). **Escape hatch if the DNS machinery ever feels too heavy:** serve the PWA *from the Pi itself* (same-origin instead of hosted cross-origin) — same-origin navigation *does* allow a one-time self-signed exception, dropping all the wildcard/DNS plumbing. Not chosen now since the hosted+acme path is already built, but it's the clean fallback.

**Status / next:** design only. To implement: (a) deploy the sslip.io-style responder under `ip.motion-master.synapticon.com` + the apex delegation; (b) add the second acme-dns CNAME and a wildcard issuance to `cert-renewal.yml` (or a sibling); (c) bake the wildcard cert into the Pi image; (d) client-side IP→dashed-hostname transform in the host-change UI; (e) arm64 `.deb` + image leg in the release pipeline; (f) optional Avahi discovery.

---

## Session 2026-06-12 — On-disk parameter-definition cache (`ParameterCache`)

**IMPLEMENTED.** Enumerating a drive's object dictionary over the SDO-Information service is hundreds of mailbox round-trips and takes *seconds* per device on every scan. The result — the parameter *definitions* (index, subindex, name, data type, bit length, access, unit, static default/min/max bounds), **not** the live values — is identical for every device of the same `(vendorId, productCode, revisionNumber)`. So persist it once per identity and reuse it on a later scan of the same hardware.

- **`ParameterCache` (`libs/node/parameter_cache.{h,cc}`)** — a single keyed store, one human-readable JSON file per identity (`parameters-<vendor>-<product>-<rev>.json`) under a per-user cache dir (`$XDG_CACHE_HOME`/`~/.cache` on Linux, `%LOCALAPPDATA%` on Windows, `~/Library/Caches` on macOS), or an explicit `directory`. Atomic temp+rename writes. A missing, unreadable, corrupt, wrong-identity, or wrong-`kFormatVersion` file is a **clean miss** → live enumeration; a write failure is logged, never propagated. All operations are best-effort and never throw.
- **Ownership — a new owned member of `DeviceManager`, control-plane only.** `DeviceManager` holds `ParameterCache parameterCache_` as a **direct member** (not a `unique_ptr`): created once, never replaced, so the pointer stays stable across the `scan()`/`reset()` rebuilds of the `devices_` vector. Each `Device` borrows it by `const ParameterCache*` (null ⇒ always enumerate). `Device::initializeParameters` consults it: a hit skips both the enumeration **and** the bounds decode; a miss enumerates then persists. This is the third owned collaborator on `DeviceManager` alongside `FieldbusDriver` and `ProcessData` — the class diagram (`docs/CLASS_DIAGRAM.md`, `CLAUDE.md` "Class Structure") was updated to add it.
- **Definitions only — never the live `value`/`syncState`.** On a hit each parameter's value is reset to the type-appropriate default and read live from the device, so a stale cache can at most cost a re-enumeration, never surface a stale value as if current. The file is a definition/schema snapshot (intended to later carry ESI-derived descriptions / enum labels).
- **Policy: Synapticon-only by default.** The cache is keyed on identity *alone*, which is only correct for a vendor that bumps its revision whenever the OD changes — true for Synapticon (`0x22D2`), not assumable for arbitrary third parties. So it is enabled by default only for Synapticon; `cacheAllVendors` opts every vendor in, and `enabled` is a master off-switch. JSONC `parameterCache` block (`{ enabled, cacheAllVendors, directory }`, mapped 1-1 into `ParameterCacheConfig` in `main.cc` via `DeviceManager::configureParameterCache`; documented in `motion-master.example.jsonc`). This is a second real config consumer after `recorder`.
- **Management surface, policy-independent.** `list()` / `readRaw(id)` / `remove(id)` / `makeId(...)` back a *Parameter Caches* page (Data section) for inspecting and cleaning up cached dictionaries (download to inspect offline; delete to force a fresh enumeration, e.g. when a third-party vendor reused a revision across a dictionary change). The `<vendor>-<product>-<revision>` id encoding lives **entirely in the cache** — `list` reports it, `readRaw`/`remove` parse it — so neither the HTTP layer nor the UI ever formats or parses the key (dumb-HTTP-layer rule). Management deliberately **ignores** the enabled/vendor policy: you must be able to inspect/delete files even with caching off. Routes: `GET /api/parameter-caches`, `GET /api/parameter-caches/{id}` (download), `DELETE /api/parameter-caches/{id}`.
- **Threading: no RT impact, no new thread, no new primitive.** Every touch is on the control plane — the HTTP/scan thread during `Device::initializeParameters`, and the management endpoints — guarded by the existing `busMutex_`/control-plane serialization. The file I/O is the same class of off-RT blocking work as FoE and the cert fetch. `docs/THREADS.md` is therefore unchanged: the cache adds no thread to the five and crosses no RT boundary.
- **Tests:** `parameter_cache_test.cc` covers hit/miss, identity mismatch, format-version mismatch, corrupt file, atomic write, and the `makeId`/`readRaw`/`remove` id round-trip.

## Session 2026-06-15 — Process Data page + batch output staging (shipped); WebSocket output-staging is a non-RT optimisation, not a determinism play

**SHIPPED.** A bus-level *Process Data* page (Data section): a live two-column mirror of the whole process image. Inputs (TxPDO) stream from a reserved page-owned monitoring over the WebSocket; outputs (RxPDO) are editable and sent together with one click, each staged into the output image and sent on the next RT cycle, then re-sent every cycle (set-once, sent-continuously).

- **`DeviceManager::stageProcessDataOutputs(span<OutputStageRequest>) → vector<OutputStageResult>`** stages a batch in one control-plane call, routing each object through the existing `Device::writeParameter`, and returns a per-item `{staged, error}` so the batch never fails wholesale (one object not output-mapped / a coercion error doesn't sink the rest). `POST /api/process-data/outputs` forwards the compact `[[slavePosition, index, subindex, value], …]` payload — the monitoring `[[pos, index, sub]]` shape with the value appended, so the wire stays consistent.
- **Reuse over a hand-rolled slot copy is deliberate.** The "just memcpy" intuition misleads: the wire bytes don't exist until the loosely-typed JSON value is coerced + encoded to the object's declared type, and that encode (`encodeSdoBytes`) *is* the copy — its result is what `writePdo` stores into the per-object `std::atomic<uint64_t>` (Design B). Routing through `writeParameter` also keeps the `DeviceParameter` cache coherent (so a later read/read-back reflects the staged value) and gives validation for free. It is an off-RT operation at human-click rate, so micro-efficiency is irrelevant. Critically, `writeParameter`'s `parametersMutex_` serialises only *non-RT* writers — the RT loop reads `outputSlots` lock-free — so staging never touches RT timing.

**Design note — the planned WebSocket output-staging channel buys throughput, not determinism.** The bidirectional WebSocket is slated to gain a client→server "stage these outputs" message (placeholder in `ws_server.h`; dispatch handler not written) so a client can push outputs over the already-open socket instead of one HTTP request per update. The use case is *continuous high-rate streaming* — a jog slider, gamepad, or external control loop pushing a target every frame — where per-update HTTP (request line/headers/route/alloc, possibly a CORS preflight) is wasteful. **It is not a real-time feature, and on an RT box it gives no RT gain.** Reasoning, recorded so it isn't re-litigated:

- **Two independent clocks.** (1) *Cyclic output cadence* — the RT loop wakes every period, reads whatever is in the `outputSlots` atomics, composes the wire image, sends; its jitter (`jitter_bench`: single-digit-to-tens of µs on PREEMPT_RT) is governed entirely by SCHED_FIFO + `mlockall` + absolute `clock_nanosleep`, and is **independent of how the value reached the slot** — the RT loop never synchronises with the writer. (2) *Command latency* — when a freshly-staged value first appears in the slot. Only (2) is affected by the transport, and it lives entirely in userspace + the network stack.
- **The HTTP/WS handler is a plain `SCHED_OTHER` thread** — on an RT box it is the *low-priority* citizen the RT thread preempts. So *when a command lands* is best-effort for both transports; the RT kernel does not elevate it.
- **WS's win over HTTP is only the magnitude and variance of clock (2):** a frame on a warm, already-handshaken socket (tens of µs, no request/response ceremony) vs an HTTP request (hundreds of µs–low-ms, more variance, connection/header overhead). Real for a 100 Hz stream — but p99 still spikes whenever the non-RT thread is scheduled late, for *either* transport. RT Linux guarantees the output *cadence* survives those spikes; it does **not** guarantee command-arrival time.
- **The only deterministic target path is generating it inside the RT context** — the planned fixed-membership `SineWaveTask`/generator `CyclicTask`s (*Session 2026-06-05 — RT tasks are fixed-membership*), which compute the value on the RT thread and write the slot directly. WS output-staging is the *client-sourced* counterpart of those: externally-pushed setpoints vs internally-computed ones, both ending as last-writer-wins deposits the RT loop re-sends every cycle. A 100 Hz stream into a 1 kHz loop simply re-sends each value ~10 cycles until the next arrives — fine for jog/teleop, never a hard-RT guarantee.
- **Conclusion.** The shipped HTTP set-and-send is the complete, correct answer for the page; adding WS staging there would buy nothing. Build the WS channel only when a genuine high-rate streaming client exists, and frame it as a throughput/overhead optimisation — never as a determinism mechanism. Anything needing determinism is an on-RT generator, not a transport choice.

## Session 2026-06-29 — C++ route-plugin extension point (`mm::api` + `libs/example`)

**SHIPPED.** A way to add application-specific C++ HTTP endpoints without editing `http_server.cc` — the server-side analogue of the `web/apps/example` PWA starter. A new copy-me library `libs/example` (`mm::example`) registers `GET /api/example/devices` (a trimmed `DeviceSummary` view) as a worked example; copy the directory, rename, replace the domain code and routes.

**The reusable piece is a new layer, `mm::api` (`libs/api/web_api.h`), not anything in `mm::node`.** This was the design crux. The plugin needs two things the built-in routes use: the response helpers (`sendJson`/`sendError`/`sendStatus` — uniform content-type + CORS, with the U+FFFD `replace` error handler so a non-UTF-8 device string can't throw on the uWS loop and kill the server) and a handle to the live collaborators. A first cut put these in `mm::node` next to the domain types; that was wrong. `web_api.h` `#include`s `<uwebsockets/App.h>`, so parking it in `node` would make the **transport-agnostic domain layer depend on the web framework** (and drag uWS into `node`'s unit tests). The fix is the layer CLAUDE.md already reserved a namespace for: `mm::api` sits *above* `node` and *below* the app, and is the only place that knows uWS. `node` stays clean.

- **`RouteContext`** — an aggregate of references (`DeviceManager&`, `MonitoringManager&`) plus `corsOrigin`, all of which outlive the running server. It is a temporary handed to the plugin *by const ref at registration time*; a handler must capture the individual fields it needs, **never the `RouteContext` itself** (it dies when registration returns). Being reference members of an aggregate, they are always initialised by aggregate-init — cppcheck's `uninitMemberVarNoCtor` fires on them anyway (it wants a constructor that can't exist for references), so that one check is suppressed for `web_api.h` rather than contorting the struct.
- **`RegisterRoutesFn = function<void(uWS::SSLApp&, const RouteContext&)>`** — the plugin contract. `HttpServer::addRoutes(fn)` queues a module (rejected with a warning if called after `start()`); the composition root (`main.cc`) is the only place that names the concrete plugin: `httpServer.addRoutes(mm::example::registerRoutes)` before `start()`. Modules run **once, on the HTTP event-loop thread, after the built-in routes and before the `OPTIONS /api/*` preflight + catch-all 404 + `listen()`**. uWS routes by specificity, so registration order doesn't affect matching — a plugin must only claim its own paths (`/api/example/...`), never `/api/*` or `/*`.
- **The route chain is built on the named `app`, not a moved/aliased one.** Interleaving the plugin loop means breaking what used to be a single fluent `std::move(app).get(…).…listen().run()` chain into three statements. uWS's builder methods return `TemplatedApp&&` *by reference to `*this`* and are **not** ref-qualified, so `std::move(app)` performs no move (`app` stays valid) and the methods are callable on the lvalue. So the three phases just operate on `app` directly (build-ins → `module(app, ctx)` → `app.options(…).listen(…).run()`); the earlier `uWS::SSLApp&& configured = …` alias added a misleading `&&`/lifetime cue for what was only a second name for `app`, and was dropped.

**`libs/example` mirrors the codebase split:** `example_logic.{h,cc}` is pure, HTTP-agnostic domain logic (`summarizeDevices(DeviceManager&)` — unit-testable with no server, returns `[]` offline), and `example_routes.{h,cc}` is the thin layer that formats the response. It links `mm::node` (domain) + `mm::api` (transport glue; pulls in uWS + nlohmann transitively).

**Not added to `swagger.yml`, deliberately.** `/api/example/...` is demonstration scaffolding meant to be deleted/renamed, not a stable contract — documenting it in the canonical spec would imply otherwise. It also lives in the plugin lib, not `http_server.cc`, so it adds no built-in endpoint and the `api-client-drift` CI job (which diffs the generated client against `swagger.yml`) stays green. The plugin *pattern* is documented in prose (README "Extending the API", this note) instead.

## Session 2026-06-30 — Diagram & threading docs catch up to the route-plugin seam

Pure documentation: the route-plugin work (2026-06-29) landed in code and in `CLAUDE.md`'s mandates but never in the two hand-maintained diagrams, `docs/CLASS_DIAGRAM.md` and `docs/THREADS.md`. Reconciled both, same as the 2026-06-08 walk-the-source pass.

- **Class diagram** now carries the `mm::api` seam: `HttpServer` gained `addRoutes(RegisterRoutesFn)` + the `routeModules_` vector; new `RouteContext` (`mm::api`) and `RoutePlugin` (`mm::example`) nodes with their relationships (`HttpServer o-- RoutePlugin`, `HttpServer ..> RouteContext`, `RouteContext ..> DeviceManager`/`MonitoringManager`); an ownership-table row for `routeModules_`; and a prose "Route plug-ins (`mm::api`)" section.

- **Threading correction — the one substantive point, not just transcription.** The first cut of the class-diagram prose claimed plug-in handlers "add no thread and cross no RT boundary." That overreached: it's true of the *example* plugin and of the registration/handler model (registration runs once on the HTTP loop thread; handlers run on that thread per request), but a plug-in is **ordinary C++ holding `DeviceManager&`** and may spawn its own off-RT `std::jthread` for long-running work — exactly as `MonitoringManager`'s sampler/refresher do. The honest framing: the built-in process runs **five** threads, but five is the built-in count, **not a hard ceiling**. Any plug-in-spawned thread is bound by the same rules as every non-RT thread — serialize bus access through `FieldbusDriver::controlPlaneMutex_`, never touch the RT path. Fixed in all three docs (`CLASS_DIAGRAM.md`, `THREADS.md`, and the `CLAUDE.md` mandate bullet, which now says the *registration fn* runs once on the loop thread rather than implying all plug-in work stays there).

No code change; `docs/` and `CLAUDE.md` only.

## Session 2026-07-01 — `CyclicTask` lifetime: non-owning is correct; the invariant is "outlive `run()`", not "outlive the loop"

A design-review question on `GameLoop`: it holds `std::vector<CyclicTask*>` (non-owning) and documented that a registered task "must outlive the loop." Is non-owning the right call, and is that the right contract? Two separate answers.

**Non-owning is correct, and for the same reason ownership lives at the composition root everywhere else.** `GameLoop` is a pure RT scheduler — it ticks a `CyclicTimer` and calls `execute()` on a fixed set of tasks. The tasks themselves are thin adapters whose *real* lifetime is coupled to the domain objects they wrap: `ProcessDataCyclicTask` holds a `DeviceManager&`, and `DeviceManager` is owned at the composition root (`main.cc`). Making `GameLoop` own the tasks via `unique_ptr` would put the RT loop in charge of a lifetime that actually belongs to the DI graph. It would also fight the fixed-membership rule (Session 2026-06-05 — *RT tasks are fixed-membership*): membership never changes at runtime, so there is no lifecycle churn that would argue for the loop taking ownership. `GameLoop` stays a mechanism, not an owner — consistent with "`main.cc` is the only place concrete types are instantiated."

**But the documented contract was one notch too strong, and `main.cc` actually violated its literal form.** The pointers in `tasks_` are only ever dereferenced *while the loop is executing* — never after `run()` returns. So the real invariant is "a task must outlive every call to `run()`," not "outlive the `GameLoop` object." The distinction mattered because the original `main.cc` declared `gameLoop` **before** `processDataCyclicTask`:

```cpp
GameLoop gameLoop{...};                // constructed first
ProcessDataCyclicTask processDataCyclicTask{...};  // constructed later
gameLoop.addTask(&processDataCyclicTask);
```

Reverse-order stack destruction destroys `processDataCyclicTask` first — so the task did **not** outlive the loop as the doc claimed. Harmless today only because `run()` has already returned by the time either destructor fires, but a latent footgun if anyone reordered these or touched tasks post-`run()`.

**Fix: both, not either.** Swapped the declaration order in `main.cc` (task before loop, so the task is destroyed *after* the loop and the language-level lifetime honours the contract) **and** restated the precise invariant in the two doc comments (`game_loop.h::addTask`, `cyclic_task.h`) — "must outlive every call to `run()`," with a note that the pointer is dereferenced only while the loop executes. Also corrected "owned by the caller (App)" → "the composition root" in `cyclic_task.h`, since there is no `App` class yet. Docs + one declaration reorder; no behavioural change.

## Session 2026-07-06 — PDO remapping (roadmap #6): read + write the cyclic mapping over CoE

Roadmap item #6 (fieldbus capabilities, Session 2026-06-01) shipped: the user can now change *which* objects are in the cyclic image, not just view the existing mapping. End-to-end — `Device`/`DeviceManager` methods, `GET`/`PUT /api/devices/:slavePosition/pdo-mapping`, swagger + regenerated TS client, and a **PDO Mapping** editor page in the console.

**One CoE read source, two shapes.** The mapping is read over the CoE mailbox by walking the PDO assignment objects (`0x1C12` outputs/RxPDO, `0x1C13` inputs/TxPDO) and the mapping objects they reference (`0x16xx`/`0x1Axx`) via SDO upload. That single reader (`readPdoAssignment`) now feeds two views:

- **Grouped** (`Device::readPdoMapping → PdoMapping`): each mapping object keeps its `pdoIndex` and its own entries (with derived `bitOffset`) — the round-trippable shape the editor loads and the write echoes back.
- **Flat** (`Device::readFlatPdoMapping → FlatPdoMapping`, cached in `flatPdoMapping_`): one flat list of `PdoMappingEntry` per direction, what the process-image builder consumes. Now *derived* from the grouped read rather than a separate walk.

This drove the rename that touches the most files: `PdoMappings → FlatPdoMapping`, `readPdoMappings() → readFlatPdoMapping()`, `pdoMappings() → flatPdoMapping()` — "flat" is now explicit everywhere the old whole-image view is meant, so the grouped view owns the plain `PdoMapping` name. The CoE mapping-word pack/unpack (`index<<16 | subindex<<8 | bitLength`) moved to `pdo_mapping.h` alongside the grouped `to_json`.

**The write follows the ETG.1000.6 §5.6.7.4.9 ordering rule, transactionally.** `Device::writePdoMapping(const PdoMapping&)` reconfigures both directions in PRE-OP: clear the sync manager's PDO assignment to zero (which makes its mapping objects writable) → clear each mapping object's entry count → write the entries as packed `uint32` words → restore the entry count → finally write the assignment listing the mapping objects and its own count. A mapping object present before but absent from the new mapping is just left unassigned (no explicit clear needed — off the SM, its contents are irrelevant).

**Why PRE-OP only.** The mapping and assignment objects are writable *only* in PRE-OP — INIT/BOOT have no CoE mailbox, and in SAFE-OP/OP the sync managers are active so the slave aborts the write. This is the "drop to PRE-OP, remap, climb back" flow. The write is deliberately **transport-agnostic and does not itself change AL state or re-map the image**: the caller (the UI, via `POST /api/state`) drives the device back to SAFE-OP/OP, and `DeviceManager::transitionToState` re-reads the mapping and rebuilds the whole-bus process image — the same reactive-mapping path a firmware update already exercised (the 2026-05-16 "manual re-map may have changed it" clause). No new re-map machinery.

**Retry the whole sequence, not per-write.** After writing, the mapping is read back (`readFlatPdoMapping`, which also refreshes the cache) and compared against the request; a mismatch or a transient SDO failure mid-sequence retries the **entire** mapping apply a few times before failing. A single dropped mailbox frame would otherwise leave the OD half-configured (some objects written, the assignment count stale); the apply is idempotent so a whole-sequence retry is safe, and read-back is the only trustworthy success signal. Related same-device concurrency fragility (shared `controlPlaneMutex_` serializes individual transactions but not the multi-write sequence) is noted but not yet guarded — see the standing memory on PDO-mapping write concurrency.

**Monitoring re-classification fallout (`fix(node)`, a5b7933).** A monitoring classified each parameter's source (PDO vs SDO) once at creation and, on a re-map, only refreshed the decode spec of parameters *already* sourced from PDO. Editing a mapping therefore left the classification stale: a parameter removed from the image kept `source=pdo` and sampled null forever; one added kept `source=sdo` and read a stale background-poll cache. `recaptureIfRemapped` now re-classifies **every** plan against the freshly published image (mapped → PDO with a fresh spec, unmapped → SDO) and reconciles the `ParameterRefresher` refcounts on each transition (acquire on PDO→SDO, release on SDO→PDO). `processImageGeneration` bumps only on a successful re-map that publishes a new image (never on teardown), so a bus that merely pauses exchange causes no PDO↔SDO churn.

**Console editor.** A per-device **PDO Mapping** page (`devices/:deviceId/pdo-mapping` + sidebar entry) loads the grouped mapping and edits it — add/remove/reorder mapping objects and entries across Outputs (RxPDO) and Inputs (TxPDO), with live bit-offset and per-object/per-direction totals. Each entry uses the shared `ParameterPicker` constrained to objects mappable in that direction (`ObjAccess` RxPDO/TxPDO bits), auto-filling bit length from object metadata; index/subindex/bitLength stay hand-editable for padding gaps or objects absent from the OD. PRE-OP guidance + a not-in-PRE-OP warning, validation, and a verified read-back on write. The Process Image page picked up the same two-column Outputs/Inputs layout and per-direction totals.

## Session 2026-07-07 — CoE Complete Access for the parameter value-read pass

`Device::initializeParameters(readValues=true)` read every parameter with one SDO upload per subindex. The *definition* enumeration (SDO-Info: object list → object description → entry description) is already cached by `ParameterCache` (Session 2026-06-12), so on a warm cache the only thing left touching the bus is this value pass — and it is always live (values are never cached). That made it the right, and only remaining, target for speeding up a full read. The lever is **CoE Complete Access (CA)**: SOEM's `ecx_SDOread(..., subindex=0, CA=TRUE, ...)` uploads *all* subindices of an object in one mailbox transfer, collapsing an N-subindex object's N round-trips into one.

**Where the win actually is.** CA only helps **multi-subindex** objects — ARRAY (`0x08`) and RECORD (`0x09`). A VAR (`0x07`) has only subindex 0; a CA read of it is one transfer, identical to a plain read. So the speedup scales with how many ARRAY/RECORD objects a drive exposes (PDO mappings/assignments `0x16xx`/`0x1Axx`/`0x1C1x`, records), not the total object count. VARs and single-subindex objects stay on the per-subindex path.

**Driver seam.** New `FieldbusDriver::readSdoComplete(slave, index)` returning the raw CA blob. Added as a **virtual with a default "unsupported" implementation** rather than a pure virtual, so `SoemFieldbusDriver` overrides it while every test mock (and any future driver) inherits a graceful fallback with zero churn. The triplicated SDO-error decoding in `readSdo`/`writeSdo`/`readSdoComplete` was factored into one `sdoErrorSuffix()` helper.

**Blob layout (the risky part).** A CA upload starting at subindex 0 is *not* a clean concatenation: per ETG, subindex 0 is transmitted as a 16-bit value (1 data byte + 1 alignment pad), then subindices 1..N follow at their native bit lengths. `Device::readParameterValues` groups the definitions by object index, and for an eligible object does one `readSdoComplete` then slices the blob back per subindex with the existing `extractBits` (LSB-first, `process_image.h`) + `decodeSdoBytes`, advancing a bit cursor (16 bits for sub0, then `bitLength` each). Two guards keep a wrong blob from ever being silently mis-decoded: CA is used **only** when the run is contiguous from subindex 0 **and** every entry is byte-aligned (`bitLength % 8 == 0`), which sidesteps bit-packed records and gaps from a dropped enumeration entry; and if the blob is shorter than the layout implies or any slice fails to decode, the whole object falls back to per-subindex reads.

**Support detection — probe once, per-object fallback.** CA is optional in CoE and support can vary; a slave/object that lacks it answers with an SDO abort. Rather than parse abort codes, the pass treats *any* CA failure as "fall back": the first eligible object is the probe — if its CA read fails, CA is disabled for the rest of the pass; once a CA read has succeeded, an individual later object that fails falls back for itself only. A CA read that *succeeds* but whose blob won't decode marks CA supported (it works) and just falls back for that object. Correctness is preserved in every branch by the per-subindex path; the only cost of a bad guess is losing the speedup.

**Config + plumbing.** `parameters.useCompleteAccess` (JSONC, default **on**) → `ParametersConfig` → `DeviceManagerConfig` → `initializeParameters(readValues, useCompleteAccess)`, wired at the composition root and forwarded by `DeviceManager::initializeDeviceParameters`. Disable only for firmware with a broken CA implementation. The PRE-OP auto-read path is unaffected — it reads definitions only (`readValues=false`), so CA never runs there. No swagger change: the endpoint's request/response shape is unchanged; CA is a purely internal speedup on the always-live value read. Four unit tests cover the one-upload decode, the unsupported-slave fallback, the disabled-flag path, and a VAR skipping CA.

**Bulk read-all path (`POST /api/devices/:pos/parameters/read`).** The init path was only the first target; `readAllParameters` — the common "refresh every value" sweep, and the one usually hit — is now CA-aware too, but it is **PDO-aware**, which is the wrinkle. `readParameter` prefers the live process image for PDO-mapped subindices while exchanging, and CA is an SDO operation, so CA must not shadow the cyclic value. The sweep snapshots readable subindices grouped by object under the lock (released between objects), then per object: if the run is CA-eligible **and not currently image-served** (`ProcessData::readPdo` returns nothing for every subindex), one CA upload fills it; otherwise each subindex goes through the unchanged, PDO-aware `readParameter`. In practice the two sets are disjoint anyway (PDO maps single-sub VARs like statusword/position, never whole records), but the image-served gate makes that exact rather than incidental. The CA read + decode runs under `parametersMutex_` for one round-trip — matching `readParameter`'s own lock-across-one-SDO contract — so the re-found parameter pointers stay valid across the transfer. The shared machinery (`completeAccessEligible`, `decodeCompleteAccess`, a `CompleteAccessProbe` probe/fallback state machine, and `readCompleteInto`) was factored out of the init path so both use identical, tested blob-slicing; the helpers take `std::span<DeviceParameter* const>` so the init path (vector of definitions) and the bulk path (live map) both fit. Three more unit tests: bulk CA decode, unsupported→fallback, disabled-flag.

**Mailbox capabilities surfaced (`ECT_*DETAILS`).** SOEM reads each slave's mailbox capability bytes (`CoEdetails` / `FoEdetails` / `EoEdetails` / `SoEdetails`) from EEPROM during `ecx_config_init` — including `ECT_COEDET_SDOCA`, the device's **advertised** Complete Access support. These are surfaced as **raw bytes** on `GET /api/bus-config`: added to `MailboxConfig` (next to `protocols`, which they refine) and the `SlaveConfigInfo` gained the device **identity** (vendorId/productCode/revisionNumber/serialNumber, denormalised from the `Device` like `deviceName` already was). The decode into named flags lives **client-side** in one shared `MailboxCapabilities` component, fed the raw bytes by **both** the Configuration page (from bus-config) and the SII page (from the EEPROM General category) — a single decode path, both pages render identical chips. The Configuration slave card also now shows identity, so it is self-contained.

Decode is deliberately **not** wired into CA detection: the flag is an EEPROM advertisement firmware can get wrong in either direction, so gating on it risks a false-negative that disables CA on a capable-but-non-advertising drive — **confirmed on SOMANET**, which reports `completeAccess=false` yet CA works (the runtime probe succeeds). The probe/fallback stays authoritative; the flag is exposed for diagnostics only, with a UI tooltip spelling out that it is advertised-only and may read false while CA works.

This shape is the settled one after two iterations: `coe` first landed as a decoded `CoeCapabilities` on the device JSON + per-device header, then moved to bus-config; that server-side decode + the per-scan CoE debug line were both removed in favour of raw-bytes-on-bus-config + client-side decode, so there is exactly one place the bit meanings are defined. **Configuration and SII stay separate** (per that discussion): SII = the raw EEPROM image of one device read live over the bus; Configuration = what the master programmed at scan, cached, all slaves — overlapping in category but not in source or meaning (SM/FMMU can differ between them), so the overlap is intentional, not a merge.

**SII General-category off-by-one fix (found via the cross-check).** Displaying CoE capabilities two ways surfaced a latent bug: the **SII page** showed `CoE details 0x00` for a drive the bus-config panel showed as SDO+Info+PDO-Assign+PDO-Config. Root cause — `parseSii`'s `parseGeneral` (ported 1:1 from the old TypeScript parser) omitted the **reserved byte at payload offset 4** of the ETG.2000 General category, so every field from `coeDetails` on read one byte early; `coeDetails` landed on the reserved `0x00` while the real value (`0x0F`) was mislabelled `foeDetails`. SOEM confirms the layout (it reads `CoEdetails` at `siifind()+0x07`, and `siifind` returns the category *size-word* address, so payload base +2 → CoE at payload offset **5**). Fixed the offsets (coeDetails 5, foeDetails 6, eoeDetails 7, soeChannels 8, ds402Channels 9, sysmanClass 10, flags 11, currentOnEBus 12, physicalPort 14, physicalMemoryAddress 16) and widened `currentOnEBus`/`physicalMemoryAddress` to their true 16-bit ETG widths. The `DecodesGeneral` test (which had locked in the buggy values against a real Circulo dump) was corrected — and its `coeDetails == 0x0F` now agrees with what SOEM reports for the same drive, closing the loop. No API-shape change (same JSON keys, corrected values).

## Session 2026-07-08 — Cert self-heal on expiring-soon; the rolling release retires the TLS secrets

Two cert fixes, surfaced by a Docker report: a container logged `TLS certificate expires in 0 days` at startup while the PWA's Connection page showed a *fresh* 82-day cert for the same `/opt/motion-master/cert.pem`. Not a contradiction — the served file had been swapped after boot by a manual `POST /api/cert/refresh` (the **Refresh certificate** button); the startup self-heal had *not* done it, because it only fetched on missing/expired, and the baked cert was merely expiring-soon.

**Fix 1 — self-heal refreshes on expiring-soon, not just missing/expired.** `healCertIfNeeded` (`cert_updater.cc`) now computes three states (`certMissing` / `expired` / `expiringSoon`, the last being `daysRemaining < kCertExpiringSoonDays` = 7) and fetches on any of them; a cert with ample life left still makes **no** network call, so a healthy boot is unchanged. The fetch stays `autoUpdate`-gated and best-effort — a present cert that fails to refresh is still served (fatal only when the cert is missing *and* unfetchable). This is what makes an ephemeral container self-heal on start, and it also fixes dev images for free: the entrypoint's last-resort **1-day self-signed** cert falls inside the 7-day window, so it now triggers a real-cert fetch on first run instead of being served as-is (the PWA rejects self-signed cross-origin). No entrypoint reorder needed.

**Fix 2 — the Docker image never actually baked a cert.** The build stage `touch`ed empty `cert.pem`/`key.pem` at the repo root, but the runtime stage copied only the binary — a runtime cert `COPY` was **never** in the Dockerfile (checked history; even the "bake release certs" commit lacked it). So every image shipped certless and self-signed. Fixed: the build stage now **fetches** cert/key from the rolling `tls-cert` release (fetch-to-`.new` + `mv`, empty-placeholder fallback so offline builds still succeed), and the runtime stage `COPY`s them into `/opt/motion-master/`.

**Full retirement of the `TLS_CERT`/`TLS_KEY` secrets and the `GH_PAT_SECRETS` PAT.** They duplicated the rolling `tls-cert` release — both were written by the same `cert-renewal.yml` run — so the secret path was redundant. The **rolling release is now the single source of truth**: `cert-renewal.yml` only publishes there (default `GITHUB_TOKEN`, `contents: write` — no PAT); `release.yml`'s three build legs `curl` it into the artifacts instead of `echo`ing the secrets; the Dockerfile `curl`s it at build. The `gh secret set` step is gone. Because the binary now self-heals at runtime regardless of install method, a baked cert is just a fresh offline seed, not load-bearing — the hybrid we settled on (bake for offline + refresh when stale). Supersedes the secret-based description in the CI section above and in Session 2026-06-06. The now-unused `TLS_CERT`/`TLS_KEY` secrets and `GH_PAT_SECRETS` PAT can be deleted from repo settings.

Also this session: `tools/docker-build.sh` (build + tag from `VERSION`; bare version + `latest`/`next` by stable/prerelease) and the README restructure (running vs. development, config-driven CLI, real platform/artefact set).

---

## Session 2026-07-09 — Trajectory playback: a fixed-membership RT task fed a precomputed setpoint buffer (design)

**Extends** *Session 2026-06-05 — RT tasks are fixed-membership; off-RT procedures are background jobs*. A `TrajectoryCyclicTask` that plays back a vector of position points one-per-cycle (chirp, step sequence, replay, arbitrary offline-generated signal) is a textbook **Category-1 RT cyclic procedure** — it writes a fresh target into the output region every cycle, phase-locked to the bus — and so it obeys every rule that session set: **one fixed-membership `CyclicTask`** registered before `run()` in `main.cc` alongside `ProcessDataCyclicTask`, a **no-op per device until activated**, iterating devices each cycle and holding only `DeviceManager&`; a **per-device control block** flipped active/idle by the HTTP thread; **the launch lives on the view** (`Cia402Drive::startTrajectory(...)`/`stopTrajectory()`), which does the op-mode + enable handshake synchronously off-RT so the RT side has no state-machine logic and no error branch; and the fixed pool is **one control-block slot per `Device`**. This is the SineWave design with the generator swapped for a buffer reader — the trajectory is *precomputed* rather than computed from scalar params each cycle.

**The one thing that changes: a `SeqLock<T>` cannot carry the control block, because the payload is a `vector`, not scalars.** SineWave's transport is `SeqLock<SineWaveParams>`, which requires `T` trivially copyable (a handful of doubles). A trajectory is an arbitrarily long `std::vector<int32_t>` — not trivially copyable, and far too large to copy on every RT `load()` even if it were. So swap the transport for the **atomic-raw-pointer + retained-generations** idiom already proven in `ProcessData` (`process_data.h`: `std::atomic<const ProcessImage*> image` + `std::vector<std::shared_ptr<const ProcessImage>> generations`; Session 2026-06-08). It is the *only* publish mechanism in this tree that is genuinely wait-free on the reader side — deliberately **not** `std::atomic<std::shared_ptr<>>`, whose `load()` is not lock-free on libstdc++ (spinlock / split-refcount fallback) and would inject jitter or a lock onto the RT thread.

```cpp
struct TrajectoryBuffer {                 // immutable once published; mlock'd off-RT
  std::vector<int32_t> points;            // one target position (0x607A) per cycle
  enum class EndPolicy : uint8_t { HoldLast, Stop, Loop } endPolicy;
  uint64_t generation;                    // bumped per publish so RT detects a new buffer
};

struct TrajectoryControl {                                    // per-device, unique_ptr on Device
  std::atomic<const TrajectoryBuffer*> buffer{nullptr};       // wait-free load on RT
  std::atomic<bool> active{false};
  std::atomic<uint64_t> completedGeneration{0};               // RT → off-RT completion signal
  std::vector<std::shared_ptr<const TrajectoryBuffer>> generations;  // keep-alive, control-plane only
};
```

**The handoff, off-RT (`Cia402Drive::startTrajectory`, HTTP thread).** (1) Parse the JSON point array into `vector<int32_t>`, **bounded by a config max length** so the allocation is predictable and a client can't ask for gigabytes. (2) Build the `TrajectoryBuffer`, wrap in `shared_ptr<const>`, and **`mlock` its backing storage** — the RT thread reads it every cycle, so an unpageable buffer avoids fault-induced jitter (same reasoning as the mlock'd recorder ring, Session 2026-06-08). (3) Do the CSP + enable handshake synchronously on this off-RT thread — `setOperationMode(CSP)`, **seed the output setpoint from the current actual position** (`positionActualValue()` → stage 0x607A) so enabling causes no jump, then `enable(timeout)`; bail with an `expected` error on any failure, leaving RT untouched. (4) Retain the `shared_ptr` in `control.generations`, then publish: `buffer.store(ptr.get(), release)` → `active.store(true, release)`. By the time the RT task observes `active`, the drive is already `OperationEnabled` + CSP — exactly the SineWave invariant that keeps all validation on the launcher's `expected<>` path.

**The handoff, RT (`TrajectoryCyclicTask::execute`, per device).** Re-resolve the `Device` via `DeviceManager::findDevice` each cycle (never cache across a rescan). `active.load(acquire)`; if false, skip. `buffer.load(acquire)` — the wait-free raw-pointer read; never dangling because the control plane retains it in `generations` until `reset()`/`scan()`, exactly as `ProcessData` retains superseded images. Keep the **playback cursor as RT-only scratch** (a member on the task, off the published block — the same rule that keeps SineWave's `phase_`/`center_` off its seqlock: the block is HTTP→RT only, readers must never see RT scribbling the cursor back), reset to 0 on a `generation` change. Write the current point via the existing lock-free output path `ProcessData::writePdo(slavePos, 0x607A, 0, bytes)` (Design B, Session 2026-06-05 — safe from RT). At end of buffer apply `endPolicy` and store `completedGeneration`. Ordering: runs *after* `ProcessDataCyclicTask` in registration order like SineWave (one cycle of staging latency — fine for a chirp; register before it only if same-cycle staging is worth the coupling).

**Stop, completion, lifetime — all deferred to off-RT, none on the RT thread.** *Stop* is `active.store(false)` plus the launcher's off-RT disable walk (state changes are never done on RT); the buffer stays alive in `generations` until the next `reset()`/`scan()` so an in-flight RT cycle never reads freed memory. *Completion* can't be an RT notification (no I/O on RT), so RT only flips `completedGeneration`; an off-RT watcher (a small poller, or folded into the existing `MonitoringManager` sampler thread) observes it and emits the WebSocket notification and/or auto-disables — the job of the planned `NotificationBus`. *Rescan safety*: the control block dies with its `Device`, so a trajectory must be stopped before a rescan (like `stopExchange`), and the task's per-cycle `findDevice()` must sit behind the same `scan()`/`reset()` drain the `ProcessData` path relies on — the same open guard flagged for SineWave.

**When the whole-buffer publish is wrong.** It fits a **finite, precomputed** signal — a 1 kHz chirp for a minute is ~240 KB, trivial. If the trajectory is **open-ended / streamed** (generated live, does not fit in memory), replace the single buffer with an **SPSC lock-free ring**: HTTP thread pushes chunks, RT pops one point per cycle, a low-water mark notifies off-RT to refill, with explicit underrun handling. More moving parts, and only worth it when the buffer genuinely cannot be materialised up front — not the case here, so start with the buffer publish.

**The model in one line.** *Trajectory playback is SineWave with the per-cycle generator replaced by a cursor into a precomputed buffer; every fixed-membership / view-launched / one-slot-per-`Device` rule carries over unchanged, and the only substitution is the control-block transport — `SeqLock<scalars>` becomes atomic-pointer-publish-with-retained-generations because the payload is a `vector`.*

## Session 2026-07-09 — The profile view is RT-callable: `Cia402Drive(device)->state()` works verbatim in RT and non-RT (design)

**The non-negotiable requirement.** `createCia402Drive(device)->state()` — and every other profile operation (`setControlword`, `shutdown`/`switchOn`/`enableOperation`, `setOperationMode`, `setTargetPosition`, all the CiA402 bit work) — must run **from the same call site, unchanged, on both the RT loop and an HTTP thread.** The alternative is a second copy of the state machine, the controlword-bit composition, and the mode handshakes living inside every RT task — a large, drift-prone duplication of exactly the knowledge `Cia402Drive`/`SomanetDrive` exist to hold *once*. So the profile layer stays single-source and RT-safety is pushed *below* it, into the one seam every profile method already bottoms out in: `Device::readValue<T>` / `writeValue<T>`. This is what an RT `TrajectoryCyclicTask` / `SineWaveTask` uses to drive state instead of reaching for `ProcessData::writePdo(pos, 0x607A, 0, bytes)` raw (as the 2026-07-09 Trajectory session sketched) — the raw call works but bypasses the profile, which is the very thing we refuse to reimplement.

**Why it isn't true yet** (this reconciles the aspirational claim in *Session 2026-06-05 — Design B*, which already said `readParameter`/`writeParameter` serve the value "identically from an HTTP handler or an RT task"). Two things make the current path unsafe to call from the RT loop:

1. `Device::readParameter`/`writeParameter` take `parametersMutex_` (a blocking `std::mutex`) around the **whole** body (`device.cc:761`, `:873`) — a lock on the RT cycle, priority-inversion against the monitoring/refresher/control-plane threads. Forbidden by this codebase's own RT rules.
2. `ProcessData::readPdo` returns `std::optional<std::vector<uint8_t>>` — a heap allocation per read, per cycle.

And `DeviceParameter::value` cannot itself be the shared RT cell (the tempting "just read the value from memory" model): it is a `DeviceParameterValue` variant whose alternatives include `std::string`/`std::vector`, so a cross-thread read while the RT loop writes it is a torn variant — mismatched discriminant vs. payload (UB), or a string read mid-reallocation (use-after-free). That is exactly *why* the lock-free RT structures are the fixed-width atomic `outputSlots` and the POD recorder ring, never the parameter map. The value the RT reader wants already lives in memory — in the ring record (inputs) / staging slot (outputs) — just not in `->value`.

**The design: dispatch by bus-state, not by thread.** Make `readValue<T>`/`writeValue<T>` choose the transport at runtime on a condition that is RT-safe *exactly when the RT task needs it* — "**is a live image published**" (the device is exchanging), **not** "am I the RT thread":

```cpp
template <typename T>
std::expected<T, std::string> readValue(uint16_t idx, uint8_t sub) {
  if (processData_) {                          // fast path — lock-free, alloc-free
    alignas(T) std::byte buf[sizeof(T)];
    if (readPdoFast(idx, sub, buf))            // ring (inputs) / slot (outputs); T known ⇒ no map lookup
      return decodeScalar<T>(buf);             // LE decode, no heap
  }
  return readParameter(idx, sub) ...;          // slow path — SDO/cache; reachable only when NOT exchanging
}
```

The RT trajectory/sine task only runs while the device is in OP, so it **always** takes the fast path and can never reach the SDO branch. Off-RT callers take the fast path when exchanging and the SDO branch only when offline. One method body, safe in both, by construction — and the whole `Cia402Drive`/`SomanetDrive` chain above it stays transport-agnostic and never learns which world it is in.

**Three pieces to build.**

1. **A non-allocating `readPdo` overload** — `bool ProcessData::readPdo(pos, idx, sub, std::span<uint8_t> out)` decoding ≤8 bytes into a caller buffer (re-checking the ring sequence for tear-safety, same as the vector overload today), sitting alongside the vector-returning one the HTTP path keeps.
2. **Thin `Device` helpers `readPdoFast` / `stageOutput`** — declared in `device.h`, **defined in `device.cc`.** Non-inline on purpose: an inline fast path would force `device.h` to `#include process_data.h` and drag the heavy recorder/buffers into every `mm::node` consumer, breaking the "only device.cc pulls the buffers" encapsulation. The helpers touch **neither** `parameters_` **nor** `parametersMutex_` — they delegate straight to `readPdo`/`writePdo`.
3. **The fast path lives in the typed templates** — in `readValue<T>`/`writeValue<T>`, because `T` is known there, so decoding/encoding needs no data-type lookup in the parameter map (the lookup is *what forces the lock* in the non-template path). The non-template `readParameter`/`writeParameter` keep their locked+cached+SDO body verbatim for generic HTTP handlers that don't know `T`. Every RT-relevant CiA402 accessor is already templated with a known type (`statusword`→`readValue<uint16_t>`, `setControlword`→`writeValue(...,uint16_t)`, `setTargetPosition`→`writeValue(...,int32_t)`, …), so they all inherit the fast path for free.

**Two behaviour changes this forces — both accepted.**

- *The health/WKC gate becomes advisory, not a diverter.* Today `readPdo` returns `nullopt` for inputs on a short working counter, so `readValue` falls through to a **blocking** SDO upload — fatal on RT. So the fast path serves the newest ring record **unconditionally while exchanging** (the freshest real value we have), and health/WKC is surfaced separately for the caller to react to, not used to silently divert into SDO. Off-RT effect: an exchanging-but-momentarily-unhealthy bus now serves the last recorded value instead of a fresh SDO upload — consistent with what the wire actually carried, and the price of one uniform call. Design B already made the analogous change for *output* read-back; this extends it to *inputs* on the fast path.
- *The live fast path does not write the cache.* Storing the decoded value into `parameters_` (as `readParameter` does at `device.cc:777`) needs the lock. So after this lands `parameter(0x6041)->value` reflects the last *offline/SDO* value while `readValue<uint16_t>(0x6041,0)` reflects *live* — arguably more correct (cache = offline snapshot, ring = live truth), but a real shift for any code reading `->value` expecting freshness. Monitoring reads the ring, not the cache, so it is unaffected.

**The boundary: only the *waiting* differs, never the profile logic.** Every atomic profile op is shared verbatim (state decode, all transitions, mode/setpoint writes, all the `kCommandMask` bit work — none of it touches threads or transport, it all bottoms out in `readValue`/`writeValue`). The **only** method that is not RT-callable is `enable()`, and not for a profile reason: it `sleep`-polls for OperationEnabled, and sleeping is a non-RT construct. On RT you do not reimplement it — you drive the same shared transition methods **one step per cycle**, keyed off `state()`:

```cpp
auto drive = createCia402Drive(device);        // same factory, same view, re-resolved each cycle
switch (*drive->state()) {                      // same decode, RT and non-RT
  case State::Fault:            drive->faultReset();          break;
  case State::SwitchOnDisabled: drive->shutdown();            break;
  case State::ReadyToSwitchOn:  drive->switchOn();            break;
  case State::SwitchedOn:       drive->enableOperation();     break;
  case State::OperationEnabled: drive->setTargetPosition(next); break;
}
```

This is precisely what *Session 2026-06-05 — borrowed views* meant by "single-shot, here-and-now operations only; multi-cycle procedures belong in a `CyclicTask` that re-resolves its `Device` each cycle." The sleep-poll (`enable()`) vs. the cross-cycle step is *sequencing across time* — a few lines in the task — **not** a second copy of CiA402.

**Ownership still has to be arbitrated (unchanged from SineWave/Trajectory).** RT-safety of `writeValue` does not remove the coordination problem: the controlword is one output object = one last-writer-wins `outputSlot`, so if an active RT task and an HTTP `enable()` walk both write `0x6040` they race. When a trajectory/sine task is active it must be the **sole writer** of that device's controlword + setpoints, gated by the per-device control block (Sessions 2026-07-09 Trajectory, and the SineWave design). That is a separate, still-required handshake layered *on top of* the RT-safe accessor.

**Status.** Design agreed 2026-07-09; implementing in the next few days. Scope: the non-allocating `readPdo` overload + `readPdoFast`/`stageOutput` on `Device` + the fast-path branch in `readValue`/`writeValue`, with tests that a PDO read/write from a non-RT thread neither locks nor allocates, and that the SDO branch is unreachable while exchanging. No change to the borrowed-view model; `parametersMutex_` stays entirely off the RT path, so the recorder ring remains the single RT-written structure.

**The model in one line.** *Push RT-safety down into `Device::readValue`/`writeValue` — dispatched by bus-state, fast path in the typed template, health advisory, cache untouched — and the entire `Cia402Drive`/`SomanetDrive` view runs verbatim in both worlds; the only thing an RT task writes itself is the sequencing (step-per-cycle instead of sleep-poll), never the profile.*

## Session 2026-07-09 — Multi-axis coordinated trajectory, and "depth-1, latest-wins" is the message discipline for every RT control block (design)

**Extends** *Session 2026-07-09 — Trajectory playback* (which designed the single-axis, per-`Device` control block). Two questions came up that that note left open: (1) whether the control block should really be a shared latest-value slot or a proper **message-passing queue** to a specific task; (2) how **coordinated multi-axis** motion (a synchronised move across several drives — the interesting real case) changes the per-device decomposition. This session settles both: the block stays a **depth-1, latest-wins mailbox** (not a FIFO), and multi-axis is modelled as **one program with a column per axis**, not N synchronised per-device blocks.

**The reframe: the control block already *is* message passing — a mailbox of depth 1, not "shared state the RT task polls".** `active` + an atomic buffer pointer *is* a message; the launcher is the producer, `execute()` is the consumer. So "control block vs. message passing" is a false choice; the real axes are (a) **depth-1 latest-wins vs. FIFO queue** and (b) **channel owned by `DeviceManager`/`Device` vs. a handle to the specific `CyclicTask`**. Axis (b) is decided by an existing mandate: a launcher holding a handle to a concrete task instance would couple the view/HTTP layer to task instances and `GameLoop` membership, breaking "`mm::node` is transport- and scheduler-agnostic" and "the launch lives on the view, not the scheduler." So **every channel hangs off `DeviceManager`/`Device`**; the task drains it, the launcher writes it, and the two never name each other. That coupling verdict is identical for a slot or a queue — it is orthogonal to axis (a).

**Decision on axis (a): depth-1, latest-wins.** It is the correct semantics for setpoint generation, where the payload is *current intent that supersedes* — newest wins, a missed intermediate is harmless (the next read is current), memory is O(1) with no overflow path, and `active` doubles as start/stop (abort = publish `nullptr`/`active=false`). The clinching argument is lifetime on RT: a FIFO of trajectory buffers makes the RT consumer pop a `shared_ptr` and **drop its refcount — a potential `free()` on the RT thread** — which forces a reclaim thread or deferred-free list. The atomic-raw-pointer + retained-generations slot (Trajectory note; `ProcessData::image`) never frees on RT by construction, so depth-1 is not only sufficient, it is *simpler and safer* than a queue for a large payload consumed on RT.

**The tradeoff this accepts, stated out loud: no chaining / no sequencing.** "Run segment A, then B back-to-back" cannot be two writes — the second clobbers the first before RT observes it. If that requirement ever lands (blended segments, a queued move list, a one-shot event like touch-probe latch that must not be collapsed), it comes back as a **small bounded SPSC command queue added *alongside* the slot** — events in the queue, bulk setpoint data still in the latest-wins slot — never a redesign of the slot into a FIFO. Do not build the queue speculatively; today's features (play one move, run one sine) are pure latest-wins.

**Multi-axis is ONE program with a column per axis — not N per-device blocks.** Decomposing a coordinated move into independent per-`Device` control blocks re-creates three problems in the RT path that a single object avoids for free: **atomic start** (axis 1 must not begin on cycle *N* while axis 2 begins on *N+3* — separate blocks need a start barrier the RT side polls, fragile against a rescan or a mid-arm fault), **drift** (each block would carry its own cursor with nothing keeping them in lockstep), and **joint fault/completion** (a fault on one axis should quick-stop the whole group — with separate blocks "the group" is an emergent property you must reconstruct). Model the move as one immutable `TrajectoryRun` (participating axes + a **row-major setpoint matrix** + one shared cursor) and advance **one cursor once per cycle** applied to every column — synchrony is then true *by construction*, because there is only one clock.

```cpp
struct TrajectoryRun {                       // immutable once published; mlock'd off-RT
  std::vector<uint16_t> axes;                // participating slavePositions; disjoint across programs
  std::vector<int32_t>  setpoints;           // row-major: [cycle * axes.size() + axisIdx] → 0x607A
  size_t                cycles;              // == setpoints.size() / axes.size()
  cia402::OperationMode mode;                // e.g. CyclicSynchronousPosition
  enum class EndPolicy : uint8_t { HoldLast, Stop, Loop } endPolicy;
  uint64_t              generation;          // bumped per publish so RT detects a new program
};
```

**Ownership moves off `Device` onto `DeviceManager`.** A coordinated move spans devices, so its block cannot live on any single `Device` — it lives on `DeviceManager`, the same place as `ProcessData`, the tree's other cross-device RT/non-RT channel. `DeviceManager` holds a **fixed pool** of published program slots (fixed membership preserved: what varies is the *contents* and *which axes*, both runtime data — never the number of tasks or slots), each an `std::atomic<const TrajectoryRun*>` with a retained-generations keep-alive vector, exactly the wait-free transport the Trajectory note chose (**not** `std::atomic<std::shared_ptr<>>`, whose `load()` is not lock-free on libstdc++). Independent groups run concurrently — left leg (3 axes) and right arm (4 axes) as two programs with independent cursors — with the invariant that **a given axis appears in at most one active program** (disjointness enforced by the launcher, so two programs never both write one axis's controlword/setpoints).

**Launch graduates from a view to a coordinator — but still composes views, so CiA402 logic stays written once.** A `Cia402Drive` view binds one `Device&`, so single-axis launch stays on the view (`Cia402Drive::startTrajectory`, prior note). Multi-axis cannot bind one view, so it becomes a node-layer free function `startCoordinatedTrajectory(DeviceManager&, spec) -> expected<void,string>` that (1) resolves each axis and builds a per-axis `Cia402Drive` — *reusing the single-device op-mode + `enable()` handshake per axis, off-RT, so the RT side has no state-machine logic and no error branch*; (2) checks the requested axes are disjoint from every active program and claims a free pool slot; (3) builds the immutable `TrajectoryRun`, `mlock`s it, retains the `shared_ptr` in the slot's generations, and publishes with one `buffer.store(ptr.get(), release)` → `active.store(true, release)`. The HTTP handler just forwards and serialises the `expected<>` — it never sees the task or `GameLoop`. Single-axis is the `N=1` special case of the same coordinator (keep the thin `Cia402Drive::startTrajectory` for the common single-drive path).

**RT scratch is keyed by pool-slot index, not a pointer→cursor map.** The naïve `std::unordered_map<const TrajectoryRun*, size_t> cursor_` grows unboundedly and hashes every cycle (a heap op on RT). Instead the task holds a **preallocated array indexed by pool slot**, and detects a newly-published program by comparing the loaded pointer against the last-seen pointer for that slot — which gives free, correct cursor reset on every re-arm (the whole point of latest-wins):

```cpp
struct AxisGroupScratch { const TrajectoryRun* seen = nullptr; size_t cursor = 0; };
std::array<AxisGroupScratch, kMaxPrograms> scratch_;   // preallocated — no RT alloc, no hashing

// TrajectoryCyclicTask::execute(), per pool slot i:
auto* prog = dm_.motionPrograms()[i].load(std::memory_order_acquire);   // wait-free raw-pointer read
if (!prog || !prog->activeFlag(i)) continue;
if (prog != scratch_[i].seen) scratch_[i] = {prog, 0};                  // new program → fresh cursor
auto& cur = scratch_[i].cursor;
if (cur >= prog->cycles) { /* apply endPolicy; store completedGeneration for the off-RT watcher */ continue; }
const size_t base = cur * prog->axes.size();
for (size_t a = 0; a < prog->axes.size(); ++a) {                        // ONE cursor → all axes synchronous
  int32_t target = prog->setpoints[base + a];
  std::array<uint8_t, 4> le; std::memcpy(le.data(), &target, 4);
  dm_.processData().writePdo(prog->axes[a], 0x607A, 0, le);            // Design B lock-free output path
}
++cur;
```

**Everything else carries over unchanged** from the Trajectory / SineWave notes: one fixed-membership `TrajectoryCyclicTask` holding only `DeviceManager&`, registered in `main.cc` and a no-op until a program is active; per-cycle `findDevice()` re-resolution (never cache a `Device*`/view across a rescan); the lock-free `writePdo` output path; off-RT completion via a `completedGeneration` flip observed by a non-RT watcher (folded into the `MonitoringManager` sampler or the planned `NotificationBus` — RT does no I/O); `mlock`'d immutable buffers; and stop/rescan safety (a program must be stopped before a rescan, and the per-cycle `findDevice()` sits behind the same `scan()`/`reset()` drain the `ProcessData` path uses). **Ownership arbitration** is still required and is exactly what disjointness buys: within a program the coordinator is the sole writer of each axis's controlword + setpoints, and an active program gates out an HTTP `enable()` walk on the same device (same handshake as SineWave, layered on top of the RT-safe accessor from the RT-callable-view note).

**Outbound: notifying clients about execution — the same discipline, reversed.** An RT task must **never** call `WebSocketServer` directly: it runs its own uWS loop/thread (port 62281), a publish allocates and can block, and uWS is only safely callable from its own loop (via `loop->defer(...)`) — all three forbidden on RT. The `CyclicTask` contract already mandates the fix: *"Any async work (e.g. pushing data to a WebSocket) must be handed off to another thread via a lock-free channel."* So RT stores a **raw signal** — scalars only, no `std::format`/json/string — into a lock-free structure and moves on; an **off-RT watcher** drains it, builds the JSON, and calls `WebSocketServer::publish` (which itself `defer`s onto the uWS loop). The watcher is the reverse-direction analogue of the launcher — folded into the `MonitoringManager` sampler thread or the planned `NotificationBus`, never a handle from the task to the server. Three tiers, chosen by event semantics — the same depth-1-vs-FIFO fork as the inbound side, mirrored:

1. **Lifecycle status (started / progress / done / faulted / aborted) → depth-1 latest-wins atomics on the control block.** RT flips `progress` / `status` / a bumped `statusGen`; the watcher polls and emits one notification *on change*. A dropped intermediate is harmless — newest is current. This is the inbound mailbox running RT→off-RT; the same `TrajectoryRun` struct carries both directions (different fields, different writers).
2. **Per-cycle telemetry ("what is the drive doing right now") → not a new channel at all; it is already monitoring.** The trajectory's setpoints/actuals land in the recorder ring every cycle and `MonitoringManager` already streams every recorded cycle losslessly. A live plot of the move is a *monitoring subscription* on those objects — the task sends nothing.
3. **Must-not-drop discrete events (each waypoint reached, distinct fault codes) → an SPSC ring the RT task produces into.** The one case that warrants a queue: each event is a distinct occurrence that latest-wins would collapse. RT pushes fixed-size records (event id, cycle, axis, code — scalars), the watcher drains and emits one message per record. Mirror of the "chaining needs a queue" escape hatch, on the outbound side; only when latest-wins genuinely loses information.

End-to-end for tier 1 — RT stores scalars, the off-RT watcher does all formatting and the publish:

```cpp
struct TrajectoryRun {                    // ... axes, setpoints, cycles, generation as above ...
  std::atomic<size_t>   progress{0};      // RT: current cycle index          (latest-wins)
  std::atomic<uint32_t> status{0};        // RT: 0=running 1=done 2=faulted    (latest-wins)
  std::atomic<uint64_t> statusGen{0};     // RT: bumped on each status change so the watcher wakes
};

// RT — TrajectoryCyclicTask::execute(), per active program: store scalars only, NEVER publish.
prog->progress.store(cur, std::memory_order_relaxed);
if (faulted)                      { prog->status.store(2, relaxed); prog->statusGen.fetch_add(1, release); }
else if (cur + 1 >= prog->cycles) { prog->status.store(1, relaxed); prog->statusGen.fetch_add(1, release); }

// OFF-RT — watcher on MonitoringManager's thread (or a small poller). ALL json + publish happens here.
void TrajectoryWatcher::poll() {                        // ~every 20 ms, off-RT
  for (size_t i = 0; i < pool_.size(); ++i) {
    const auto* prog = pool_[i].load(std::memory_order_acquire);
    if (!prog) continue;
    uint64_t gen = prog->statusGen.load(std::memory_order_acquire);
    if (gen == lastSeenGen_[i]) continue;               // unchanged → no message
    lastSeenGen_[i] = gen;
    publish_("trajectory", nlohmann::json{               // built OFF-RT; publish_ == WebSocketServer::publish
      {"type", "notification"},
      {"data", {{"event", statusName(prog->status.load(relaxed))},
                {"progress", prog->progress.load(relaxed)}, {"cycles", prog->cycles}}}});
  }
}
```

**Generalizing both channels — one shape per direction, not one class per task.** The bespoke `TrajectoryWatcher` above was an *illustration*; you do not write a watcher (or a launcher) per RT task. Each direction has exactly two task-specific functions and everything else is shared plumbing — but the two directions are **not** symmetric in one important way: outbound needs a bridging thread (RT can't call out, so something must poll and publish), inbound needs none (the HTTP thread writes the atomic mailbox directly and returns).

*Outbound (RT → client): one `NotificationBus`, a registry of sources.* The thread, the poll loop, the change-dedup, and the single `WebSocketServer::publish` callback are written once; a feature registers only a **change token** (read an RT-bumped atomic) and a **render** (build the JSON, off-RT):

```cpp
class NotificationBus {                       // one off-RT thread; the SINGLE publisher to the WebSocket
 public:
  struct Source {
    std::string                                    topic;
    std::function<uint64_t()>                      changeToken;  // cheap: read RT-bumped atomic(s)
    std::function<std::optional<nlohmann::json>()> render;       // off-RT: build the message
  };
  void addSource(Source s);                   // called once per feature at startup
  // run(): every ~20 ms, for each source: if changeToken() != lastSeen[i], publish_(topic, *render())
};
```

All three outbound tiers fit this one interface: tier-1 lifecycle status is `changeToken = statusGen`, `render = snapshot`; a tier-3 must-not-drop event stream is `changeToken = ring.writeCount()`, `render = drain-the-ring`. Same thread, same loop — only the two lambdas differ. Tier-2 per-cycle telemetry deliberately does **not** use the bus: it stays the high-rate lossless `MonitoringManager` stream.

**The first source to fold in already exists as a standalone thread: `BusHealthReporter`** (`apps/motion_master/bus_health_reporter.{h,cc}`, 2026-08-16). It is this design in miniature — one off-RT thread, a change token (`ProcessImageInfo::shortWkcCycles`, an RT-bumped atomic), a render (format and `spdlog::warn`), and a last-seen mark so one fault is reported once. Folding it in is deleting the class and registering `Source{changeToken = shortWkcCycles, render = the same message}`; it publishes to the log rather than a topic today, which the bus's `publish_` callback subsumes.

Two things it settled that are worth keeping. **The counters must not be per-image**: they were originally cleared on every re-map, and a re-map happens whenever a device is brought into or out of SAFE-OP/OP — precisely when someone is chasing a fault — so the record was erased at the moment it was most wanted. They now run `init` → `reset` and are cleared nowhere else, which also means the bus may poll them at any cadence without racing a teardown. **And reporting must not hang off control-plane boundaries**: the first version called a `DeviceManager::reportShortWkc` from the four moments that end a stretch of exchanging (state change, re-map, reset, rescan), which failed twice over — four call sites is a rule you can forget (the rescan one *was* forgotten, and only surfaced on a third review), and a boundary reports whenever the user next touches something rather than when the fault happened, so an unattended bus said nothing at all. That method is gone; the reporting logic, including the last-seen mark, belongs to whoever reports, not to `DeviceManager`.

*Inbound (client → RT): no thread — the reusable pieces are the transport, the pool, and a launch skeleton.* Three write-once pieces replace the per-task control-block boilerplate:

```cpp
template <typename T>                         // T = immutable payload (TrajectoryRun, …)
class RtMailbox {                             // the depth-1 latest-wins slot itself
 public:
  const T* current() const {                  // RT: wait-free, alloc-free; nullptr when idle
    return active_.load(acquire) ? buffer_.load(acquire) : nullptr;
  }
  void arm(std::shared_ptr<const T> next) {   // off-RT: retain (RT never frees) + atomic publish
    generations_.push_back(next); buffer_.store(next.get(), release); active_.store(true, release);
  }
  void disarm();  void reset();               // abort/stop; scan()/reset() once RT is drained
 private:
  std::atomic<const T*> buffer_{nullptr}; std::atomic<bool> active_{false};
  std::vector<std::shared_ptr<const T>> generations_;   // control-plane keep-alive
};                                            // (specializes to SeqLock<T> transport for trivial scalar T)

template <typename T> class RtMailboxPool {   // fixed pool + the "an entity in ≤1 active slot" rule
 public:
  RtMailbox<T>& operator[](size_t i);  size_t size() const;      // RT iterates these
  std::expected<RtMailbox<T>*, std::string> claim(std::span<const uint16_t> entities);  // disjointness + free slot
};

template <typename T, typename Spec>          // the launch skeleton — the two hooks are all a task writes
std::expected<void, std::string> launchRtTask(
    RtMailboxPool<T>& pool, const Spec& spec,
    auto&& validateAndHandshake,              // per-task: op-mode/enable/limits — synchronous, OFF-RT
    auto&& buildPayload) {                    // per-task: -> shared_ptr<const T> (immutable, mlock'd)
  if (auto r = validateAndHandshake(); !r) return r;             // bail leaves RT untouched
  auto box = pool.claim(spec.entities);       if (!box) return std::unexpected(box.error());
  (*box)->arm(buildPayload());                return {};
}
```

So the coordinated-trajectory launcher collapses to its two genuinely task-specific parts, mirroring the `Source`'s two:

```cpp
std::expected<void, std::string> startCoordinatedTrajectory(DeviceManager& dm, const CoordinatedSpec& s) {
  return launchRtTask(dm.trajectoryPool(), s,
      [&]{ return handshakeAllAxes(dm, s); },   // resolve per-axis views, op-mode + enable each
      [&]{ return buildTrajectoryRun(s); });    // matrix → immutable, mlock'd TrajectoryRun
}
```

`DeviceManager` owns one `RtMailboxPool<T>` per task family (`trajectoryPool()`, `sinePool()`); the RT task iterates `pool[i].current()` each cycle. Both directions use **composition, not inheritance** — lambdas/policies, no single-impl interface (repo rule). And **do not** generalize the HTTP *surface* into a command-bus routing a generic `POST /api/rt-tasks {type}`: routes stay explicit per `swagger.yml` and the plug-in design; only the plumbing *beneath* the handlers is generalized.

| | Outbound (RT → client) | Inbound (client → RT) |
| --- | --- | --- |
| Generalized as | `NotificationBus` + `Source{changeToken, render}` | `RtMailboxPool<T>` + `launchRtTask(validate, build)` |
| Per-task surface | two functions | two functions |
| Owns a thread? | **yes** — RT can't call out, a pump polls | **no** — HTTP thread writes the atomic directly |
| Written once | thread, loop, dedup, publish | mailbox atomics, pool, disjointness, arm/keep-alive |

**The model in one line.** *Every RT control block is a depth-1 latest-wins mailbox on `DeviceManager`/`Device` (never a queue, never a handle to the task) — a coordinated multi-axis move is one such mailbox holding a single immutable program with a column per axis and one shared cursor, launched by a coordinator that composes per-axis views off-RT; synchrony is by construction because there is exactly one cursor, chaining (if ever needed) is a command queue added beside the slot rather than a redesign of it, and client notifications run the same mailbox in reverse — RT stores a raw status scalar, an off-RT watcher formats and publishes it, so the RT thread never touches the WebSocket. Both directions generalize to two task-specific functions over shared plumbing — outbound a `NotificationBus` + `Source`, inbound an `RtMailboxPool` + `launchRtTask` — the only asymmetry being that outbound owns a bridging thread and inbound does not.*

## Session 2026-07-11 — How the WebSocket subscribe/unsubscribe + monitoring stream works today (as-built)

Clarification session, no code change. Records the runtime shape of the live monitoring path exactly as built, and pins down two things that are easy to get wrong: **why subscribe/unsubscribe is on the WebSocket and not HTTP**, and the **create-vs-subscribe split** that decides which client sees which stream. Cross-references *Session 2026-06-06 — HTTP and WebSocket on separate ports/loops*.

**"Topic" is a uWebSockets application-level feature, not part of the WebSocket standard.** RFC 6455 gives exactly one thing: a single TCP connection (upgraded from HTTP via `101 Switching Protocols`) carrying discrete messages bidirectionally between *one* client and the server. No topics, channels, rooms, or broadcast — each connection is its own island and the protocol doesn't know other clients exist. uWS layers **pub/sub** on top (MQTT-borrowed vocabulary + wildcard syntax): a server-side **topic tree** mapping topic strings → the set of subscribed sockets. Three ops: `ws->subscribe(topic)` (adds *that socket* to the set — per-connection state stored on the socket), `ws->unsubscribe(topic)`, and `app->publish(topic, msg)` (sends to every socket in the set, one call). It is in-memory and per-process; publishing to a topic with zero subscribers is a silent no-op. All three appear in `ws_server.cc` (`message` handler ~L88 does `ws->subscribe`/`unsubscribe`; `publish()` at L60 does `app->publish`).

**Why subscribe/unsubscribe lives on the WS, not on an HTTP route.** Not arbitrary — it falls out of the uWS model:

- **A subscription is per-connection state, and the WS socket *is* the connection.** `subscribe` is a method *on the socket* (`ws->subscribe`); there is no `ws` handle on the HTTP side. Delivering (`app->publish`) is keyed on that same per-socket subscriber set. HTTP would force a parallel topic→connection registry plus an invented session/correlation token to link a stateless HTTP request back to a specific WS socket.
- **Automatic teardown on disconnect.** Because the subscription lives on the socket, uWS drops it from every topic on close — the `close` handler (`ws_server.cc:81`) has nothing to undo. HTTP-side subscribe would orphan subscriptions the moment a WS connection died and need reconciliation.
- **No cross-loop hop.** `HttpServer` (61447) and `WebSocketServer` (62281) run on separate loops/threads. The `message` handler runs on the very loop that owns the socket, so `ws->subscribe()` mutates socket state with zero locking. An HTTP subscribe would have to `defer()` the mutation onto the WS loop (the way `publish()` already must, since the sampler thread calls it) *and* first resolve which socket.
- The only case that would push the registry off the socket is **durable subscriptions surviving reconnects** — not a requirement here, and the roadmap keeps adding *inbound WS* commands (output staging), reinforcing the WS as the bidirectional control channel.

**Two decoupled layers — the crux of "will another client see it?".** The word "topic" spans two separate concepts:

- *Layer 1 — the Monitoring (server-side, global, created over HTTP).* `POST /api/monitorings` (body carries `topic` id + parameter list; `monitoring_api.cc:55`) creates a shared server object validated + owned by `MonitoringManager`. Its sampler thread reads the recorder ring and calls the publish callback for that topic **on its flush cadence regardless of who is listening**. It is global — visible to anyone via `GET /api/monitorings`.
- *Layer 2 — the Subscription (per-connection, over WS).* Whether *your* connection *receives* those batches depends on whether *you* sent `{"subscribe":"<topic>"}` on *your* socket.

So, directly: when client A creates monitoring `left-leg`, the monitoring exists and the sampler publishes to it — but client B does **not** auto-receive it. B must `subscribe("left-leg")` on its own WS connection (any number of clients can, up to the ~5-client cap — fan-out, not exclusive ownership). And **creating ≠ subscribing**: even A must subscribe on its own socket to see the data it just created. Mental model: *the Monitoring is a radio station the server broadcasts on a frequency; `subscribe` is each client tuning its own receiver to it.* Creating the station tunes in nobody; tuning in is per-receiver.

**`"pdos"` was a reserved name for a whole-image stream — decision this session: that stream will not exist, and the reservation is removed.** Until now `monitoring_manager.cc` defined `constexpr char kReservedTopic[] = "pdos"` and `create()` rejected it (`"topic 'pdos' is reserved"`); the header, `swagger.yml`, and a unit test documented/asserted it. There was **no `publish("pdos", ...)` anywhere** — the producer side was never built. It had been held for a *planned* built-in, always-on high-frequency stream of the **entire process image** every cycle (a zero-setup "show me everything on the bus" firehose), distinct from user-created monitorings.

**Rejected.** For a real bus (30+ devices) the whole image is orders of magnitude bigger than a curated monitoring: a monitoring row is ~450 bytes (~40 chosen 32-bit values, the throughput budget in *Session 2026-05-16*), whereas a whole-image stream is every device's full input+output IOmap, every recorded cycle, losslessly, to each of up to ~5 subscribed clients — a continuous multi-hundred-KB/s-per-client firehose that the browser then decodes and mostly discards. The only thing it bought over a monitoring was "don't pick parameters" — the one thing the client is best placed to do cheaply. And the two genuine needs are already served by better-fitting mechanisms: **a few live traces** → a targeted monitoring (already lossless per-cycle); **a full capture** → the on-demand `.mmpd` dump (`POST /api/process-data/dump`, bounded file, not a forever-stream to five browsers). So dropping `"pdos"` leaves no gap. **Removed** `kReservedTopic` + the `create()` check + the header/`swagger.yml`/test references, so `"pdos"` is now just an ordinary available topic name with no special meaning. Rule going forward: **clients create a monitoring for the parameters they need; there is no built-in whole-image stream.**

**Aside — the "up to 5 clients" figure is a design budget, not an enforced cap.** It appears in the docs (`CLAUDE.md`, this file's *Session 2026-05-16* throughput estimate of ~2.25 MB/s) but **nothing in code limits connections** — `WebSocketServer::connections_` is just a set for `broadcast()`; a 6th client is accepted like any other. The number is the assumed concurrent-client load used to size the loopback throughput estimate (5 × ~450 B/ms ≈ 2.25 MB/s, negligible on loopback), and it was one of the inputs to rejecting `"pdos"` above (a firehose × 5 clients is the load that actually hurts). If a hard cap is ever wanted it would be an explicit reject in the `open` handler; today there is none.

## Session 2026-07-13 — `CyclicTimer` overrun policy: skip-to-grid, not catch-up; and the three platforms unified

The absolute-deadline `CyclicTimer` (drift-free by construction — each cycle sleeps to a fixed grid point `T0 + n·period`, so a late wake never shifts the next deadline) had an unexamined behaviour on the *overrun* path. When a deadline is already in the past — a genuine overrun (cycle work > period) or a multi-cycle scheduling stall (the non-RT scheduler simply didn't run the thread for several periods) — `clock_nanosleep(TIMER_ABSTIME)` returns immediately, so the loop was running every missed cycle **back-to-back with no sleep** until the clock caught up to the grid. That is the correct behaviour for an *accounting* timer (it preserves exact cycle count) and the wrong one for an *I/O* timer.

**Why catch-up is wrong for EtherCAT — the frames it emits are stale and harmful.** (1) In CSP/CSV each PDO frame carries a setpoint meant for *this* cycle's point on the timeline; a burst of catch-up frames shoves N setpoints computed for already-elapsed moments, which is a jerk on the axis, not a replay of lost time. (2) Under DC SYNC0 the slave latches process data at the SYNC0 pulse, not on frame arrival — so a burst between two pulses just overwrites the SM buffer N-1 times and delivers the *same* single latched value, having burned RT budget during an already-stressed moment and raised the odds the *next* cycle also slips. (3) The missed real-world time can't be un-missed; catching up the master's cycle *count* buys nothing physical. This is what production masters (TwinCAT, IgH) do too: on a blown cycle they skip and bump an "exceed" counter, they don't emit the backlog.

**Decision: skip-to-grid.** `waitForNextCycle()` now, after advancing the deadline by one period, checks whether that deadline is already behind `now`; if so it fast-forwards over the whole backlog (`while (next < now) { next += period; ++skipped; }`) to the next *future* grid point and sleeps to that, returning the skipped count. Two properties matter: (a) **at most one cycle runs per period** — the backlog is dropped, never bursted; (b) **phase is preserved** — because we advance along the original `T0`-based grid rather than re-anchoring to `now`, the frames that *do* go out stay aligned with the SYNC0 timing the drives expect (skip-*to-grid*, deliberately not re-anchor-to-now). Ordinary jitter is untouched: a late wake whose work still fits leaves the next deadline in the *future*, so the `while` never runs and `skipped == 0` — the drift-free absorption in the next cycle's slack is exactly as before. Two caveats that reinforce the choice: the slave's PDO/SM watchdog sets the ceiling where policy matters at all (a stall longer than the watchdog drops the slave to SAFE-OP regardless, and we re-map — see the AL-transition/partial-bus notes); and a single dropped CSP setpoint is recoverable (the drive interpolates/holds one cycle) whereas a stale-setpoint jerk is not.

**Sustained overrun on a coarse-timer machine — the common Windows case, and why skip-to-grid matters most here.** The above frames overruns as *occasional*, but the case that will actually dominate is *steady-state* under-delivery: many Windows hosts have an effective timer granularity of ~1.5 ms, so wakes land ~1.5 ms apart no matter what deadline is requested, and a configured 1 ms period is simply unachievable. This is not an edge case for us — most commissioning/inspection users run Windows userspace. Trace the steady state (executes at ~T0, +1.5, +3.0, +4.5, +6.0 ms against a 1 ms grid): each `waitForNextCycle()` does `next += 1ms` then skips the grid points now behind `now`, so it settles into *execute ~every 1.5 ms, skip one grid slot roughly every other cycle*. The numbers: **executed ≈ 667 Hz** (1 per 1.5 ms), **skipped ≈ 333/s** (grid runs at 1000/s, so ~1 skip per 2 executed cycles), and `skippedCycles()` climbs steadily at ~333/s. Crucially it **degrades gracefully** — the three things a naive design would do and this does not: (1) *no burst* — every executed cycle sleeps to a real future grid point, frames go out at ~667 Hz phase-aligned to the 1 ms grid, never in catch-up bursts; (2) *no drift* — the grid stays anchored at `T0`, the frames that go out are on true 1 ms boundaries; (3) *no runaway* — per-cycle `skipped` stays ~0–1, the backlog never grows. Per consumer: `elapsed` still advances at the true 1 ms-grid rate (it is `executed + skipped`, and skipped fills the gap), so a *Real-time* trajectory stays on wall-clock schedule and merely drops ~1/3 of its points (small jumps), while a *Sequential* trajectory advances one point per executed cycle and stretches (a 1 s move plays in ~1.5 s, smoothly). For EtherCAT/DC the drives get a fresh frame in only ~2 of every 3 SYNC0 windows — fine only if the PDO/SM watchdog and interpolation tolerance allow it. **The remedy is operational, not code**: a `skippedCycles()` that climbs steadily right after start is the signal that the configured period is too aggressive for that hardware — raise it (e.g. to 2 ms) so the loop actually meets its grid. Skip-to-grid cannot conjure timer resolution the OS does not provide; its whole value on these machines is that it fails *visibly and honestly* (runs at the achievable rate, phase-locked, with a monotonic skip counter) instead of drifting, bursting, or spiralling. This is the strongest argument for the policy — the machines most likely to overrun are exactly the ones most of our users are on.

**Surfacing is deferred, but the sensor is built now.** `waitForNextCycle()` returns `uint64_t skipped` (not `[[nodiscard]]`, so `jitter_bench` ignores it freely); `GameLoop` accumulates it into a silent `skippedCycles()` counter (relaxed atomic, sibling to `executedCycles()`) — **no logging, no notification on the RT path** (a non-RT dev box skips constantly; unthrottled logging would be pure noise). (Naming: the count is called *skipped* everywhere — the timer's neutral, cause-agnostic term for "deadlines the timer jumped over"; "overrun"/"stall" are reserved for prose naming a *cause*, never an identifier. The earlier `overruns()` accessor was renamed `skippedCycles()`.) The counter is the series the **planned master-side frame/WKC health timeline** (Session 2026-06-01) will sample — an intermittent RT stall is precisely the kind of fault a point-in-time reading misses. Building the counter now avoids retrofitting the hot path when that timeline lands.

**Unified the three platform implementations on one model while here.** Linux was the reference (absolute grid, `EINTR` retry). macOS already mirrored the `next_sec_/next_nsec_` bookkeeping via `mach_wait_until`; it gained the skip-to-grid loop **and** a `KERN_ABORTED` retry loop it had been missing (the header promised interrupt-retry; the old code didn't honour it). Windows was the outlier — a *periodic* `SetWaitableTimer` (`lPeriod` in ms) that couldn't report a skip count and diverged structurally from the absolute-deadline mental model. It was **rewritten** to the shared model: a `QueryPerformanceCounter` (monotonic) grid, a one-shot HIGH_RESOLUTION timer armed each cycle with a *relative* due time computed as `next − now` (relative avoids the wall-clock/FILETIME dependency an absolute due time would carry, and QPC keeps the grid monotonic). `advanceOnePeriod()` — the one-period advance with ns-carry normalisation — is now a single inline header helper shared by the Linux and macOS TUs (each platform compiles exactly one `cyclic_timer_*.cc`, so it's ODR-safe). Windows remains explicitly a dev target, not hard-RT. Tests: `libs/core/tests/cyclic_timer_test.cc` covers anti-drift (N cycles can't finish in < N periods), skip engagement (forced multi-period stall → single `waitForNextCycle()` returns the backlog count, robust lower bound), and re-sync (post-stall cycles sleep normally again), with tolerances loose enough for a non-RT CI runner — no per-cycle `skipped == 0` assertion, since a loaded runner can legitimately miss an undelayed 2 ms deadline.

**Is the Windows rewrite "better"? Yes on correctness/observability, no on raw timing — and it's worth being precise so nobody later "optimises" it expecting a speedup.** (1) **Same wake primitive → no jitter/latency win.** Both old and new arm a `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` timer, so the kernel mechanism that actually wakes the thread (~0.5 ms, sub-ms on Win10 1803+) is identical; single-wake jitter is unchanged. If anything the new version does marginally *more* work — one `SetWaitableTimer` syscall per cycle to re-arm the one-shot vs. the old arm-once periodic timer (~1 µs at 1 kHz ≈ 0.1% overhead, immaterial). (2) **The real correctness win is sub-/non-integer-ms periods.** The old periodic timer set its cadence via `lPeriod`, an **integer-millisecond** field (`period.count() / 1000`): a 1500 µs period truncated to 1 ms (silently wrong cadence), and a 500 µs period truncated to `lPeriod = 0`, which means *one-shot* — it fired once and never repeated (broken). The QPC-tick grid (`periodTicks_ = µs × qpcFreq_ / 1e6`, ~10 ticks/µs at a typical 10 MHz QPC) represents any period accurately. The default 1 ms path was fine before; anything else was latently wrong. (3) **The catch-up→skip behaviour change barely moved Windows.** A Windows auto-reset periodic waitable timer already *coalesced* missed periods — signalled-is-a-boolean-not-a-count, so one `WaitForSingleObject` consumed the whole backlog and the next blocked to the following period, behaviourally close to skip-to-grid already, just **silent** and phased off the periodic timer's own schedule. So the dramatic burst→skip fix mattered on *Linux*; on Windows the gains are that the skip is now **counted** (`skippedCycles()`) and the phase is a precise monotonic QPC grid. (4) **Ceiling unchanged:** stock Windows determinism is scheduler-bound, so no timer rewrite makes it hard-RT — the RT story stays Linux + `SCHED_FIFO` + `PREEMPT_RT`.

## Session 2026-07-13 — Skips are normal here, not a fault; user-chosen skip policy; one TrajectoryCyclicTask absorbs SineWave

Follow-on to the same-day `CyclicTimer` skip-to-grid work, pushing the overrun question up from the *timer* (which frame) into the *task* (which setpoint), and re-calibrating it for the actual deployment. **Supersedes the separate-`SineWaveTask` design in Sessions 2026-06-05 and 2026-07-09** (the fixed-membership / view-launched / per-`Device`-slot machinery all stands; only the "SineWave is its own task with a `SeqLock<SineWaveParams>` transport" part is retired).

**Recalibration: skips are routine on the platform this software actually runs on.** The earlier note flirted with treating a mid-trajectory skip as a fault ("the RT contract broke while commanding a live axis"). That is right *only* for the PREEMPT_RT production minority. The typical user runs Motion Master on **Windows** (and macOS) userspace for commissioning and inspecting motion — no `SCHED_FIFO`, no `mlockall`, so the scheduler *will* deschedule the loop regularly and skips are **inevitable and fine**. So the task-level posture is: absorb skips gracefully by default, and give the user the choice of *how* — not cry wolf. (A strict fault-on-skip mode can exist as an opt-in for the RT crowd, but it is never the default.)

**The scheduler owns the fact; the task owns the policy.** `GameLoop`/timer can only report "N cycles were skipped; real time advanced by 1+N periods since your last `execute()`." It must not bake in a policy, because the right response depends on what the task's content *means*: `ProcessDataCyclicTask` ignores skips entirely (it sends the freshest I/O — nothing time-indexed to catch up, which is also why timer-level skip-to-grid was right: exchange once per real cycle, never burst); a target generator has to decide.

**The user-facing choice is shape-vs-timing, exposed per trajectory.** On a skip a playback task has two honest options, and neither is universally correct, so it is a launch parameter the user picks (default **Sequential**):

- **Sequential** (default — "command every target"): advance the cursor by **1**, ignore the skip. Preserves the **shape** — every setpoint is sent, motion stays smooth, no jumps — at the cost of **timing**: a 10 s move may take 10.3 s under jitter, and coordinated multi-axis can desync. This is what commissioning/tuning wants (see every point, smooth), and — importantly — it needs **nothing** from `GameLoop`: the task just increments its own cursor per `execute()` and never looks at the skip count. So the default path is dead simple.
- **Real-time** ("stay on schedule"): advance the cursor by **1+skipped** (i.e. `cursor = ctx.elapsed - startCycle`). Preserves the **timing** — wall-clock position honoured, waveform frequency stays accurate, multi-axis stays coordinated — at the cost of **continuity**: the setpoint jumps at resume (a velocity/accel spike, possible following-error). A **clamp** on the per-cycle setpoint delta (cap to a safe velocity) is a safety net inside this mode, not a third mode.

**Plumbing: only Real-time needs it — but built now, ahead of its consumer.** Because Sequential needs no timing context, the only *behavioural* reason to change `CyclicTask::execute()` is Real-time. The plumbing was nonetheless landed now (2026-07-13) — it is far cheaper to change the one-line `execute()` signature and its single implementor (`ProcessDataCyclicTask`) while they're trivial than to retrofit it once several tasks exist. `execute()` now takes a small immutable `CycleContext { uint64_t elapsed; uint64_t skipped; }` (defined in `cyclic_task.h`; fields are bare — the struct name supplies the "cycles" unit, matching sibling `skipped`), pushed by `GameLoop` (push, not a pull of `GameLoop::skippedCycles()` — the scheduler telling tasks about time is the scheduler's job). `ProcessDataCyclicTask` ignores it; no other task exists yet. `elapsed` is the primary field — total cycles elapsed since `run()`, jumps by 1+skipped on a stall, computed in `run()` as `executedCycles() + skippedCycles()`. A time-parameterised task computes its state as a **pure function of `elapsed`** (`cursor = ctx.elapsed - startCycle`), the *same* robustness principle as the absolute-deadline timer — no per-cycle accumulation, skips handled by the count simply jumping. `skipped` rides alongside only for anomaly policy (e.g. the opt-in strict mode). `skipped` rides alongside only for anomaly policy (e.g. the opt-in strict mode).

**One task, not two: SineWave collapses into `TrajectoryCyclicTask` + a `repeat` flag.** A sine is just "play back one period of samples, looping." Motion Master generates the sample buffer in **one testable userspace function** and hands it to the same trajectory mechanism over the API; `repeat` loops it. This deletes `SineWaveTask`, the `SineWaveParams` struct, and — the real win — the whole **`SeqLock<SineWaveParams>` transport**: everything now goes through the trajectory's atomic-pointer + retained-generations publish (the *only* wait-free reader path in the tree, and already required because a buffer is a `vector`, not scalars). It fits the standing principle exactly — **dumb RT, waveform math in userspace**: no `sin()` on the RT thread, the RT task stays a buffer-reader + cursor, and *any* waveform (sine, chirp, step, ramp, replay, arbitrary offline signal) is the same code. No precision loss — a buffer sampled at the cycle rate is identical to computing the sine per-cycle. With `repeat` you store **one period**, so memory is tiny (1 Hz sine at 1 kHz = 1000 pts = 4 KB), bounded by the config max-length already specified. This retires the SineWave-specific RT scratch (`phase_`/`center_`/`wasActive_`) too — a repeating buffer has only a cursor.

**The one subtlety `repeat` introduces: the loop seam.** If the waveform period isn't an integer number of cycles (3 Hz at 1 kHz = 333.33 cycles/period), a one-period buffer won't wrap cleanly — a small phase discontinuity every period. Two clean fixes: (1) **quantise the requested frequency** to the nearest value that divides evenly into the cycle rate (3 Hz → 3.003 Hz — imperceptible for commissioning, simplest); (2) **store a whole number of periods** (the smallest buffer that is an integer count of both cycles and waveform periods — an LCM — loops seamlessly at any frequency, bigger buffer). Default to (1), expose (2) only if an exact odd frequency is needed. The seam interacts with the skip modes consistently: Sequential+repeat dips the frequency slightly during a stall (shape preserved); Real-time+repeat is `cursor = ctx.elapsed % bufferLen` (frequency exact, jumps).

## Session 2026-07-14 — `GET /api/game-loop` RT-health endpoint + console page; "task time" is blocking wire I/O, not compute

Surfaced the game-loop's real-time health end-to-end so a user — especially on the non-RT Windows/macOS majority — can see whether the loop is meeting its period. `GameLoop::health() → GameLoopHealth` (configured period + `targetHz = 1e6/periodUs`, cumulative `achievedHz` from a monotonic uptime captured in `run()`, executed/skipped counters, per-cycle task-execution time last/max/avg ns, and `schedFifo`/`memLocked` RT flags — `setRealtimePriority()` now returns an `RtSetupResult` reporting which of the two best-effort steps succeeded). Served at `GET /api/game-loop` via an `HttpServer::Config` callback wired in `main.cc` (same composition-root pattern as `GetLogFn` — the server never references `GameLoop`; the loop is constructed before the servers so the callback can borrow it). Raw counters + a server `timestampUs` let the client diff successive polls for the live instantaneous rate. Console **Game Loop** page (Server sidebar group, gated on `online` only — the loop runs unconditionally, no scan needed) polls every 1 s, renders metric tiles + skip/RT-status callouts, and computes a "Live rate" by diffing `Δexecuted / Δtimestamp` (the cumulative `achievedHz` is too damped to show a transient stall); a collapsible `GameLoopExplainer` documents each card's formula.

**The non-obvious operational fact this exposed: per-cycle "task time" is dominated by blocking network I/O, not process-data composition.** `health()` measures task time by bracketing the `CyclicTask::execute()` loop with two `steady_clock` reads. That loop is almost entirely `SoemFieldbusDriver::exchangeProcessData()`, whose two IOmap memcpys are a few µs — but between them sit `ecx_send_processdata()` (a `sendto` syscall) and `ecx_receive_processdata(ctx, EC_TIMEOUTRET)`, which **blocks** until the EtherCAT frame has circulated through the whole daisy-chain and returned. So the measured window is syscall + NIC hardware/driver latency + wire round-trip + scheduler wakeup; the wire portion for a few drives is tiny and the OS/NIC path dominates. Concretely: the endpoint's first verification on an RT-tuned host read **~4 µs** worst-case, while the same build on a commodity laptop NIC reads **~100–300 µs** — a ~100× spread that is entirely the NIC/kernel, not the code. On a laptop that figure is **normal and harmless**: it is *budget consumed* (the core is parked on the wire, not spinning), so a 1 ms grid still runs at ~999 Hz with 0 skips. The real-fault signature is `maxExecNs` approaching `EC_TIMEOUTRET` (2000 µs default) with `skippedCycles` climbing — a lost/late frame stretching one cycle past its deadline.

**Tuning knobs to reduce it (only worth it if `max`/skips misbehave — not needed when it fits the budget):** a PREEMPT_RT kernel; an Intel server NIC (`igb`/`igc`/`e1000e`) over a laptop/USB adapter; **disabling interrupt coalescing on the EtherCAT NIC — `ethtool -C <iface> rx-usecs 0 tx-usecs 0`, often the single biggest win on commodity hardware**; and CPU isolation (`isolcpus`/`nohz_full`) for the RT thread. Captured in CLAUDE.md (Game Loop / RT Threading) so it isn't re-derived from a puzzled "why is a PDO exchange 300 µs?" every time.

## Session 2026-07-14 — Runtime cycle-period control (`PUT /api/game-loop`); and the recorder ring goes period-independent (fixed `capacity`)

The health endpoint above answers "is the loop meeting its period?"; this session added the **operational lever** to fix it when it isn't. `PUT /api/game-loop {"periodUs": N}` retimes the *running* RT loop. `GameLoop::period_` became a `std::atomic<std::chrono::microseconds>` (8-byte, lock-free) that the loop reloads once per iteration; on a change it calls the new `CyclicTimer::setPeriod()`, which recomputes the period and **re-anchors the deadline grid to now** on all three platforms (Linux `clock_gettime`, macOS `nowNs()` off the mach clock, Windows the QPC baseline). Re-anchoring is the subtle part: without it a longer period against a baseline in the past would fast-forward and report a pile of phantom skips, and a shorter one would strand the deadline far in the future — so the change is verified by two new timer tests (`SetPeriodChangesCadence`, `SetPeriodDoesNotBurstSkips`). The change is **transient** (not written back to config; a restart reverts to `gameLoop.periodUs`) and the server never references `GameLoop`/`DeviceManager` — a `SetGameLoopPeriodFn` callback in `main.cc` validates `periodUs != 0` (same rule as config) and does the retune, exactly like `GetGameLoopHealthFn`. The operational point (in CLAUDE.md and the swagger/UI copy): on a host whose `skippedCycles` climbs steadily, raise the period until the loop meets its grid — the timer cannot conjure resolution the OS lacks.

**Changing the period starts a fresh health epoch.** The whole reason to change the period is to improve the loop's health, so carrying the pre-change cumulative counters across would be self-defeating — the skip total and `achievedHz` average would still show the old, worse regime and mask the very improvement. So applying a change resets `executedCycles`/`skippedCycles`/`maxExecNs`/`sumExecNs` and re-anchors the `achievedHz` monotonic baseline to that instant. This is done **on the RT thread, at the point it detects the change** (right beside the timer re-anchor), because those counters are single-writer from that thread — resetting them from the HTTP thread's `setPeriod()` would race the loop's `fetch_add`. The console drops its poll-diff baseline on a successful apply so the next 1 s tick doesn't diff fresh counters against the stale sample; the instantaneous-rate diff already clamps negatives to zero as a backstop.

**The period change surfaced dead coupling in the recorder, now removed.** Wiring the retune revealed that `DeviceManager` carried a `cyclePeriodUs` (mirrored from `gameLoop.periodUs`) with two uses — sizing the recorder ring (`capacity = historySeconds × 1e6/periodUs`) and stamping the `.mmpd` dump header — and that a runtime "keep it in sync" setter earned nothing: it never resized the ring (records carry absolute timestamps, so the ring stays decodable), so it only mutated a header field that the offline viewer doesn't need (rows self-describe via `timestampNs`) and that a mid-recording period change would make inconsistent anyway. The ring was always fundamentally **row-based** (`ProcessDataRing::allocate` takes a record count); `historySeconds` was a veneer whose only job was the period division. So: config `recorder.historySeconds` → **`recorder.capacity`** (a fixed row count, default **300000** ≈ 5 min at 1 ms); `DeviceManagerConfig.cyclePeriodUs` and `DeviceManager::setCyclePeriodUs` **deleted**; `recorderHistorySeconds` → `recorderCapacity`; the `cyclePeriodUs==0` division guard is gone; and `cyclePeriodUs` is dropped from the `.mmpd` header. `kDumpFormatVersion` stayed **1** (removed in place — pre-release, no dumps in the wild) so the fixed prefix shrank **40 → 36 bytes** (`rowCount` now at offset 16, `kDumpRowCountOffset` 20 → 16); `mmpd.ts` and the `RecorderPage` were updated in lockstep, the page now deriving cadence from `CycleStatsBar`'s median Δt over the row timestamps instead of the dropped field. Net effect: **`GameLoop` is the single source of runtime-period truth**, `mlock`'d recorder RAM is period-independent (a 250 µs config no longer silently quadruples the ring), and the period-change callback touches only `GameLoop` — the recorder needs no re-sync at all. See the recorder-sizing veneer discussion; the row-based ring is the honest model.

## Session 2026-07-14 — `NotificationBus` outbound design pinned down: the mailbox *is* the queue, and the feature registers itself (design)

Fleshed out the planned `NotificationBus` — the outbound (RT → client) half of the mailbox discipline from Session 2026-07-09 — from a one-line sketch to a specced interface, driven by a good instinct that "this should be two message queues, inbound and outbound, each `{target, payload}`." That instinct is **right about the shape and wrong about the depth**, and separating those two halves is the whole session.

**Right about the shape: the two channels already exist — they are the mailbox, run in each direction.** Inbound (client → RT) is `RtMailboxPool` — target = which slot, payload = the armed program. Outbound (RT → client) is the control block's status atomics + this bus — target = topic, payload = the rendered JSON. There is no separate queue *object* to add; the design has both channels.

**Wrong about the depth: a literal FIFO queue is worse here, for two concrete RT reasons.** (1) *Payload-in-queue forces RT to allocate.* A generic `{target, payload}` has a variable-size payload — inbound payloads are vectors (setpoint buffers), outbound are JSON/strings — and RT can neither `malloc` nor build a string. Forcing "payload is a pointer/scalars only" just puts you back at the retained-`generations` keep-alive and the off-RT `render()` step you already have; the queue is pure addition in front of them. (2) *FIFO is the wrong semantic and it is wasteful.* Progress fires **every cycle (~1000/s)**; a FIFO would enqueue 1000 stale "progress=N"/s that the drainer then dedups back to "latest," whereas latest-wins is one relaxed atomic store — no ring, no sizing, no overflow. Inbound is the same: newest intent must supersede, never replay five stale "go to X". **Depth-1 is the feature, not a compromise.** A FIFO earns its keep only for **must-not-drop discrete events** (each waypoint, distinct fault codes) — which is exactly why that is the one tier that is a ring and everything else is an atomic. For **v1, drop the ring and tiers entirely: atomics-only lifecycle status** (started/progress/done/faulted); add a discrete-event ring later only when a feature genuinely needs guaranteed delivery.

**The real complaint was code organization, not architecture, and the fix is self-registration.** The earlier main.cc sketch built JSON with inline `addSource({...})` lambdas at the composition root — which violates "serialize next to the type" and "dumb wiring layer." The fix: **the feature registers itself.** `TrajectoryCyclicTask::publishTo(NotificationBus&)` lives in `trajectory_task.cc`, next to the `execute()` that writes the scalars and the `trajectoryStatusName()` that names the enum — so producer, formatter, and enum can't drift. main.cc collapses to one line, `trajectoryCyclicTask.publishTo(notificationBus)`, holding no topic/field/JSON knowledge — the same shape as `gameLoop.addTask(&trajectoryCyclicTask)`.

**The bus stays a pure pump.** `NotificationBus` (in `mm::node`, names no server type — the publish seam is a `std::function<void(std::string topic, std::string json)>` wired in main.cc to `WebSocketServer::publish`, identical to `MonitoringManager::setPublish`). It owns **one** off-RT thread regardless of how many features register — the point of generalizing over the bespoke per-task watcher. A feature contributes only a `Source { topic; revision; render }`: `revision` is a cheap read of an RT-bumped atomic (`0` while idle — only inequality is used), `render` builds the message off-RT and returns `std::optional<json>` (`nullopt` suppresses — resolves the idle-slot race cleanly). The pump: registry **fixed before `start()`** (same fixed-membership rule as `CyclicTask`, so the poll thread reads it lockless), `lastSeen_` seeded from `revision()` at `start()` (an already-armed program at boot emits no phantom "started"), then every `pollInterval` (default 20 ms) for each source `if (rev != lastSeen[i]) { lastSeen[i]=rev; if (auto m = render()) publish_(topic, m->dump()); }`. The sleep is a `condition_variable::wait_for` so `stop()` is prompt, not blocked a full interval. Serialisation (`m->dump()`) happens in the bus so `render` returns a composable/testable `json` while the seam stays `(string, string)`.

**The change signal is a standard *version counter* (a.k.a. generation counter / seqlock sequence number), not an invented mechanism.** The control block carries `std::atomic<uint64_t> revision` that the RT writer bumps (`fetch_add(1, release)`) on each notification-worthy change and only ever increases; the off-RT poller keeps a private `lastSeen` copy and treats `revision() != lastSeen` as "changed", never writing it back. A monotonic counter — not a dirty `bool` — is what makes this race-free: there is no clear step to lose an update against, and N bumps between two polls **coalesce** into one publish of the current state, which is exactly the latest-wins semantic wanted. The `release`/`acquire` pairing also makes `revision` the **publish fence**: when the poller observes the new `revision`, it is guaranteed to see the `status`/`progress` scalars written *before* the bump (those stay `relaxed`, read only behind the `revision` acquire), so a snapshot can never show a new generation with stale fields. *(This `revision`/`revision()` naming supersedes the `statusGen`/`changeToken` names used in the earlier Sessions 2026-07-09 and 2026-07-13 outbound sketches — `changeToken` in particular collided with .NET's callback-based `IChangeToken` and misled; the field name avoids `generation` to keep clear of the retained-buffer `generations` vector.)*

**RT side writes scalars only.** `TrajectoryCyclicTask::execute()` per active program does `progress.store(relaxed)` and, on a transition, `status.store(relaxed)` + `revision.fetch_add(1, release)` — never JSON, never a `WebSocketServer` handle. Per-cycle `progress` writes do **not** bump `revision`, so they raise no notification (otherwise ~1000/s); `render` reads whatever `progress` happens to be at the moment a genuine *status* transition fires. Notification latency is therefore **poll cadence (~20 ms), not cycle time** — correct for lifecycle events; anything needing per-cycle fidelity is by definition a monitoring subscription on the recorder ring (tier-2), not the bus.

**One open item for when this lands:** the `RtMailbox` accessors this assumes — `revision()`, `current()` (wait-free active-slot pointer), and `snapshot()` (a struct-of-scalars read). `snapshot()` reads several atomics non-atomically so it can tear across a status change (see `running` with the new `progress`); benign for lifecycle notifications (next poll corrects it, fields are independently meaningful) so **not** gating it on a seqlock for v1. The full inbound generalization (`RtMailboxPool` + `launchRtTask`) remains as specced in Session 2026-07-09; this session only pinned the outbound pump and the self-registration seam.

## Session 2026-07-14 — Hoist `CyclicTask` into `libs/core`: an RT task is a node object fed by a composition-root-owned mailbox channel — no adapter, no controller, no `DeviceManager` pool (design)

Resolved where a runtime RT procedure's *state* lives, and the answer retires a whole tower of indirection the earlier trajectory notes had accreted. The trigger was a plain question — why would `DeviceManager` own a `trajectoryPool`? It never reads or writes it (contrast `ProcessData`, which `exchangeProcessData()` drives every cycle); it would be a **pure holder**. The reason it had been placed there was a *layering accident*, and naming the accident dissolves it.

**The accident: `CyclicTask` lived in `apps/`, so `node` could not implement it.** `cyclic_task.h` sat in `apps/motion_master` next to `GameLoop` (just because `ProcessDataCyclicTask`, the first task, was born there). `libs/node` cannot depend on the app, so a node-layer object could not *be* a `CyclicTask` — hence `ProcessDataCyclicTask`, an app-layer **adapter** whose only job is to be the concrete `CyclicTask` that can see both `GameLoop`'s interface and `DeviceManager`. And hence the earlier trajectory design's contortions: the launcher is a node-layer view (`Cia402Drive::startTrajectory`) that must reach the task to arm it, but node can't name the app-layer `TrajectoryCyclicTask`, so the control-block "rendezvous" was pushed onto the nearest node object both sides *can* see — `DeviceManager` — and a `TrajectoryController` was invented to give the pool a node-layer owner distinct from the app-layer task. Every one of those pieces is downstream of the misplaced header.

**The adapter protects nothing — `GameLoop`'s isolation comes from the interface, not the adapter.** `GameLoop` includes only `cyclic_task.h`; it holds `std::vector<CyclicTask*>` and never sees a concrete task type. So `ProcessDataCyclicTask` was never shielding `GameLoop` from `DeviceManager` — the *interface* does that. The adapter exists solely so *node code* has an implementable `CyclicTask`. It is pure layering tax, not a design boundary.

**The move: `CyclicTask` + `CycleContext` → `libs/core`.** Zero cost, and arguably a correction: `cyclic_task.h` includes only `<cstdint>` (two tiny structs, no deps), and its companion **`CyclicTimer` is already in `libs/core`** — the timer that drives the tasks and the task interface it drives belong together; the split was the real inconsistency. `node` already *talks about* `CyclicTask` in `cia402_drive.h`/`somanet_drive.h` doc comments ("belongs in a `CyclicTask` that re-resolves its `Device` each cycle") — it just couldn't name the type. No dependency cycle: `app → node → comm → core`, plus `app → core`; core depends on none of them. `GameLoop` stays in `apps/` and still only ever sees `CyclicTask*`.

**What it collapses.** A runtime RT procedure becomes **one node object** that is its own tick and self-registers, reading a channel the composition root owns (see the *Refinement* below — the mailbox is **not** a task member):

```text
libs/core/cyclic_task.h     ← CyclicTask, CycleContext            (moved here; joins CyclicTimer)
libs/node/trajectory_task.h ← class TrajectoryCyclicTask : public CyclicTask
                                RtMailboxPool<TrajectoryRun>&   (injected channel — owned at the root, not here)
                                execute(ctx)         (reads the pool, plays one point per cycle — IS the tick)
                                publishTo(bus)       (reads the `revision` atomics → owns render)
apps/motion_master/main.cc  ← RtMailboxPool<TrajectoryRun> trajectoryMailbox;   // the channel, owned here
                                TrajectoryCyclicTask trajectoryCyclicTask{deviceManager, trajectoryMailbox};
                                gameLoop.addTask(&trajectoryCyclicTask);   // GameLoop sees only CyclicTask*
                                trajectoryCyclicTask.publishTo(bus);
                                config.startTrajectory = wire → node startTrajectory(dm, trajectoryMailbox, …)
```

Three things from the earlier design are **rejected outright**, each as a symptom of the accident rather than a real need:

- **`DeviceManager` does not own the pool.** "It cannot live on any one `Device` → therefore `DeviceManager`" skipped the real answer: *→ therefore a channel owned by neither device nor manager* (the composition root owns it — see the *Refinement* below). Multi-axis is unaffected — a coordinated program spans devices only in that the task holds `DeviceManager&` and re-resolves each axis per cycle via `findDevice`, exactly as before. This supersedes the pool-on-`DeviceManager` model in Sessions 2026-07-09/07-13.
- **No `TrajectoryController`.** It existed only to give the pool a node-layer owner separate from the app-layer task. Once the task itself is node-layer, task and controller merge.
- **No app-layer adapter.** `TrajectoryCyclicTask` registers itself with `GameLoop` directly; the launcher is a node free function that arms the shared channel (see the *Refinement* — it names no task, and the `Cia402Drive` view does only the handshake).

**Costs, honestly.** It touches shipped code: `ProcessDataCyclicTask` **relocates** `apps/ → libs/node` (still ~6 lines, but no longer a cross-boundary adapter — a plain node task beside `DeviceManager`), and `game_loop.h`'s include repoints to `core/cyclic_task.h`. It also *commits `node` to housing RT tasks* — a directional statement, but the one already taken by the RT-callable-view plan (Session 2026-07-09): task **logic** (play a buffer into output slots) is a motion concern that belongs in node; task **scheduling** (`GameLoop`) stays in `apps/`. Clean three-way split — interface in core, logic in node, scheduler in app. The HTTP launch still needs **one** wire — a `startTrajectory` composition-root callback in `main.cc` (a `std::function` capturing `DeviceManager&` + the mailbox, mirroring `setGameLoopPeriod`), so `HttpServer` names neither the task nor the pool — not the JSON-building `addSource` loop rejected in the NotificationBus session above.

**Sequencing.** Do the hoist as its own isolated, behaviour-preserving commit *before* the trajectory feature: (1) move `cyclic_task.h → libs/core`, repoint `game_loop.h`, relocate `process_data_task.h → libs/node` — build stays green, behaviour identical; (2) *then* build `TrajectoryCyclicTask` on the clean foundation. This keeps the "did I disturb the RT loop?" structural change separate from the "does trajectory work?" feature — each independently verifiable and revertible. Net: this is not a workaround layered on the earlier design, it removes the thing (a misplaced header) that forced the earlier design's workarounds.

**Refinement (same session) — the mailbox is a decoupled channel, not a task member.** The "owns its inbox" phrasing above overstated it: if the task *owns* the `RtMailboxPool`, every producer must hold a `TrajectoryCyclicTask&` to reach it, which re-couples the whole send path (the HTTP server transitively included) to the concrete task — the very coupling the hoist was meant to relax. Corrected model: the `RtMailboxPool<TrajectoryRun>` is a **composition-root-owned** object (a `main.cc` local, like `gameLoop`/`deviceManager`), injected **by reference** into `TrajectoryCyclicTask` (the sole RT reader) and into a node **launch free function** `startTrajectory(DeviceManager&, RtMailboxPool<TrajectoryRun>&, slavePos, TrajectoryRequest)` (the writer; a coordinated multi-axis analogue mirrors it). The two ends never reference each other — the channel is the only shared address — and the launch **names no task**: it resolves the device, uses a `Cia402Drive` view purely for the op-mode/enable handshake (`prepareForTrajectory(request)`; the earlier `Cia402Drive::startTrajectory(TrajectoryCyclicTask&, …)` form was a responsibility leak — a view over one device has no business reaching the scheduler's task), and builds the immutable `TrajectoryRun` from the client input — a separate `TrajectoryRequest` DTO (matching the existing `OutputStageRequest`; the repo convention is `*Request` = a client-originated command, `*Spec` = an internal descriptor like `PdoSampleSpec`) — which it arms into a claimed slot. `HttpServer` triggers it through a `startTrajectory` `std::function` callback capturing the pool (mirroring `setGameLoopPeriod` and the outbound `NotificationBus` publish seam). **Net: `TrajectoryCyclicTask` the concrete type is named in exactly one place — `main.cc`.** This is the inbound mirror of the outbound decoupling (both a composition-root-owned seam the two sides borrow), and it **generalizes**: `RtMailboxPool<T>` is the client→RT channel for *any* command-driven RT task, `T` its payload struct — own one per task at the root, inject both ways; the only per-task code is `T`, the `execute()` that reads the pool, and the launch's two lambdas (`launchRtTask(validate+handshake, build)`). `TrajectoryRun` is simply trajectory's `T`: the immutable command (`axes`, setpoint buffer, `SkipPolicy`, `repeat`) **plus** the atomic status (`progress`/`status`/`revision`) the outbound `NotificationBus` reports — one struct spanning both channels, command fields `const` after `arm()`, status atomics `mutable`. The remaining asymmetry is intrinsic: **one shared `NotificationBus` outbound** (every message serialises to JSON, so the type erases to a string) vs **one typed `RtMailboxPool<T>` per task inbound** (the RT consumer reads a typed struct alloc-free with no dispatch, so a single type-erased inbox is impossible). Resist a task/mailbox *registry* until a second command-driven RT task exists — the `RtMailboxPool<T>` type and the wiring pattern are the generalization; a dispatcher would be speculative.

## Session 2026-07-16 — Lockstep versioning is a contract statement, not a convenience (rationale)

Clarification session, no code change. Pins down *why* the `motion-master` binary, the `web/apps/*` PWAs, and `@synapticon/motion-master-client` all carry one shared version and bump together — a decision the tooling already enforces (`tools/bump-version.sh` writes every artifact's version from the single `VERSION` file; `release.yml` builds the binary and publishes the client on the same `v*` tag; `deploy-pages.yml` pins the hosted PWAs to the latest tag). The open worry was the visible waste: a UI-only or client-only change still bumps the binary, which can produce a byte-identical rebuild under a new number.

**The waste is the cheap side of a good trade, because all three artifacts share one contract.** The contract is **the HTTP API (61447) + the WebSocket protocol (62281)** — the binary *implements* it, the client library *wraps* it, the PWAs *consume* it through the client. They are bound to that one surface by construction, so a single version number is an accurate, zero-effort statement that they were built and tested against the same contract: `console@X` works against `binary@X`, no compatibility matrix, no version-mapping table. Independent per-component versions would only re-encode, by hand and with ongoing maintenance (a matrix, per-artifact minimum-server-version checks, users reasoning about combinations), a fact lockstep gives for free. An occasional no-op binary rebuild is strictly cheaper than that machinery — version numbers are free and infinite.

**The governing rule is "same contract → same version," not "same repo → same version."** This is the part worth keeping sharp: lockstep is justified by the *shared API surface*, not by co-location in the monorepo. Every artifact shipped today (and every one planned) touches that surface, so all of them lockstep — the rule currently has no exceptions. It earns its place by naming the *one* case that would break it: a future artifact depending on **none** of the API/WS contract (a standalone offline tool, say), for which a shared version would be pure noise with no compatibility payoff. No such artifact exists; the principle is recorded so that exception is recognisable rather than re-litigated.

**Two consequences, both already true in the plumbing.** (1) A web-only fix still needs a version bump + `v*` tag — the hosted PWAs track the latest tag, never `main` (CI/`deploy-pages.yml`), so `main` alone never changes what users run. (2) Because the version number no longer signals *what* changed (a bump can be binary-only, UI-only, or all three), that signal has to come from **commit scopes** (`style(console)`, `feat`, `fix(comm)`) and/or a changelog — not from decoding the version. Recorded in CLAUDE.md's *Versioning* section as the standing rationale.

## Session 2026-07-17 — The error-promotion path made concrete: `FoeError` as a reference, not a rewrite (design)

Discussion session prompted by a question about `readFile`/`writeFile` in the fieldbus driver returning `std::expected<T, std::string>`: file operations fail for several distinct reasons (file-not-found, buffer-too-small, packet-number desync, generic protocol error, no-response) — how does a caller tell them apart, and is a plain string the right error type? The answer is a worked example of the no-exceptions mandate's "`std::string` by default, structured `Error` only where a caller branches" rule, plus a small artefact so the promotion is copy-pasteable the day it's needed.

**Nothing changed in behaviour.** The FoE reason was never actually lost — `foeErrorDetail` in `soem_fieldbus_driver.cc` already decodes SOEM's negated `ec_err_type` into `" (file not found)"` / `" (buffer too small)"` / … and appends it to the message. Every FoE caller today (the two `http_server.cc` handlers) only forwards that text into a 500 body; a grep confirms **no production code string-matches an error message** to steer control flow (the only `.error().find(...)` hits are in tests asserting message content, which is legitimate). So the branchable information is present for humans; what's absent is a typed handle, and no caller needs one yet. Uniform `std::string` therefore stays correct — it composes through `.and_then()` across layers and costs nothing to produce.

**The artefact: `libs/comm/foe_error.h`, deliberately unused.** It defines `FoeError { FoeErrorKind kind; Retry retry; std::string message; }` with a string face (`operator<<`, `.message`, `.what()`) so promoting `readFile`/`writeFile` from `std::string` to `FoeError` would change forwarding callers by at most one word (`.error()` → `.error().message`) and ripple no further — the whole point of the mandate's "keep the two interchangeable" clause. The transport-agnostic vocabulary (kind + retry + reason) lives in the header; the SOEM-specific `wkc → kind` decode stays in the driver `.cc` where the SOEM include already is, so a future SPoE driver maps its own codes to the same `FoeErrorKind`. A top-of-file comment records that it is currently unused, when it would be used (a firmware flasher branching retry-on-transient / abort-on-permanent — the live candidate, given the BOOT warm-up drain-retry work), and why the design is shaped this way.

**Why this shape and not the two nearby alternatives.**

- *Not a plain string forever*: the predictable next consumer is a flasher that must distinguish "device still warming up after BOOT, retry" from "no such file, give up." That's genuine branching, and message-matching to do it is exactly the smell the mandate names.
- *Not a generic `OpError` swept across the codebase* (the question that came up explicitly): a single shared error type forces its enum to be either too coarse to serve the branching caller — who then string-matches the message anyway, defeating the point — or a grab-bag union of every layer's failure modes (FoE + SDO + AL-state + register), which is just all the per-surface enums glued together, buying cross-layer coupling with no abstraction. It also taxes the ~90 % of call sites that only log, forcing them to pick a `kind` they never read. Hence per-surface-or-string, nothing in between; and the type is named for its surface (`FoeError`), not `OpError`, because a generic name is a magnet for the global-sweep anti-pattern the mandate forbids.

**The `retry` tag is the one genuinely shared axis.** Transient-vs-permanent is the distinction *every* fallible bus op's branching caller would act on (an `SdoError` would carry the same tag), so it rides *alongside* the FoE-specific `kind` rather than being encoded into it — the `absl::Status` insight (a universal code + operation-specific payload), sized down to the single distinction that matters here. This is the same fork Rust drew as `anyhow` (leaf/forwarding code) vs `thiserror` (libraries whose callers `match`), and Go as `fmt.Errorf("…: %w")` vs `errors.Is`/`errors.As`; the mandate is a deliberate pick of the "typed only where you branch" side. Should a second surface promote, factor `Retry` (and `operator<<`/`.what()` boilerplate) into a shared home then — not pre-emptively.

Promoting for real is a ~6-file change (both driver impls, the `FakeDriver` double, the `FieldbusDriver` virtual signatures, `Device`, the two handlers) — contained but deliberate, so it waits for the first real branching caller.

## Session 2026-07-17 — Fixed-width integer style: `<cstdint>` + unqualified `uint16_t` (decision)

Side question while reviewing `foe_error.h`'s `#include <cstdint>`: since C++ also exposes `std::uint16_t`, is the unqualified `uint16_t` the "wrong"/less-C++ form, and is it worth switching the codebase to the namespace-qualified names? Decision: **leave it — unqualified `uint16_t`, included from `<cstdint>`, is the house style, and it stays.**

The technical facts settle it. `<cstdint>` (the C++ header — the codebase never uses `<stdint.h>`) *guarantees* the names in namespace `std`; whether it *also* injects them into the global namespace is, per the standard, only conditionally supported. So unqualified `uint16_t` leans on an optional-but-universal behaviour — libstdc++, libc++, and MSVC all inject the global names, and `-Wpedantic` (which flags extensions, not reliance on optional-standard behaviour) does not complain. The gap is theoretical; there is no target compiler on which it fails.

Against that near-zero portability upside stands an unambiguous existing convention: a grep found **1952** unqualified `uint(8|16|32|64)_t` uses, **0** `std::`-qualified ones, and **50** `<cstdint>` includes with **0** `<stdint.h>`. A sweep to `std::uint16_t` would be a huge, review-noisy diff across hot files for no behavioural change and a benefit that never materialises. Consistency wins decisively. The standing rule, therefore: **include `<cstdint>` (never `<stdint.h>`), write the unqualified names.** (`std::`-qualified would be the strictly-portable-per-standard choice for a greenfield "nothing leaks to global" project — noted only so the road not taken is on record; it is not this project's rule.)

## Session 2026-07-17 — Auto-discover `motion-master.jsonc` next to the binary (so the Windows release can preset a 4 ms RT period)

Problem: the Windows release (`motion-master-<version>-windows-x64.zip`) runs the game loop at the 1 ms built-in default, but most stock Windows hosts can only sustain ~1.5 ms timer granularity, so the loop degrades to skip-to-grid and drops a third of its cycles right after start (see the coarse-timer note in CLAUDE.md). Field experience: **4 ms is the sweet spot on typical Windows.** We want the Windows package to just work at 4 ms, while Linux/macOS keep 1 ms.

Decision — **partially reverse the 2026-06-08 "explicit `--config` only, no default search path" rule**: when no `--config` is given, auto-discover a `motion-master.jsonc` sitting **next to the executable** (`mm::core::exeDir()`, the same anchor the TLS cert discovery already uses) and load it. `--config` still wins over it; with neither, the in-code defaults apply. The `/etc`/system-wide lookup the original rule rejected **stays rejected** — the only implicit path is scoped to the install directory (a location the operator populated by unzipping), never a machine-global one they didn't name. Auto-discovery is **cross-platform** (identical next-to-binary rule on every OS); only the *shipped* file differs.

Why a real config file rather than a compiled-in `#ifdef _WIN32` default of 4000: the value stays **visible and operator-editable** (unzip, open `motion-master.jsonc`, change the number) instead of baked into the binary, and it reuses the config path already exercised by `--config` — one loader, one schema, no platform branch in `config.h`. The built-in default remains 1000 for every platform; Windows just ships a file that overrides it.

Mechanics:

- **Loader (`options.cc`):** resolve the effective path — `--config` if present, else `exeDir()/motion-master.jsonc` if it exists — then the existing parse/validate/`parseConfig` path runs unchanged. `Options::configPath` now records the *effective* path (was: only `--config`).
- **Shipped file:** `apps/motion_master/motion-master.windows.jsonc` (a minimal partial override — just `gameLoop.periodUs: 4000` + comments; every other key stays default). `release.yml`'s Windows leg copies it to `motion-master.jsonc` beside the exe before zipping. Linux/macOS legs ship **only** the annotated `motion-master.example.jsonc` (unchanged) — no active config, so their 1 ms default holds.
- **Docs touched:** `motion-master.example.jsonc` header (now documents the two-file load order + how to activate it by copying to `motion-master.jsonc`), CLAUDE.md (config mandate + release.yml description), swagger `GET /api/config` description, and this note. The example file stays *not loaded* under its own name — only the exact basename `motion-master.jsonc` is auto-discovered.

## Session 2026-07-18 — Jasper is a soft-ESC: block LRW, send split LRD/LWR process data

Why `SoemFieldbusDriver::blockLrwOnPruIcssSlaves()` exists, and how EtherCAT process-data addressing actually works — written up because the "block LRW, use LRD+LWR separately" configuration reads as opaque without the underlying model.

**What a soft-ESC is.** An EtherCAT Slave Controller (ESC) is the chip that terminates the EtherCAT protocol on a slave: it processes frames on the fly at wire speed, applies FMMUs/SyncManagers, maintains the Distributed Clock, and increments the working counter. Normally it is a dedicated hardware ASIC or FPGA (Beckhoff ET1100/ET1200, Microchip LAN9252) with the datagram-processing unit implemented in silicon. A **soft-ESC** implements that same processing unit in *software/firmware on a general-purpose core* instead. SOMANET **Jasper** uses TI's **PRU-ICSS** (Programmable Real-time Unit — Industrial Communication SubSystem): small deterministic RISC cores on the TI SoC run firmware that emulates the ESC. It is a real EtherCAT slave to the outside world, but its frame processing is code on the PRU, not gates — and that firmware does not implement the full datagram-command set with the same completeness a hardware ESC does. We detect it by reading ESC register `0x0000` (`ECT_REG_TYPE`): low byte `0x90` == TI PRU-ICSS (`blockLrwOnPruIcssSlaves()`, `soem_fieldbus_driver.cc`).

**EtherCAT process-data addressing — the three logical commands.** Cyclic process data uses *logical addressing*: each slave's FMMU maps a slice of a flat logical address space to its local physical memory, so the master addresses the whole bus's process image by offset without naming individual slaves. Three EtherCAT commands operate on that space:

- **LWR** (Logical Write) — master puts output data in the datagram; each slave *copies its outputs out* as the frame passes.
- **LRD** (Logical Read) — each slave *stamps its inputs into* the datagram as the frame passes; the master reads them when it returns.
- **LRW** (Logical Read/Write) — **one** datagram does both in a single pass: at a shared logical range, output slaves read their outputs out of the frame *and* input slaves write their inputs into it, in transit. This is the default and the optimisation — one datagram, both directions.

The working-counter arithmetic in the codebase encodes the LRW convention: an output region contributes **+2** and an input region **+1** to the expected WKC (`fieldbus_driver.h`), because an LRW slave that both reads and writes bumps the counter by 3. With split LRD/LWR that accounting differs (each direction is a separate +1), which SOEM handles internally once `blockLRW` is set.

**Why Jasper can't do LRW.** The PRU-ICSS firmware's processing unit mishandles the combined read-and-write LRW datagram. When an LRW cyclic frame hits it, every frame dies in the processing unit (RX error counter `0x030C` saturates at 255), the working counter stays 0, and the SAFE-OP → OP transition fails with AL status `0x001B` (SM watchdog) — the drive never sees valid process data, so its sync-manager watchdog trips. (This is the same class of soft-ESC limitation behind the neighbouring `deactivateMailboxStatusFmmus()` fix, where a *register-space* FMMU inside an LRW is likewise fatal on these ESCs.)

**The fix — `blockLRW`.** Before `ecx_config_map_group` builds the group's send plan, we read each slave's ESC type and set `s.blockLRW = 1` on every `0x90` slave. `ecx_config_map_group` folds the per-slave flag into `grouplist[0].blockLRW`, and from then on SOEM emits, per cycle, an **LWR datagram for the outputs and a separate LRD datagram for the inputs** instead of one combined LRW. Same process data, split into a write pass and a read pass — the two-datagram form the PRU firmware does handle correctly. Must run *before* the map, since that's when SOEM reads `blockLRW` to build the plan. The cost is one extra datagram per cycle (~12-byte header + the different WKC accounting), not a whole extra Ethernet frame: EtherCAT concatenates multiple datagrams into a single frame up to the ~1486-byte MTU, so for a normal Jasper bus the LWR + LRD pair rides in **one** Ethernet frame (SOEM only segments across frames when a group's process data exceeds the MTU — which happens with plain LRW too).

**Orthogonal point that always confuses — `send` vs `receive` is *not* the read/write split.** `exchangeProcessData()` always calls two functions: `ecx_send_processdata()` (put the frame on the wire, returns immediately) then `ecx_receive_processdata(EC_TIMEOUTRET)` (**block** until that *same* frame circulates the daisy-chain and loops back). EtherCAT is a ring: one physical frame goes out and comes home, and slaves fill it in transit. This split is about *time* (transmit vs round-trip completion) and exists for every SOEM setup regardless of LRW — nothing "goes out" on `receive`; it only waits for what `send` already launched. The LRW-vs-LRD/LWR choice is about *datagram packing within that frame*, a completely separate axis. Don't conflate the two: Jasper still does `send…` then `receive…` every cycle; blocking LRW only changes whether that frame carries one combined datagram or two direction-specific ones.

## Session 2026-07-19 — Off-RT procedures with progress: `ProcedureManager` owns the threads + busy-set; OS command is generic, the measurements are SOMANET; progress is a `ProgressStep[]` snapshot (design)

Designed the shape of the first **off-RT procedure with live progress** — Offset Detection — with the explicit goal that it becomes the mould every later off-RT procedure (auto-tuning, firmware install) is poured into. The reference is the previous client's `offset-detection.ts`: a fixed, ordered array of `ProgressStep` (`{id, label, status: idle|running|succeeded|failed, value?, error?}`) that the procedure clones and mutates as it runs, emitting the whole array over time so a UI renders where each step is. Nothing is implemented yet; this pins the layering.

**Three tiers, split by where the knowledge actually lives.** (1) *Mechanism — `ProfileDevice::runOsCommand(cmd, timeout)`.* The OS command object set (CiA301 0x1023 command / 0x1024 mode / 0x1025 response) is **generic** — any CoE device that supports OS commands has it — so it belongs on `ProfileDevice`, exactly like the device-type/identity accessors that made `ProfileDevice` concrete (Session 2026-07-18 landed those). It is the blocking handshake: write the command payload, poll the status/response sub-object until done or timeout, decode the reply. Control-plane only, serialized on `controlPlaneMutex_`, and slow (seconds) — which is *why* it's off-RT. (2) *Commands — typed OS commands on `SomanetDrive`.* Each SOMANET measurement is a thin typed wrapper that builds the command bytes, calls `runOsCommand`, and decodes the result: `measurePhaseResistance() → double`, `measurePhaseInductance() → double`, `detectPhaseOrder() → …`, `measureCommutationOffset() → int`, plus open-phase detection. Single-shot, synchronous, individually unit-testable — no threading, no progress. This is the vendor tier. (3) *Procedure — the orchestration*, below.

**The procedure body is a plain function over a reporter — not a class.** `runOffsetDetection(SomanetDrive&, ProgressReporter&, std::stop_token)` runs the ordered tier-2 calls, updating the reporter's step array after each (`running` → `succeeded`/`failed`, stamping `value`), checking the stop token between steps. Keeping it a free function over an injected reporter keeps the orchestration **transport-free and testable** (feed a fake reporter, assert the emitted step sequence) — the same discipline as the trajectory launch being a node free function, not a method reaching the scheduler.

**`ProcedureManager` owns the threads *and* the per-device busy-set — this is the decided piece.** It is the `MonitoringManager` analogue for command-and-wait work: it owns the cancellable `std::jthread`s and a **per-device exclusive activity token**. The busy-set is the part `controlPlaneMutex_` cannot provide — that mutex serializes *individual transactions* (one SDO, one mailbox round-trip), but a procedure is a *multi-second span of many transactions* interleaved with sleeps, and "this device is busy detecting offset — reject a second start, reject a conflicting motion, block a rescan" is a span-level exclusion. So `ProcedureManager::start(devicePos, …)` **try-acquires the device's token** and returns *rejected* (→ HTTP 409) if it's already held; the token releases when the jthread exits (success, failure, or cancel). This supersedes the earlier "a dedicated `FirmwareInstaller` … is planned" note in CLAUDE.md: firmware install is not a bespoke class, it is *another procedure body + step template* hosted by the same `ProcedureManager`.

**Progress is a full-array snapshot, re-emitted on every step change — never a delta.** The procedure seeds its `std::vector<ProgressStep>` from a per-procedure template (the C++ mirror of `offsetDetectionSteps`) and re-emits the *entire* array each time a step transitions. Full-snapshot is what makes a **late-joining subscriber correct with zero replay logic**: a UI that opens mid-run receives the next snapshot and renders the complete current state — no need to have seen the earlier messages. The POD, in `mm::node`:

```cpp
enum class ProgressStatus { Idle, Running, Succeeded, Failed };
struct ProgressStep { std::string id; ProgressStatus status; std::optional<double> value; std::optional<std::string> error; };
```

**Delivery does *not* go through the RT `NotificationBus` poll — it publishes directly.** Important distinction from the trajectory outbound design (Session 2026-07-14): the `NotificationBus` exists because an **RT** producer *cannot call out* (no alloc, no strings, no server handle), so it bumps a `revision` atomic and an off-RT poll thread renders + publishes. A procedure has none of that constraint — it *already runs on a normal off-RT thread* and knows exactly when a step changed — so it calls the publish seam **directly** when it mutates a step, with no version-counter poll in between. `ProcedureManager` holds the same `std::function<void(topic, json)>` publish callback wired in `main.cc` (mirroring `MonitoringManager::setPublish`), names no `WebSocketServer`. Reuse the existing notification message type rather than inventing one:

```json
{"type":"notification","data":{"event":"offset-detection-progress","devicePosition":3,"steps":[ … ]}}

```

So the RT/off-RT boundary decides the transport: RT sources → `NotificationBus` poll pump; off-RT procedures → direct publish through the identical seam. Both keep `node` ignorant of the server; only the trigger cadence differs.

**Start surface mirrors `MonitoringManager`.** A node entry point (free function or `ProcedureManager` method) `startOffsetDetection(DeviceManager&, devicePos)` resolves the device, builds a `SomanetDrive` view for validation, try-acquires the token, and spawns the jthread; it returns *accepted*/*rejected* synchronously (nothing else running → accepted). HTTP is a thin `POST /api/devices/:pos/procedures/offset-detection` → 202 accepted / 409 busy, wired so `HttpServer` reaches `ProcedureManager` the same way it reaches `MonitoringManager` today (a held reference or a composition-root callback — dumb HTTP layer, node owns the logic). A C++ caller hits the node function directly. Cancel is `DELETE`/a second endpoint that requests the jthread's `stop_token`.

**A companion `GET /api/devices/:pos/procedures/offset-detection` returns the current `steps` snapshot — this is decided, not a lean.** The WebSocket stream alone leaves a client that connects (or reloads) *mid-run* blind until the next step transition fires, which for a slow measurement step can be seconds of an empty UI. The `GET` closes that gap: `ProcedureManager` already holds the live `steps` array for the busy device (it's what it publishes), so the endpoint just serializes the current snapshot — a UI opening mid-run renders the complete state immediately, then follows the WS deltas. When no procedure is running (or has ever run) for the device it returns the idle template (all steps `idle`), so the client always has a well-formed array to render. This is the same "snapshot to catch up, stream to stay current" pattern the monitoring surface uses (`GET /api/monitorings/{topic}` for the parameter order, WS for the rows); the procedure surface mirrors it. Cheap because the snapshot is a byproduct of publishing, not extra state.

**The WebSocket is optional — polling the `GET` alone is a fully supported, lossless mode.** A client that never opens a WebSocket can just poll the `GET` on an interval to track a procedure to completion, and the snapshot design is what makes that correct rather than lossy. Because each notification is a *full-array snapshot in which every step retains its terminal `succeeded`/`failed` status and its measured `value`* — an accumulating state, not a discrete-event feed — a poller **cannot miss a result**: even a step that both starts and finishes between two polls is still visible as `succeeded` (with its value) in the next snapshot. The only thing polling skips is the transient `running` blip on a fast step, which carries no data. For offset detection (seconds-long measurement steps) a 500 ms–1 s poll observes every transition in practice anyway. So WebSocket is the low-latency *push*; `GET`-poll is the equivalent *pull* — same authoritative snapshot, client's choice. Two requirements this places on `ProcedureManager` for polling-only to be fully correct: **(1) the last `steps` snapshot persists after the jthread exits** (retained per device until the next run overwrites it) — otherwise a client polling *after* completion would see the idle template and never learn the outcome; and **(2) the payload carries an overall procedure `status`** (`idle`/`running`/`succeeded`/`failed`) alongside the per-step array, so "is it done?" is a single-field check and a polling loop is trivial (`while status == running: sleep; GET`) instead of scanning every step.

**Open items, with current leans (not locked — only `ProcedureManager` ownership is decided):**

- *`value` type* — TS uses a single `number` and formats client-side (`Yes/No`, `mΩ`, `Normal/Inverted`). Lean: keep server `value` numeric + client-side formatting (server stays dumb, i18n on the client, matches the reference's `formatOffsetDetectionStepValue`); revisit only if a step needs a genuinely non-numeric payload.
- *label ownership* — reference emits `label` from the server template. Lean: server emits only `id/status/value/error`; **client owns labels + formatting** (it already owns the formatter). Less server churn for wording.
- *cancellation granularity* — `stop_token` checked *between* steps; an in-flight OS command finishes (they aren't abortable once issued), then the procedure stops and returns the drive to a safe state (disable → SwitchOnDisabled) before exiting. Lean: accept between-step cancellation as sufficient.
- *message type* — reuse `{"type":"notification", data:{event,…}}` vs a dedicated `{"type":"procedure",…}`. Lean: reuse — less protocol surface, client already multiplexes on `data.event`.

**Why this generalizes cleanly.** The only per-procedure code is (a) the step template, (b) the body `runXxx(SomanetDrive&, ProgressReporter&, stop_token)`, and (c) any typed tier-2 commands it needs. Threading, the busy-set/single-run guard, snapshot emission, cancellation plumbing, and the publish seam are all `ProcedureManager` + `ProgressStep` — written once. Offset detection, auto-tuning, and firmware install differ only in their body and template. Resist a procedure *registry*/dispatcher until a second procedure actually exists — the `ProcedureManager` + procedure-as-function-over-reporter pattern is the generalization; a dispatcher would be speculative (same restraint as the inbound `RtMailboxPool<T>` note in Session 2026-07-14).

## Session 2026-07-20 — An SDO read failure message is diagnostic: bare `failed` = 700 ms mailbox timeout (no answer); `failed (SDO abort …)` = fast CoE refusal (as-built)

A user noticed that reading an unknown object gave two *different* error messages for adjacent subindices — `0x2345:00` → `SDOread slave 1 0x2345:00 failed (SDO abort 0x08000000: General error)`, but `0x2345:01` → a bare `SDOread slave 1 0x2345:01 failed` with no abort code — and asked why. This note records the answer, because it's a genuinely useful diagnostic that isn't obvious from the message and I initially guessed it wrong.

**Both messages come from the same branch in `SoemFieldbusDriver::readSdo`** (`wkc <= 0` after `ecx_SDOread`; `wkc` is the EtherCAT **working counter** — SOEM's `ec_main.h` documents the `outputsWKC`/`inputsWKC` fields as "workcounter", `ec_type.h`'s `EC_WKCSIZE` as the datagram's workcounter item — the field a slave's ESC increments only when it processes the addressed memory, so at the end of a mailbox transaction `wkc > 0` is SOEM's own "succeeded to read slave response?" test and `<= 0` means the receive datagram went unanswered). **Caveat — the working-counter reading is CoE-specific.** CoE (`ec_coe.c`) keeps `wkc` a genuine working counter: on an abort/packet error it sets `wkc = 0` and pushes the detail onto the error queue (which is why `readSdo` classifies via `ecx_poperror`, above). FoE (`ec_foe.c`) and EoE (`ec_eoe.c`) instead **overload the identically-named return with a negated `EC_ERR_TYPE_*` enum** on failure — `ecx_FOEread` returns e.g. `-EC_ERR_TYPE_FOE_FILE_NOTFOUND` (`= -10`) or `-EC_ERR_TYPE_FOE_BUF2SMALL` (`= -6`) — so a *negative* FoE/EoE return is an error code carrying the specific reason, not a working counter, and there is nothing on the error queue to pop. That is exactly why the FoE path decodes the return value directly: `SoemFieldbusDriver::foeErrorDetail(int wkc)` does `switch (-wkc)` over the `EC_ERR_TYPE_FOE_*` set rather than calling `sdoErrorSuffix`. Same variable name across `ecx_*` functions, two different contracts — don't carry the CoE working-counter reading over to FoE/EoE. The base line is always `SDOread slave N 0xIIII:SS failed`; `sdoErrorSuffix()` appends a suffix **only if** `ecx_poperror(ctx, &err)` pops an `ec_errort` off SOEM's per-context error queue. So the presence/absence of the suffix is entirely "did SOEM enqueue an error for this transaction," and reading `ecx_SDOread` (`ec_coe.c`) shows exactly when it does:

- The slave returns a proper CoE **Abort SDO Transfer** frame (`Command == ECT_SDO_ABORT`) → `ecx_SDOerror()` enqueues `EC_ERR_TYPE_SDO_ERROR` with the abort code → **`failed (SDO abort 0x…: <reason>)`**. This is *the slave answering and refusing* — fast (single mailbox round-trip, ~ms).
- An unexpected/wrong-shape frame comes back → `ecx_packeterror()` enqueues `EC_ERR_TYPE_PACKET_ERROR` → **`failed (packet/timeout error)`**.
- `ecx_mbxsend` fails **or** `ecx_mbxreceive` times out — *no CoE response arrives at all* → `ecx_SDOread` returns `wkc <= 0` having **enqueued nothing** → **bare `failed`**. Crucially, the receive blocks the full `EC_TIMEOUTRXM` (**700000 µs = 700 ms** in the pinned SOEM `ec_options.h`) before giving up.

**Does the CoE `wkc` return carry meaning without `ecx_poperror`? Partially — and it's a genuinely useful distinction we currently throw away.** Following up on the question of whether SOEM encodes the failure reason in the return value the way FoE does: for CoE it's a *hybrid*. The documented `EC_*` return codes (`ec_type.h`: `EC_NOFRAME -1`, `EC_OTHERFRAME -2`, `EC_ERROR -3`, `EC_SLAVECOUNTEXCEEDED -4`, `EC_TIMEOUT -5`) can come straight out of `ecx_SDOread`, so a **negative** return is self-describing, but a **zero** return is not. Tracing the three CoE failure paths:

- **Mailbox-receive timeout** (slave never answers): `ecx_mbxreceive`'s "no read mailbox available" branch sets `wkc = EC_TIMEOUT` and does **not** clear it, so `ecx_SDOread` returns **`-5`** — meaningful *on its own*, no error queue needed.
- **Mailbox-send failure**: `ecx_mbxsend` ends with `if (wkc < 0) wkc = 0;` (`ec_main.c`) — it **clamps its own negatives to `0`** — so `ecx_SDOread` returns **`0`**.
- **Abort / unexpected frame**: enqueues the detail (`ecx_SDOerror`/`ecx_packeterror`) and forces **`wkc = 0`**.

So `0` is the *only* ambiguous value (send-failed vs. answered-with-abort), and that is precisely the case the error queue exists to explain; `EC_TIMEOUT (-5)` is unambiguous from the return alone. **Concretely: our `0x2345:01` bare failure is `wkc == -5`, not `0`** — the ~703 ms latency (= `EC_TIMEOUTRXM`) confirms it sat in the receive timeout. This corrects the first version of this note, which said the unanswered case returns "`0`". `SoemFieldbusDriver::readSdo` originally collapsed everything into `if (wkc <= 0)` and classified purely via `ecx_poperror`, discarding the `-5`-vs-`0` signal — so a "no response" timeout and a send failure both rendered as the same suffix-less `failed`. **Fixed 2026-07-21:** `sdoErrorSuffix(ctx, wkc)` now takes the return value; it still prefers a queued reason (SDO abort / mailbox error) when `ecx_poperror` yields one, but on an *empty* queue it decodes the `wkc` sentinel — `EC_TIMEOUT (-5)` → `(no response — mailbox timeout)`, `EC_NOFRAME`/`EC_OTHERFRAME`/`EC_ERROR`/`EC_SLAVECOUNTEXCEEDED` → their own tails — leaving only `wkc == 0` with nothing queued (a mailbox-send failure) as a bare `failed`. So `0x2345:01` now reports `… failed (no response — mailbox timeout)` while `0x2345:00` still reports `… failed (SDO abort 0x08000000: General error)`. The message is richer but nothing *branches* on the reason yet — the `~700 ms` stall is identical — so this stays a `std::string` face, not a promotion to a structured `Error`; that promotion waits for a real "retry-on-timeout, abort-on-refusal" caller (a firmware/param-sweep feature), exactly the `FoeError` posture.

**Verified against real hardware, not reasoned** — the user had a SOMANET Integro wired up in PRE-OP, so I read it through the running server's `GET /api/devices/1/sdo/0x2345/0x{00,01}` and timed it with `curl -w %{time_total}`:

| Read | Latency | Body |
| --- | --- | --- |
| `0x2345:00` | **~6 ms** | `… failed (SDO abort 0x08000000: General error)` |
| `0x2345:01` | **~703 ms** | `… failed` (bare) |

The ~703 ms is `EC_TIMEOUTRXM` to the millisecond — proof the bare failure is a **mailbox-receive timeout**, i.e. the slave sends nothing for that subindex. Fully deterministic across interleaved orderings (`00,01,00,01,01,00`), which **falsified my first guess** ("transient mailbox desync after the first abort, retry clears it"). It doesn't clear — the slave simply never answers `:01`. Subindex 0 is different because it's always meaningful (the entry-count / highest-subindex field of any object that exists at all), so the slave recognises the object enough to refuse it — with the catch-all `0x08000000` "General error" rather than the cleaner `0x06020000` "object does not exist" or `0x06090011` "subindex does not exist", i.e. SOMANET handles `0x2345` in a vendor-specific/degenerate way.

**Two takeaways worth keeping.** (1) *A bare `failed` is "no answer" (timeout/mailbox), not "no such object."* Those are different failure classes; don't string-match a bare `failed` into "doesn't exist." If you need to know whether a subindex truly exists, a repeatable bare timeout means the slave isn't responding at all, whereas a repeatable abort code is the slave's definitive reason. (2) *Probing unknown subindices is expensive* — every miss that isn't an explicit abort costs ~700 ms of wall time (the `readSdo` call, and the HTTP handler bracketing it, block for the whole `EC_TIMEOUTRXM`). Relevant if a future feature ever sweeps a subindex range blind rather than reading a known layout. No code change came out of this — the behaviour is correct SOEM/CoE semantics; it's the *interpretation* of the two message shapes that's the durable fact.

## Session 2026-07-24 — Store parameters (0x1010) ships; and why a profile procedure is a thin *access wrapper* on `DeviceManager`, not a growing per-verb surface (rationale)

Shipped the generic CANopen **store-parameters** feature: `ProfileDevice::runStoreParameters(StoreParametersConfig)` writes the ASCII `"save"` signature (`0x65766173`) to `0x1010:01`, waits a settle for the drive to begin its flash write, then polls `0x1010:01` until it reads back `1` ("save completed"), retrying a poll that isn't yet `1` — a value mismatch *or* a transient mailbox read error (a store in progress can leave the mailbox briefly unresponsive) — up to `config.retries` times, `config.interval` apart. It's synchronous and sleep-polling, exactly like `Cia402Drive::enable()`: it runs on the control-plane (HTTP) thread and blocks it for the store's few seconds, which is fine because each poll's bus access takes the driver's control-plane lock only per transaction and the WebSocket is a separate port/loop. Surfaced as `POST /api/devices/{slavePosition}/store-parameters?retries=&interval=` (the `settle` stays internal — it tracks device behaviour, not caller preference — but is a `StoreParametersConfig` field so a caller/test can override it; tests pass zero delays to run the retry/confirm walk instantly). It lives on `ProfileDevice` (the root of the view chain) because `0x1010` is a **generic CiA301 object present on any CoE device**, not a drive-specific one — nothing about it is EtherCAT-specific either, so it generalises to SPoE and future fieldbuses for free.

**The design question worth recording** (a user asked it): `DeviceManager` also has a `runStoreParameters(slavePosition, config)` — is `DeviceManager` the right home for a growing set of these, on a general-purpose multi-protocol platform? Answer: **yes for this class of operation, and it's important to see *why*, because the same reasoning marks the seam where it would stop being right.**

- **The domain logic is not on `DeviceManager`.** It's on the profile *view* (`ProfileDevice::runStoreParameters`). The `DeviceManager` method is *purely an access wrapper*: take `busMutex_` (shared) → resolve the device by position (`resolveProfileDevice`) → bind the view → run the op under the lock → return. So it is not a service-layer method sneaking back in (the "no service layer" mandate holds); the logic still lives with the object-dictionary knowledge.
- **Only `DeviceManager` *can* be that wrapper.** Profile views **borrow** `Device&`, and a `Device&` from `findDevice` is valid only while `busMutex_` is held — a rescan rebuilds the `devices_` vector and dangles the reference. `busMutex_` is private to `DeviceManager`. So the "hold the lock across a borrowed-view operation" glue is structurally forced into `DeviceManager`; an HTTP handler can't hold that lock (and, per the *dumb HTTP layer* rule, shouldn't try). This is the same pattern as `runCia402Command` / `setCia402Target` / `getCia402Status` — deliberately consistent.
- **What doesn't scale is one `DeviceManager` method per profile verb.** Each new profile procedure (store, restore, a homing kick-off, an SDO-based tuning step…) currently adds another `resolve + lock + delegate` method and forces `DeviceManager` to *name* every profile type. Left unchecked that drifts toward a god-object that must know every profile's vocabulary — which is not its job (own the device set + fieldbus lifecycle + scanning).

**The seam, and the trigger.** When the resolve-and-delegate surface grows past the CiA402 set (rule of thumb: ~3+ more of these piling up), extract **one** lifetime-scoped access primitive on `DeviceManager` — `withDevice(pos, fn)` that takes the shared lock, resolves, and hands `Device&` to a callback — and move new procedures out to **node free functions over `DeviceManager&`** (the already-planned `startTrajectory(DeviceManager&, …)` shape), or to view methods invoked *through* that primitive. That keeps `DeviceManager`'s responsibility crisp ("own the devices; lend safe, locked access to them") and stops it naming profile types. This is the "general solution in the owning layer" move — the recurring need is *safe locked access by position*, so solve it once. **Not done yet, on purpose (YAGNI):** for a single operation matching the CiA402 precedent, one explicit per-operation method is more discoverable than a generic `Device&`-yielding callback (which is leakier and a template-in-header), so the primitive should *earn* its keep rather than be introduced speculatively. `runStoreParameters` stays a plain `DeviceManager` method today; this note is here so the extraction is a deliberate decision when the surface actually starts to hurt, not an accident.

---

## Session 2026-07-24 — Off-loopback deployment, take two: dnsBOX access makes the wildcard cert real; the static-record path (A) unblocks a Pi now, the responder path (B) is the end goal (plan)

**Extends *Session 2026-06-12 — Remote/LAN deployment*** with two things that session lacked: (1) confirmed **dnsBOX DNS-management access** over the `synapticon.com` zone (so the DNS pieces are now self-serve, not a request to someone else), and (2) an explicit **end goal** — a user runs the Motion Master server on *any* PC on their network and drives it from the hosted console PWA with zero cert friction, not just the Raspberry Pi appliance. The 2026-06-12 session framed the wildcard-over-IP-subdomain scheme; this one splits it into two concrete, independently-shippable paths and records what changes in *this repo* versus what is pure DNS/infra. A Pi lands in ~2 weeks (early Aug 2026); the near-term job is to get *it* working without blocking on the full product path.

**Why the localhost cert breaks off-loopback — one-line recap.** The bundled Let's Encrypt cert is for `local.motion-master.synapticon.com`, a public A record pinned to `127.0.0.1`; it works *only* because server and browser share a machine. Any server the PWA reaches by network address fails on name mismatch, can't use a public cert for a private IP, and can't fall back to self-signed because the PWA's cross-origin `fetch()` gives no browser click-through (the same constraint that forced cert self-heal into the binary — Session 2026-06-06). The fix in both paths below is the same shape as the localhost trick: a **real, publicly-trusted cert for a real hostname that happens to resolve to a private/LAN address**, served locally by the binary.

**The pivot dnsBOX access exposes: static records vs. a synthesizing responder — a *choice*, not one mandatory build.** The 2026-06-12 session assumed the sslip.io responder was the price of admission. It is not, for a fleet you own. That responder exists to *synthesize* `A` answers for **arbitrary, unregistered** IPs (`192-168-1-50.ip.… → A 192.168.1.50`) — the value it adds is zero-touch for addresses you'll never know in advance. With DNS control and a *known* device, a plain static record does the same job with no software to run. So:

- **Path A — static A records (dnsBOX only, no responder software). Unblocks the Pi now.**
  Add a per-device record under a subdomain you own, e.g. `pi-lab-1.ip.motion-master.synapticon.com A 192.168.1.50`. A public authoritative server returning an RFC1918 address is fine and already precedented (`local.… → 127.0.0.1`). A wildcard cert `*.ip.motion-master.synapticon.com` covers `pi-lab-1.ip.…` exactly as well as the dashed-IP form, so **the client IP→hostname transform is not needed on this path** — the PWA host field is pointed straight at `pi-lab-1.ip.motion-master.synapticon.com`. Cost: one manual record per device, plus a DHCP reservation (or static lease) so the address doesn't wander. Right for internal rigs and a small known set of machines. **This is the two-week plan.**

- **Path B — sslip.io-style synthesizing responder (the end goal). Zero-touch, any PC, any address.**
  Delegate `ip.motion-master.synapticon.com` (NS records in dnsBOX) to an authoritative responder that parses the leftmost label into an `A` answer (sslip.io is open-source Go, effectively one file). Now *any* user on *any* network points the console at their server's IP and it just works — no record to pre-create, which is the actual product experience wanted ("run the server on any PC, use the console, no problem"). Same **one** wildcard cert covers every address, sidestepping the Let's Encrypt 50-certs/domain/week limit entirely. This is where the client-side **IP→dashed-hostname transform** lands (`192.168.1.50` → `192-168-1-50.ip.motion-master.synapticon.com`), because now the human types a bare IP and the client must build the cert-matching name. **Worth checking first:** some dnsBOX backends (PowerDNS w/ Lua records or the pipe backend) can synthesize label-derived answers natively — if so, Path B needs *no* separate responder host, just a scripted zone. Confirm the appliance's capability before standing up sslip.io as separate infra.

**A is a strict subset of B — no throwaway work.** Both paths need the *same* wildcard cert and the *same* binary change; A simply omits the responder and the client transform. Doing A first and B later re-uses everything A built. The only A-specific artifact is the handful of static records, which coexist harmlessly with a later responder (an explicit `A` record shadows the wildcard delegation for that one name, or is deleted).

**What is already in place (don't rebuild it).**

- **Client host-change UI exists.** `web/apps/console/src/pages/ConnectionPage.tsx` already edits host + HTTP/WS ports and persists them to `localStorage` via `ConnectionContext`; `@synapticon/motion-master-client` (`constants.ts`, `client.ts`) already takes overridable API/WS URLs. Pointing the PWA at another backend is *done*. Path A needs **no** client code — only the existing field, filled with the `.ip.…` hostname. Path B adds the transform + a LAN-mode copy variant (today's explanatory text is entirely localhost-framed).
- **The binary serves whatever `cert.pem`/`key.pem` are on disk.** Baking a wildcard cert into an image (or dropping it next to the binary) needs **zero** serving-side change. `tools/run.sh`'s discovery order and the `POST /api/init` TLS bind don't care about the cert's name.

**What changes in this repo (small, and shared by A and B).**

1. **Binary: two hard-coded localhost assumptions in the cert self-heal must become config-driven.** `apps/motion_master/cert_updater.cc` hard-codes `kCertCommonName = "local.motion-master.synapticon.com"` (line ~28) and *rejects* any downloaded cert whose CN differs (`validatePair`, line ~98) — so a server expected to serve the wildcard would fail its own self-heal validation. Promote the **expected CN** and the **default fetch URL** (`mm::defaultCertUrl()`, already overridable via `--cert-url`/`--key-url` and surfaced in `options.cc`) into config, defaulting to today's localhost values; an image/host serving the wildcard overrides both (expected CN `*.ip.motion-master.synapticon.com`, fetch URL → the wildcard release asset). Note the CN check is a *convenience* identity check, not a security control (TLS trust comes from the CA chain the browser validates) — but keep it honest by matching against configured expectation rather than loosening it to "anything". ~½ day.
2. **CI: a wildcard issuance leg.** `cert-renewal.yml` already issues `local.…` via acme.sh + `dns_acmedns` (DNS-01, the only challenge type that issues wildcards). Add a parallel `--issue -d '*.ip.motion-master.synapticon.com'` that needs a **second acme-dns CNAME** — `_acme-challenge.ip.motion-master.synapticon.com` → a new acme-dns subdomain — which **dnsBOX access now lets us add ourselves**. Publish the wildcard cert/key to a **separate** rolling release asset (parallel to the `tls-cert` release, e.g. `tls-cert-ip`) so the two certs rotate independently and a host fetches exactly the one it serves. Reuse acme-dns rather than switching acme.sh to a dnsBOX-native DNS-01 plugin — less churn, the machinery is proven. ~½ day.
3. **Client (Path B only): IP→dashed-hostname transform + LAN-mode copy.** A pure helper (`192.168.1.50` → `192-168-1-50.ip.motion-master.synapticon.com`) plus a branch in `ConnectionPage` and a second explanatory-copy variant. Purely additive. ~½–1 day. **Skipped for Path A / the Pi.**
4. **Packaging: an arm64 `.deb` leg (Pi-specific, not cert-related).** `build-linux-arm64.yml` builds the arm64 *binary*; `release.yml`/`tools/package.sh` are x64-only. A full flashable Pi image is a further, larger step. **Sidestep for first bring-up:** grab the CI arm64 binary, `scp` it onto Raspberry Pi OS Lite (64-bit, Bookworm — it *is* Debian, so the existing `setcap` postinst logic applies), `setcap` by hand. Proper arm64 `.deb` + image leg is the follow-up once the Pi is proven. ~1–2 days for the `.deb` leg.

**DNS-side threat posture — unchanged, still fine.** Both paths return private/LAN addresses from public DNS (already true for `local.… → 127.0.0.1`) and ship a private key in the image/host (Path B). The wildcard key only authenticates `*.ip.motion-master.synapticon.com`, whose names always resolve to a LAN address an attacker must already be on-path for — identical exposure to the localhost keypair already published on the `tls-cert` rolling release. Not a regression.

**Discovery is still a separate problem from the cert (unchanged from 2026-06-12).** Even Path B leaves the user needing to *know* the server's IP to type it. Ladder by effort, later: manual IP entry → mDNS/Avahi (`motion-master.local`, then connect via the `.ip.…` name so the cert still matches). Out of scope here.

**Phasing.**

- **Now → ~2 weeks (Pi bring-up, Path A):** add the second acme-dns CNAME in dnsBOX; add the wildcard issuance leg (#2); make the binary CN/URL config-driven (#1); add one static `A` record for the Pi + a DHCP reservation; hand-deploy the arm64 binary. No client change, no responder, no image build. Gets a real Pi answering the console over TLS.
- **Then (Path B, the product path):** stand up the synthesizing responder (or a dnsBOX-native scripted zone if the appliance supports it) under `ip.…`; add the client IP→hostname transform + LAN-mode UI (#3). Now any PC running the server is reachable from the console with nothing to pre-register — the stated end goal.
- **Parallel track:** arm64 `.deb` + flashable image leg (#4) for the appliance form factor.

**Rejected / escape hatch (from 2026-06-12, still the fallback):** self-signed + click-through is dead (cross-origin `fetch()`); a private CA users import is a per-customer support burden; `.local` + public cert is impossible (reserved TLD). If the DNS machinery ever feels too heavy, **serve the PWA from the server itself (same-origin)** — same-origin navigation *does* allow a one-time self-signed exception, dropping all wildcard/DNS plumbing. Not chosen (the hosted+acme path is already built and the wildcard is cheap given dnsBOX access), but it remains the clean bail-out.

**Status:** plan only; no code yet. Prereqs owned outside the repo (DNS records, acme-dns subdomain) are now self-serve via dnsBOX.

## Session 2026-07-28 — aarch64 Linux joins the release: build *on* Debian 13, not on the arm64 runner's Ubuntu (as-built)

Closes item #4 of *Session 2026-07-24* (the arm64 packaging leg) except for the flashable image. `build-linux-arm64.yml` already proved the arm64 *build*; what was missing was a release leg that ships it. The release now has four build legs, and the aarch64 one produces the same three Linux artefacts the x64 leg does — `-linux-arm64.tar.gz`, `-arm64.deb`, `-aarch64.rpm`.

**The one real decision: which libc the artefact links, i.e. which machine builds it.** GitHub's `ubuntu-24.04-arm` runner is Ubuntu (glibc 2.39); the deployment target is *Debian* aarch64 (Raspberry Pi OS). Three candidates:

- **Build on the runner (Ubuntu 24.04, glibc 2.39).** Assumed at the time to be the widest of the three — but see the measurement below: it makes no difference. The binary is built against a distro we don't ship for, and nothing pins that alignment as the Ubuntu runner image moves.
- **Build in a `debian:12` (bookworm, glibc 2.36) container.** Best coverage — bookworm is still the majority of Raspberry Pi OS installs — but bookworm's default gcc-12 has no `std::expected`, so it needs a backported or hand-built toolchain in the container. Rejected for now as build plumbing with real risk; revisit if bookworm devices actually need packages.
- **Build in a `debian:13` (trixie) container — chosen.** Trixie ships gcc-14 natively, so the container is a plain `apt-get install` and the artefact links exactly the libc/libstdc++ of the distro it targets.

**The assumed trade turned out not to exist — and the correction is the most useful thing in this entry.** The leg was chosen, and initially documented, on the belief that building in trixie raises the runtime requirement to trixie's glibc (2.41), narrowing coverage versus the Ubuntu runner's 2.39. **That is wrong: a binary's floor is the highest symbol version it actually references, not the build host's libc version.** Measured on the published artefacts with `readelf -V`, both the Ubuntu-24.04-built x64 binary and the trixie-built arm64 binary require at most **`GLIBC_2.38`** and **`GLIBCXX_3.4.32`** (libstdc++ from GCC 13.2), and link only `libc`, `libm`, `libstdc++`, `libgcc_s`. The container choice therefore costs **nothing** in coverage: both artefacts run on glibc ≥ 2.38 (Ubuntu 24.04, Debian 13, Raspberry Pi OS trixie), and Debian 12 (2.36) was always out of reach for either build host. README's *Prerequisites* now states the measured 2.38 requirement for both architectures and shows the `readelf -V` one-liner; the pre-correction claim of a 2.41 arm64 floor (and the 2.39 x64 claim it sat next to) was never true of the shipped binaries. **Rule to carry forward: never quote the build container's distro version as the compatibility floor — measure the binary.**

**Two smaller things worth not re-deriving.**

1. **`tools/package.sh` is arch-aware by preset, not by host.** A `case` maps the preset to the pair of names deb and rpm use for the same machine (`x64-linux-*` → `amd64`/`x86_64`, `arm64-linux-*` → `arm64`/`aarch64`); `Architecture:`/`BuildArch:` and both output filenames come from it, and `rpmbuild --target` names the arch explicitly so an `arm64-linux-cross-*` preset can package an aarch64 rpm from an x86_64 machine. An earlier draft added a `[format]` argument so the arm leg could build the deb alone — deleted once the leg shipped all three formats: an rpm is the same binary with different metadata, so the flag was speculative machinery.
2. **The aarch64 leg's vcpkg cache key carries a `debian13` token.** Keying it like `build-linux-arm64.yml` (`vcpkg-<arch>-<os>-<hash>`) would *look* like cache sharing but produce the opposite: those archives were built with Ubuntu's toolchain, whose ABI hash this container's compiler never matches, and — worse — the key already existing means `actions/cache` skips the save, so the Debian archives would never persist and every tag would rebuild every dependency from source.

**Status:** as-built and merged; unverified until the next `v*` tag runs the leg (nothing in it can be exercised locally). Still open for the Pi appliance: the flashable image leg.

## Session 2026-07-30 — RT thread setup hoisted to `libs/core`; `SCHED_RESET_ON_FORK`; and priority 80 is a *target-verification* item, not a settled number (as-built + deferred)

Prompted by a review of the `SCHED_FIFO` setup. Two suggestions came in; one was a real defect, one was insurance, and the third thing — the one the review was actually pointing at — turned out to be a measurement task with no code change owed today.

**The real defect was duplication, and it was in the instrument.** `setRealtimePriority()` existed twice with independently hardcoded `80`: file-local in `game_loop.cc`, and again in `hil/jitter_bench/main.cc`. These docs asserted the bench "calls the same routine" — it did not. That matters more than ordinary copy-paste because **`jitter_bench` is the tool you would use to answer the priority question**, and a measurement tool that has drifted from the thing it measures is worse than no tool: it reports confidently about a configuration that isn't shipping. Hoisted to `mm::core::setRealtimePriority()` (`libs/core/realtime.{h,cc}`), beside `CyclicTimer` and for the same reason the `CyclicTask` interface went there (Session 2026-07-14) — the layer that owns the concern owns the primitive. `kRtThreadPriority = 80` is now the single definition of the number, which is what makes any future retune one line instead of a grep.

**The routine reports; the caller logs.** `libs/core` has no spdlog dependency and should not grow one for this, so the shared function returns the existing `RtSetupResult { schedFifo, memLocked }` and says nothing. `GameLoop` emits its `spdlog::warn`s, the bench its `std::cerr` lines. The two warnings stay wrapped in `#ifndef _WIN32` / `#ifdef __linux__` at the *call site* — a false result means "didn't happen", and on a platform where the step doesn't exist that is not worth warning about (Windows would otherwise complain about both on every start). Incidental fix: the bench's unconditional `mlockall` would not have compiled on macOS (its CMake gate is only `if(NOT WIN32)`); the shared implementation's `__linux__` guard removes that trap.

**`SCHED_RESET_ON_FORK` — taken, but be honest that it is insurance and not a fix.** The flag makes a child forked from the RT thread start at `SCHED_OTHER`/nice 0 with the flag cleared. Current exposure is **nil**, not merely small: `fork()` clones only the calling thread, the game loop never forks, and the tree's only `fork()` is `mm::core::openInBrowser` (`platform.cc`) called from `main.cc` *before* `gameLoop.run()` — i.e. while the main thread is still `SCHED_OTHER` — with `system_info.cc`'s `popen` likewise off the RT thread. It was worth folding in anyway because it converts "no child inherits RT priority" from a fact about the current call ordering into a property of the call itself, and once there is exactly one copy of the routine it costs one `|=`.

Two details that would have bitten a naive application of it:

1. **It is Linux-only.** Darwin's `<sched.h>` does not define `SCHED_RESET_ON_FORK`, and the block is `#ifndef _WIN32` with macOS-arm64 a shipped release leg — a bare `SCHED_FIFO | SCHED_RESET_ON_FORK` breaks that build. Guarded with `#ifdef SCHED_RESET_ON_FORK`.
2. **The policy encoding was verified, not assumed** — a rejected policy would silently cost RT scheduling entirely on the target, which is a worse failure than the one being prevented. Unprivileged, `pthread_setschedparam` returns `EPERM` for both plain `SCHED_FIFO` and `SCHED_FIFO | SCHED_RESET_ON_FORK`, not `EINVAL`; the kernel validates the policy *before* the permission check, so `EPERM` on the OR'd form is positive evidence the encoding is accepted and only the capability is missing.

**Priority 80 vs the NIC's threaded IRQs: the concern is real, narrower than it sounds, and does not bite today.** The review raised the old worry (inherited from an earlier codebase that used priority 1 "to avoid preempting network threads") that an RT loop above the NIC's softirq threads starves the very traffic it depends on. Three things resolve it for now:

- **On a stock kernel it cannot happen.** Hardirqs and softirqs run in interrupt context and preempt `SCHED_FIFO` at *any* priority; `ksoftirqd` (`SCHED_OTHER`) only picks up overflow. Every current deployment target is a stock kernel, so the number cannot starve the NIC path there.
- **Under `CONFIG_PREEMPT_RT` the relation is real but conventional.** Threaded IRQs default to ~50 and NAPI runs in the IRQ thread, so the loop at 80 does outrank the interface. That is the normal arrangement, and 80 stays well clear of the 90+ band the kernel's own migration/watchdog threads occupy.
- **The pinned SOEM receive path blocks rather than spins**, which is what removes the livelock. `ecx_waitinframe_red` (`oshw/linux/nicdrv.c`) polls with `ppoll()` on a 50 µs timeout, so the loop yields the core while waiting for the frame; the starvation scenario needs a *spinning* consumer holding the CPU against a lower-priority IRQ thread, and this isn't one.
- **The old priority-1 rationale is not good guidance either** — 1 is below anything else RT on the box, which trades a hypothetical problem for a real one.

So: no change to the number, and nothing to decide until there is a PREEMPT_RT host to measure on. Recorded instead as a checklist line for that work — **confirm 80 against the target's threaded-IRQ priorities (`ps -eo pid,cls,rtprio,comm | grep irq`) before DC SYNC0 activation relies on it** — alongside the DC-locked cycle timer, since SYNC0 is where actuation stops depending on frame arrival and these priorities start to matter (Session 2026-07-09 and the SYNC0 item of Session 2026-06-01). Pinning the RT thread and the NIC IRQ thread to an isolated core is the companion measure, and `jitter_bench` — now genuinely measuring the shipped configuration — is the instrument.

**Status:** hoist, `SCHED_RESET_ON_FORK`, and the doc corrections are as-built and merged (274/274 tests, format/cppcheck/lint clean). Priority 80 is deferred to the PREEMPT_RT/SYNC0 session as a measurement, not a code change.

---

## Session 2026-07-31 — Off-loopback deployment, as-built: one certificate with two SANs, a `bindAddress` knob, and the client-side IP→hostname map

Implements *Session 2026-07-24* (which extended *2026-06-12*), with one design change that made the whole thing smaller than either plan predicted. The end goal is unchanged: run the server on a machine that is not the browser's — a Raspberry Pi wired to the drives — and drive it from the hosted Console with no certificate friction.

**The change: one certificate carrying both names, not two certificates.** Both prior sessions assumed a *second* certificate for the wildcard, published to a second rolling release (`tls-cert-ip`), selected per host by promoting the expected CN and the fetch URLs into config (`tls.commonName` / `certUrl` / `keyUrl`). That was the wrong shape, and the question that exposed it was simply *"why keep `local.…` if the wildcard covers `127.0.0.1`?"* — the answer being that we should keep the name but stop treating the two deployments as two certificates. A single Let's Encrypt issuance carries both `local.motion-master.synapticon.com` and `*.ip.motion-master.synapticon.com` as SANs, so:

- **Every host serves the same file.** There is nothing per-deployment to configure — no expected-CN knob, no per-host fetch URL, no second rolling release, and no change to the `Dockerfile` / `release.yml` cert bakes. Three planned config keys evaporated.
- **One renewal, one timeline.** The monthly `cert-renewal.yml` run rotates both names at once; a host cannot end up with the "wrong" certificate for how it happens to be reached.
- **The desktop install keeps its independence.** Folding localhost into the wildcard as `127-0-0-1.ip.…` would have been *simpler still* — one name, no exception — but it makes every ordinary desktop install depend on the `ip.…` responder resolving. `local.…` stays a plain static record in the parent zone (served by dnsBOX's own nameservers), so a responder outage cannot break the common case. That is the sole reason the second name survives; it is not sentiment about the existing name.

**What that cost: `validatePair` had to stop comparing the CN.** `cert_updater.cc` compared the subject CN to one constant and rejected anything else, which a two-SAN certificate fails. It now calls `X509_check_host` once per required name — the same matcher a browser uses, including RFC 6125 wildcard semantics — so the binary accepts exactly what the client will accept, rather than second-guessing it with a string compare. The wildcard is verified by probing a concrete name below it (`127-0-0-1.ip.…`, chosen because the responder maps it back to loopback, so the probe is a name that genuinely resolves). `cert-renewal.yml` runs the same check with `openssl` before publishing: every host self-heals from that release, so a certificate missing a SAN would be pushed everywhere and refused everywhere — cheaper to catch at issuance.

**The blocker neither plan listed: both servers hard-bound loopback.** `http_server.cc` and `ws_server.cc` passed a literal `"127.0.0.1"` to `.listen()`, so no amount of certificate work would have made a Pi reachable. Now `server.bindAddress` (default `"127.0.0.1"`) threads into both `Config`s. It is validated non-empty at the config root, because uWebSockets reads an empty host as *every interface* — an omitted-but-present key would otherwise silently expose an unauthenticated server to the network. Binding all interfaces has to be spelled `"0.0.0.0"`, deliberately. The 2026-07-24 note that "the binary serves whatever `cert.pem` is on disk ⇒ **zero** serving-side change" was true about TLS and misleading about deployment; worth remembering that a cert-shaped problem had a socket-shaped blocker sitting behind it.

**Client side.** `lanHostname()` in the SDK (`web/packages/motion-master-client/src/lan.ts`) maps `192.168.1.50` → `192-168-1-50.ip.motion-master.synapticon.com`, passing non-IPv4 input through untouched, so a user who types a hostname (or the dashed name itself) is left alone. `ConnectionPage` applies it on commit and previews the resulting name while typing. `GET /api/cert` gained `dnsNames` (the leaf's SAN list) and the page renders it as **Valid for** — with one certificate now covering two very different deployments, "is the host I typed covered?" is the first diagnostic question, and the CN cannot answer it.

**Path A (static per-device `A` records) is abandoned, not deferred.** The 2026-07-24 session framed it as the near-term step and Path B as the end goal. That ordering does not survive contact with the actual product: a Raspberry Pi ships as a **flashable image** and joins the customer's WiFi over DHCP, so nobody — not us, not the customer — knows its address in advance, and there is no record anyone could have created. A static record only ever worked for a machine whose address we control, which is not the deployment. So the responder is the *only* path, and the choice of where it runs is now the critical-path item rather than a later refinement. Confirmed while checking: **dnsBOX cannot host the synthesis** — its nameservers are PowerDNS (`version.bind`), whose `LUA` + `createForward()` would do it natively, but the zone editor offers only A/AAAA/CAA/CNAME/HINFO/MX/NAPTR/NS/PTR/SRV/TXT/ALIAS, and `enable-lua-records` is operator-controlled and off by default on shared infrastructure. Worth recording for whoever revisits this: `createForward()` parses **dotted and hex** IPv4 labels but *not* dashed ones (dashes are IPv6-only, `createForward6()`), so a PowerDNS-hosted version would have had to use hex labels (`ipc0a80132.ip.…`) — a dotted address is four labels deep and a wildcard covers exactly one. Since we are delegating to CoreDNS instead, the dashed format stands and `lanHostname()` needs no change.

**The responder is gone: the `ip.…` names are resolved by a hosts entry on the client, and that is the design — not a fallback.** This session first went down the Path B road: delegate `ip.motion-master.synapticon.com` and run a synthesizing responder so any address resolves with nothing to pre-register. A CoreDNS config (`template` plugin, restricted to RFC1918 + loopback, with an `_acme-challenge` CNAME in a companion zone file) was written and verified against CoreDNS 1.12.0 — all three private ranges, the `172.16`/`172.31` boundaries, public-address rejection, `NODATA` on AAAA, TCP — and then **deleted**, because the requirement it served was not real. Recorded so it is not rediscovered as an oversight:

- **It buys convenience, not capability.** Users are willing to edit `/etc/hosts` (stated outright), and TLS validates a certificate against the *name*, never against how that name was resolved — so a hosts entry produces exactly the same trusted, warning-free connection a public `A` record would. The responder's only real benefit was avoiding that one line, on clients that cannot have it (phones, tablets, locked-down IT).
- **What it cost was disproportionate.** A public authoritative nameserver to operate, an availability commitment where our downtime means no customer can reach their own device, an NS delegation, and — because the certificate's private key ships in every public release — an **abuse surface**: anyone could point `<their-ip>.ip.motion-master.synapticon.com` at their own host and serve trusted HTTPS under a `synapticon.com` name. That is why sslip.io ships `-public=false`, and why the deleted config restricted itself to private ranges. Not publishing the names at all removes the problem outright rather than mitigating it.
- **It would not even have been sufficient.** Many resolvers drop private addresses returned from public domains (`dnsmasq --stop-dns-rebind`, default on OpenWrt), and a site with no internet cannot query us at all — so the hosts-file path had to exist regardless. Building both meant paying for the responder *and* still shipping the fallback.
- **dnsBOX could not have hosted it anyway** (details above), so the responder always implied separate infrastructure.

What remains outside the repo is therefore **one static record, once, ever**: `_acme-challenge.ip.motion-master` CNAME to the **existing** acme-dns subdomain, so the wildcard can be issued over DNS-01. No `A` records, no delegation, nothing to operate. That account keeps two rolling TXT records, this issuance needs exactly two, and that is also the ceiling — a third SAN would evict one and require a second account.

**Deferred, deliberately.** Discovery is still a separate problem from the certificate: the Pi will advertise itself over mDNS/Avahi (configured on the device, not shipped in the packages) so a user can find its address, then type it into the Console. Shipping an Avahi service file in the deb/rpm was considered and dropped for now — it would make every Linux install advertise itself, which is a bigger decision than this change. A flashable Pi image remains the open item from 2026-06-12.

**Decision: stay on Let's Encrypt, and accept two manual steps for a remote machine.** A full-offline requirement landed late in this session (an air-gapped machine network is a normal industrial deployment). The PWA half already holds — `vite-plugin-pwa` precaches the app shell, so the Console loads with no network — and the hosts-file mechanism above needs no DNS, so *resolution* is fine offline. The certificate is the part that cannot be solved, and the conclusion is that it does not need to be:

- **No CA can issue a long-lived certificate**, so there is nothing to shop for. Public maximums are 398 days today, dropping to 200 (Mar 2026), 100 (Mar 2027) and 47 (Mar 2029). A private CA or self-signed cert does not escape it either: Apple caps *every* TLS server certificate at **825 days**, including privately-issued and self-signed ones (measured — 800 days works, 1592 does not). The better-known 398-day rule exempts user-added roots; the 825-day rule does not. So ~2¼ years is the hard ceiling anywhere, and only with a root installed on every client.
- **So the two accepted manual steps for a remote install are:** one **hosts-file line** on each client machine (the name is not in public DNS by design), and **keeping `cert.pem`/`key.pem` up to date** on the server — automatic via the startup self-heal wherever there is internet, a periodic file copy where there is not.
- **Rejected: a private CA.** It would stretch the copy interval from ~90 days to ~2 years, and costs CA key custody plus a trust-store install on every client — impossible on locked-down IT and on phones/tablets. Against the "as simple as possible" mandate that is a bad trade; it can be added later without disturbing anything built here if the interval ever becomes a real complaint.
- **Rejected for now: serving the Console from the device** (same-origin + long-lived self-signed, which needs no DNS and no renewal and works air-gapped). It is the strongest offline answer on paper, but it costs a one-time browser warning per device, static-asset serving in the binary, and the API and WebSocket collapsed onto one port — browsers key certificate exceptions by host *and port*, and the WebSocket connection cannot prompt for one. Kept on the shelf as the escape hatch it has been since 2026-06-12.

**Status:** as-built for everything in this repo (283/283 tests, cppcheck/lint/format clean, console typechecks). Not yet exercised end to end — that waits on the one `_acme-challenge` CNAME, a certificate reissued with the wildcard SAN, and a Pi on the bench.

## Session 2026-08-02 — Procedure progress is poll-only: drop the WebSocket push, retain the last run per device in memory, and let `runCount` stand in for a run id (design)

**Supersedes** the delivery half of *Session 2026-07-19* (`ProcedureManager` … `ProgressStep[]` snapshot). Everything that session decided about *structure* stands unchanged — the three tiers (`ProfileDevice::runOsCommand` as generic mechanism, typed measurements on `SomanetDrive`, `runOffsetDetection(SomanetDrive&, ProgressReporter&, std::stop_token)` as a transport-free free function), `ProcedureManager` owning the cancellable `std::jthread`s and the per-device exclusive activity token, and progress as a full-array accumulating snapshot. What changes is that **the WebSocket push is dropped entirely**: a client learns about a procedure by polling one `GET`, and nothing else.

**The push was never load-bearing, and the earlier session had already proved it.** 2026-07-19 argued at length that polling the companion `GET` alone is a *fully supported, lossless* mode, because each notification is a full-array snapshot in which every step retains its terminal `succeeded`/`failed` status and its measured `value` — an accumulating state, not a discrete-event feed. A poller therefore cannot miss a *result*; the only thing it skips is the transient `running` blip on a fast step, which carries no data. Having established that the pull path is complete on its own, keeping the push path meant maintaining a second delivery mechanism for a latency improvement nobody needs on a procedure whose individual steps take seconds. So it goes.

**What that deletes.** `ProcedureManager` holds **no publish callback** — no `std::function<void(topic, json)>` member, no `setPublish`, no wiring in `main.cc` (contrast `MonitoringManager`, which keeps its seam because a monitoring stream genuinely is a high-rate feed). The RT-vs-off-RT transport split from 2026-07-19 ("RT sources → `NotificationBus` poll pump; off-RT procedures → direct publish through the identical seam") collapses to just the first half: the `NotificationBus` remains for RT producers that cannot call out, and procedures are simply not a WebSocket producer at all. The *message type* open item (reuse `notification` vs a dedicated `procedure` type) is moot. The late-joiner/replay reasoning that justified full-snapshot-over-delta is still architecturally true but no longer carries any weight — the snapshot shape is now justified by the poll path alone.

**The surface is one resource with three verbs.** The addressed thing is the pair `(devicePosition, procedureName)` — `offset-detection` *is* the procedure identifier — and there is deliberately **no run id in the path**, because only the latest run per device+procedure is retained and there is nothing else to address. It is a singleton sub-resource keyed by name, not a collection of runs:

```text
POST   /api/devices/:pos/procedures/offset-detection   → 202 accepted | 409 busy
GET    /api/devices/:pos/procedures/offset-detection   → the snapshot (below)
DELETE /api/devices/:pos/procedures/offset-detection   → request the jthread's stop_token
```

`DELETE` means **cancel the running procedure**, not *delete the record*: the retained snapshot stays behind with `status: "cancelled"`. Clearing retained state is not a user need — the next run overwrites it and a rescan drops it — so nothing addresses that operation. (`POST …/cancel` was the alternative if the verb read ambiguously against a resource that persists; it does not, because the thing being deleted is the *run*, not its result.)

**The snapshot, with two additions the poll-only model requires and one it earns.**

```jsonc
{
  "status": "succeeded",          // idle | running | succeeded | failed | cancelled
  "runCount": 3,
  "startedAt": 1735821000123,     // epoch ms; absent while idle / never run
  "finishedAt": 1735821042456,    // epoch ms; absent while running
  "steps": [ { "id": "…", "status": "succeeded", "value": 1234 }, … ]
}
```

- **Overall `status`** was already required by 2026-07-19 so that "is it done?" is a single-field check. It gains **`cancelled`** as a fifth value — *overall only*; the per-step `ProgressStatus` stays `Idle/Running/Succeeded/Failed`. Folding a user cancel into `failed` would lose the distinction between "the drive could not measure" and "I stopped it", which is exactly the distinction a returning user needs.
- **Retention after the thread exits** was likewise already required. It is now the feature rather than a correctness footnote: a user who navigates back to the Offset Detection page sees the result of the last run **for that session**, with no run in flight and no polling.
- **`startedAt`/`finishedAt`.** Without a push channel there is no temporal cue that the displayed result is ten minutes old, and a stale commutation offset presented as current is actively misleading. The timestamps are what make the returning-user view honest.
- **`runCount` earns more than a UI label.** With no run id, a poller cannot otherwise distinguish one run from another: a second run started from another tab that begins *and* completes between two polls is invisible, and the client would read a different run's result as the one it was watching. `runCount` is a cheap per-device generation counter that closes that gap — if it changed since the last poll, this is a different run — and `(devicePosition, procedureName, runCount)` is a stable run identifier should one ever be needed, with no id allocation. It increments on each **accepted** start (a 409 does not count).

Never-run is well-formed too — `status: "idle"`, `runCount: 0`, no timestamps, every step `idle` from the per-procedure template — so the client renders one component with no empty/absent special case. The client loop is `while (status === 'running') { sleep; GET; }`, and a page mount that finds anything else polls zero times.

**Retained snapshots are cleared on `scan()`/`reset()`.** Retention is keyed by slave position, and a rescan rebuilds `DeviceManager::devices_` with positions that may remap — a retained "offset = 1234" would then be rendered against a *different physical drive*. Keying by device identity (serial / vendor+product+serial) so results survive a rescan is more faithful but more machinery for a value that is explicitly session-scoped; a missing result is strictly better than a wrong-drive result, so the snapshots (and `runCount`) reset on rescan.

**What is unchanged.** `ProgressReporter` still exists and still owns the step-mutation vocabulary — it simply updates the retained array in place instead of also calling out, which is what keeps `runOffsetDetection` transport-free and testable against a fake reporter. The busy-set is still the span-level exclusion `controlPlaneMutex_` cannot provide (that mutex serializes individual transactions; a procedure is a multi-second span of many, interleaved with sleeps). Cancellation is still checked *between* steps, with an in-flight OS command allowed to finish and the drive returned to a safe state before exit. The `value`-stays-numeric and client-owns-labels leans stand. And the standing restraint stands: no procedure registry/dispatcher until a second procedure actually exists.

## Session 2026-08-02 — Every OS command is a procedure: one `/procedures` surface, a descriptor catalogue, and composition below the manager (design)

**Extends** *Session 2026-07-19* and the poll-only note above. The question that opened it: should a raw OS command get its own endpoint? The answer generalises — **a single OS command already *is* a procedure**, and treating it as one collapses what would otherwise be a second API dialect.

**The structural argument.** A single OS command is a multi-second command-and-wait span on one device, cancellable, reporting progress, needing exclusion for its whole duration. That is the definition `ProcedureManager` was built around. Every piece of it — the cancellable `std::jthread`, the per-device busy token, the retained snapshot, `runCount`, the timestamps — is exactly what running one raw OS command needs, with **no machinery to add, only a body to supply**. Offset detection is then not a different *kind* of thing but a composite one: several OS commands under a single span. So the surface is uniform for both:

```text
POST   /api/devices/:pos/procedures/<name>   → 202 accepted | 409 busy
GET    /api/devices/:pos/procedures/<name>   → the snapshot
DELETE /api/devices/:pos/procedures/<name>   → cancel
GET    /api/devices/:pos/procedures          → every procedure + descriptor + last-run snapshot
```

The raw escape hatch is `procedures/os-command` (body: the eight command bytes + timeout), **not** a sibling `/api/devices/:pos/os-command` — one collection, one set of verbs, addressed by procedure name. Both it and the typed commands stay: the raw one is for bring-up and for commands not yet wrapped (the previous client carried the same idea as its `CUSTOM_OS_COMMAND`), while a named procedure adds parameter validation, error-code naming, and a decoded typed result.

**The list endpoint is what makes a UI possible.** With ~23 commands a per-device *Procedures* page cannot issue 23 requests or hard-code 23 descriptions. `GET .../procedures` returns the catalogue — each procedure's descriptor plus its last-run snapshot — so the page renders in one request and stays generic as commands are added.

**Two earlier decisions this voids, both of them conditional when made:**

- *The registry restraint.* 2026-07-19 said "resist a procedure registry until a second procedure actually exists". That condition is now met many times over. But the answer is **not** 23 hand-written bodies: most OS commands are a single call, so they are served by **one generic body parameterised by (command bytes, response decoder, step template)**; only genuinely multi-step procedures (offset detection) get a bespoke body. The registry is a table of descriptors, not a pile of near-duplicate functions.
- *Client-owned labels.* Leaning the labels and formatting onto the client was free with one procedure and is a liability with ~23: a client-side catalogue must be kept in sync with the server's procedure set by hand. The **server owns a descriptor** per procedure — title, description, caveats, whether it moves the motor, whether it requires the drive enabled, the step template. The house rule that every action control carries a description *and its caveats* means that text exists regardless; duplicating it in the console is how it goes stale. (Per-step *labels* may still be client-side; the per-procedure descriptor is not.)

**Progress stays logged and dropped — deliberately, and this is the one place the "single OS command is a procedure" analogy does not pay off.** The temptation was obvious: a single-command procedure has exactly one step, so the firmware's 0-100 % (status band 100-200) would be that step's progress bar, and `ProgressStep` would gain a `progress` field with `OsCommandConfig` regaining the callback `runOsCommand` deliberately left out. Not worth it. The firmware's percentage reporting is thin — the specification itself makes it optional (255 means "executing, percentage not supported") — and in practice the great majority of OS commands are run-and-complete, finishing in one or two polls with no meaningful intermediate state to show. Plumbing a field that is empty for almost every procedure, to drive a progress bar that would almost never move, buys a worse API for a rarer case. `runOsCommand` keeps logging the percentage at debug level, which is the right home for something only useful while watching a specific command. Revisit only if a genuinely long command with trustworthy percentage reporting turns up (system identification is the plausible candidate) — and then as a field added for that command, not as a change to the general shape.

**The trap, recorded before it is built: procedures compose as functions, never as procedures.** Offset detection runs several OS commands. The tidy-looking implementation has it call `ProcedureManager::start("phase-resistance")` for each — which tries to acquire a device token **the parent already holds**, and self-deadlocks (or spuriously 409s against itself). The rule is that a procedure body calls `drive.runOsCommand(...)` **directly**; the token is acquired exactly once, by the outermost thing the *user* started, and composition happens below the manager in plain calls on the view. This is the same layering that keeps the body a free function over a reporter.

**The accepted cost.** Some OS commands are instant and read-like — skipped-cycles counter (13), read object dictionary (21), use-internal-encoder-velocity (18, a setter). Wrapping a ~10 ms operation in start/poll/cancel is ceremony. Taken deliberately: an instant procedure is simply one whose first `GET` already reads `succeeded`, and the alternative is two API shapes plus a per-command judgement about which one each command earns. Uniformity is worth more than a saved round trip — but it is a trade, not a free win.

**"Every OS command is a procedure" does not invert — and firmware installation is the proof.** There are three of them, on three different transports, and all three are procedures:

- **Drive firmware — no OS commands at all.** Transition the device to BOOT, write two files over FoE, optionally write the SII, transition back to PRE-OP. The generic OS-command body does not serve this one; its body is state transitions plus FoE.
- **SMM firmware — FoE *and* OS commands.** The safety module is updated through a combination of file transfer and the 11.x SMM acyclic handler commands (11.9 software update login, 11.10 configure, 11.11 transmit, 11.12 finalize).
- **Kübler encoder firmware — OS commands only** (17.2 switch to bootloader, 17.3 check update size, 17.4 erase, 17.5 write, 17.6 exit bootloader). A different artefact reached *through* the drive's mailbox, not the drive's own firmware — easy to confuse with the first, and the two are unrelated.

That spread is the argument, not a complication: **a procedure is classified by its lifecycle — off-RT, multi-second, multi-step, cancellable, exclusive on one device — never by the transport its body happens to use.** All three want the same `ProgressStep[]` snapshot, the same busy token, the same run/poll/cancel surface, while their bodies share almost no code. It is also why 2026-07-19 was right to retire the standalone `FirmwareInstaller`: there was never one firmware installer to build.

**Sequencing — validate the machinery before the volume:**

1. `ProcedureManager` core with raw `os-command` as its **only** procedure. The ideal first one: no per-command knowledge, exercises run/poll/cancel/retain/busy-set end to end, immediately useful on the bench.
2. The catalogue — descriptors and `GET .../procedures`.
3. Typed commands one at a time, `phase-resistance-measurement` first (the smallest thing that proves the decoder and error-naming layer).
4. Offset detection — proves composition.
5. The console *Procedures* page, once the catalogue lets it be generic. With ~23 commands it will want grouping: Measurement, Calibration, Encoder, Diagnostics, and an Advanced group holding the raw command.

Two corrections to *Session 2026-07-19* fall out of the OS command work and belong in its as-built note rather than as retro-edits: its **three tiers are two** (the mechanism is as vendor-specific as the commands — see the `runOsCommand` placement rationale), and its lean that **cancellation can only happen between steps is wrong** — `0x1024 = 3` aborts an in-flight command, so a 30 s measurement stops within one poll interval.

## Session 2026-08-02 — `DeviceManager` is profile-ignorant: `withDevice` lends locked access, and each profile owns an `X_control.h` (decision)

The trigger was `ProcedureManager` needing to hold a `SomanetDrive&` for a thirty-second span. A borrowed `Device&` is valid only while `busMutex_` is held, and that mutex is **private** — so *anything* wanting to operate on a device for the length of an operation had to be added to `DeviceManager` as another method. Six already had been (`getCia402Status`, `setCia402OperationMode`, `runCia402Command`, `setCia402Target`, `runStoreParameters`, `runRestoreDefaultParameters`), and the next wave is ~23 procedures plus a SOMANET set. *Session 2026-07-24* had predicted this and set the trigger at "~3+ more piling up"; it is met twice over.

**`DeviceManager::withDevice(pos, fn)` — lend locked access instead of adding a verb.** It takes the shared lock, resolves the position, and hands `Device&` to a callable returning `std::expected<T, std::string>`. That is the whole primitive, and it is what makes the rest possible: code outside the class can now hold a device safely without the mutex ever leaving it. The lock is **shared**, so borrowers and the position-based read/write methods run concurrently and only `scan`/`reset` wait — and holding it across a multi-second operation is *correct rather than merely tolerable*, because a rescan midway through would invalidate the very device being worked on. Its one hazard is documented on the method: `fn` must not re-enter an exclusive `DeviceManager` operation, since `busMutex_` is not recursive.

**The layering rule this establishes, which is the part worth remembering:**

> **`Device` and `DeviceManager` are the base — one slave, and the set of slaves plus the driver, scanning, process data, and lending locked access. They know nothing about profiles. Everything profile-shaped builds outward from them.**

**Concretely: a new profile becomes "add a view class, add `X_control.h`, register routes" — and `device_manager.{h,cc}` is not touched at all.** That is the payoff, and it is why this is worth a layer rather than being architecture for its own sake. Today the same change means adding methods *and* profile type names to a header half the codebase includes: `device_manager.h` currently pulls in `node/cia402.h` and `node/cia402_drive.h` and names `Cia402Status`, `Cia402Command`, `Cia402TargetKind`, `StoreParametersConfig`, `RestoreGroup`, `RestoreDefaultParametersConfig` — six type dependencies that exist *purely* to host those six wrappers, none of them needed to own devices, own the driver, or scan a bus. Move the wrappers out and every one of them goes. The benefit compounds with each profile added; the cost is fixed and paid once.

**Each view header gets a control header, 1:1.** `profile_device.h` → `profile_control.h`, `cia402_drive.h` → `cia402_control.h`, `somanet_drive.h` → `somanet_control.h` when SOMANET operations arrive. One axis, one rule, and "where do I call this from a handler?" always has the same answer. An earlier draft named the second file `profile_procedures.h` (store and restore genuinely *are* procedures by our own definition) and that was rejected: a procedure body's signature is `runXxx(ProfileDevice&, ProgressReporter&, std::stop_token)` — it takes the *view*, because `ProcedureManager` does the `withDevice` and the resolve. So store and restore are not procedure-shaped functions sitting in the wrong file; they are control-shaped functions that will be *rewritten* into a different signature later. Naming a file for a destination whose shape it does not have buys nothing.

**Keeping the indirection honest.** The migrated path is one hop longer than the method it replaces (handler → free function → `withDevice` → lambda → view), because `withDevice` is the `shared_lock` + `findDevice` that was already inline, given a name. Two things keep that from compounding: a file-local `withDrive`/`withProfile` template in each control `.cc` folds the borrow-and-bind pair every function repeats, so each public function is one line rather than seven; and no helper is added that merely renames a factory call (a first draft had a `bindDrive` wrapping `createCia402Drive` — deleted as pure noise).

**What stays on `DeviceManager`:** `readParameter`/`writeParameter`/`readPdoMapping`/`writePdoMapping` by position. Those are `Device`-level operations on something it genuinely owns, with no profile vocabulary involved. Also unchanged: the *no service layer* mandate. A control free function is not a service — it holds no state, owns nothing, and its entire body is borrow, bind, delegate. The domain logic stays on the view.

**One consequence to be aware of:** `runCia402Command`'s `enable()` walk holds the shared lock for up to its full timeout, so a rescan issued during an enable waits for it. That is unchanged behaviour — the method did exactly this before — but it is now visible in one place instead of buried in six.

## Session 2026-08-02 — A procedure body cannot change AL state, and firmware installation is the exception that needs a second body shape (as-built + design)

`ProcedureManager` shipped this session. One constraint fell out of it that is invisible until you hit it, and firmware installation hits it immediately.

**The constraint.** A body runs inside `DeviceManager::withDevice`, which holds `busMutex_` **shared** for the run's whole duration. Mapping the lock modes settles what that costs:

| Mode | Operations | During a running procedure |
| --- | --- | --- |
| **Exclusive** | `init`, `scan`, `reset`, `configureProcessData`, **`transitionToState`** | blocked |
| **Shared** | every parameter read/write, `readAllDeviceParameters`, PDO mapping read/write, output staging, diagnostics, watchdog config, recorder dump | concurrent |
| **Untouched** | `exchangeProcessData` (RT loop, lock-free), monitoring, the WebSocket | unaffected |

So a five-second procedure leaves the UI responsive, SDO reads working, monitoring streaming and the drive cycling — the cost is confined to `init`/`scan`/`reset` and, decisively, **`transitionToState`**.

That last one is not merely "a user cannot change state meanwhile": **the body cannot either**. It holds the lock shared and `transitionToState` wants it exclusively, and `std::shared_mutex` offers no upgrade — the body would deadlock against itself. Harmless for everything queued next (os-command, offset detection, store/restore, auto-tuning are SDO/mailbox work) and fatal for firmware installation, which *is* its state transitions: BOOT → two FoE writes → PRE-OP, and the SMM and Kübler variants likewise.

**The exception: a second body shape, not a redesign.** The deadlock comes entirely from who holds the lock, so the fix is to stop holding it across the span. Alongside today's

```cpp
ProcedureBody = std::function<expected<void, string>(Device&, ProgressReporter&, stop_token)>
```

firmware installation gets a body taking `(DeviceManager&, uint16_t position, ProgressReporter&, stop_token)`, which the manager spawns **without** borrowing anything. That body borrows per *step* — `withDevice` around each FoE write — and calls `transitionToState` between steps, when it holds nothing. Same thread, same busy token, same snapshot, same cancellation; only the borrowing granularity differs, and it is an added `start` overload rather than a change to the existing one. This is also the shape CLAUDE.md already prescribes for long-lived work ("re-resolves its `Device` via `findDevice` each cycle") — the whole-span hold is the special case, justified for measurements because a rescan mid-measurement is meaningless.

**Two consequences to handle when it is built**, neither fatal:

- *A rescan can interleave*, since nothing holds the lock between steps. The install itself copes — the next borrow fails with "device not found" and the run fails cleanly — but `discardIfRescanned` must then **skip entries that are still running**, or it will join a live thread while holding the manager's mutex. Today it may clear unconditionally precisely because a rescan cannot overlap a run.
- *Nothing blocks a concurrent `scan()` during the install.* The busy token cannot prevent it without `DeviceManager` consulting `ProcedureManager`, which would undo the profile-ignorance established earlier today. Probably acceptable — flashing fails safely — but it is the trade being made, not an oversight.

**Also worth recording from the build:** the destructor collects running threads under the lock and joins them *after* releasing it, because a finishing run takes the bus lock on its way out (cppcheck's "reduce the scope of `threads`" would reintroduce the deadlock — suppressed with that reasoning). And a run's outcome is written through atomics so the finishing thread never needs the manager's mutex at all, which is what makes that destructor ordering possible. The other trap, found by a hung test: **`request_stop()` does not notify a `std::condition_variable`**, so a body that blocks must wait on the stop-aware `std::condition_variable_any` overload or it will never wake, and the joining destructor hangs with it.

## Session 2026-08-03 — The procedure catalogue: one generic route triple, applicability by identity, and never-run is a state rather than a 404 (as-built)

Step 2 of the sequencing in *Session 2026-08-02* ("Every OS command is a procedure"), built as planned plus two decisions the plan left open.

**The routes collapsed to `:name` now, not later.** `POST`/`GET`/`DELETE` on `/api/devices/:pos/procedures/:name` plus `GET .../procedures` are four handlers that serve *every* procedure: the catalogue resolves the name, decides whether the device has it, validates the request and supplies the body. The alternative — keep the literal `procedures/os-command` route and add only the list endpoint — was rejected because the volume is known to be coming, and doing it later means rewriting the spec, the generated client and every call site twice. The visible cost is in the OpenAPI spec: one `{procedureName}` path with an open `ProcedureRequest` body instead of per-command operations, so the generated client gained `startProcedure(pos, name, body)` where it had `startOsCommand(pos, body)`. That is the honest shape — one endpoint, one method — and it does not degrade as procedures are added.

**`libs/node/procedure_catalogue.{h,cc}` is the registry, and it is a table.** A `ProcedureCatalogueEntry` is a `ProcedureDescriptor` (name, title, description, caveats, `movesMotor`, `requiresEnabled`, the step template), an `applies` predicate, and a `makeBody` factory turning a client request into a `ProcedureBody`. Adding a procedure is a row: no route, no handler, no `ProcedureManager` change. It sits *outward* of `DeviceManager`/`ProcedureManager` and names profile types freely, which is what lets `http_server.cc` now name **no** profile type at all on this surface — the include of `somanet_procedures.h` went away with the handler that used it.

**Never-run answers `200` with an idle snapshot, not `404`.** The poll-only design already said never-run is well-formed *precisely* so a client renders one component with no empty-state branch — and the `404` the first cut returned forced exactly that branch. The blocker was that `ProcedureManager` cannot invent an all-idle snapshot: it is only told a step template when a run *starts*. The catalogue holds the template, so `idleSnapshot(steps)` lives beside the manager rather than in it, and the caller the manager's own doc comment anticipated now exists. `404` is left meaning what it should: unknown device, or a procedure this device does not have.

**Applicability is decided from identity known at scan time — a bug caught before it shipped.** The obvious predicate for the OS command entry was `createSomanetDrive(device).has_value()`, and it is wrong: that factory also requires the device's object dictionary to have been enumerated, because its CiA402 check looks for controlword/statusword in the parameter map. Enumeration is *opportunistic* — `deviceStates()` does it when a device is first seen at PRE-OP, gated on `readObjectDictionaryOnPreop` — so a genuine drive whose OD had not been read yet would be reported as having **no procedures at all**. That is a wrong answer, not a late one. The predicate is the vendor ID, which comes from SII at scan and is always known; whether the device is also a conformant CiA402 drive stays the *body's* business, where it fails with a reason. The general rule: **a catalogue predicate may only consult state that exists as soon as the device does.**

**`ProcedureStartError` became `ProcedureError` with four kinds.** `kBusy` → 409 and `kUnknownDevice` → 404 are still the only two the manager can judge; the catalogue adds `kUnknownProcedure` → 404 and `kInvalidRequest` → 400, because whether a procedure *exists* and whether a request *validates* are things the manager is never told (it is handed a body, not a name). One type across both layers keeps the status mapping in a single `procedureErrorStatus()` in `http_server.cc` instead of translating between two error vocabularies.

**Two smaller shapes worth keeping.** A descriptor's `steps` serialise as bare **ids**, not as `ProgressStep`s — every entry of a template carries status `idle` and no value, which tells a client nothing; live per-step status is the snapshot's job. And an **absent request body is read as an empty object**, so a procedure taking no parameters is started with no body at all rather than a mandatory `{}`, and every validator reads its fields the same way instead of each having to accept "nothing" as well.

**A trap found by a failing test, in the tests rather than the product:** nlohmann stores a non-negative integer *parsed* from JSON as unsigned but one built from a C++ `int` literal as **signed**, so a hand-built request fails the `is_number_unsigned()` byte-range check that the byte-identical parsed request passes. The HTTP path always parses, so the validator is right; a test that builds a request with an initializer list is testing something the server never sees. Request fixtures parse from text.

**A framing correction, and it is not cosmetic: `os-command` is not an "escape hatch."** Sessions 2026-08-02 and the first draft of this surface's text described it as the fallback "for bring-up and for commands not yet wrapped", with typed procedures as the real destination. That is wrong, and it was wrong when written: **issuing an OS command directly is an ordinary, first-class way to operate the drive**, not a workaround waiting to be replaced. Byte 0 is the command ID and bytes 1-7 its parameters — that *is* the drive's OS command interface, and reaching for it deliberately is normal practice, including for commands that also have a procedure of their own. What a typed procedure adds is named parameters, validation and a decoded result for *one* command; what it does not do is deprecate the general route. The two are peers. This matters beyond wording because "escape hatch" framing invites treating the raw path as second-class — hiding it in an "Advanced" group, skimping on its description, assuming every command must eventually be wrapped before it is properly usable. None of that follows. Every user-facing description was rewritten accordingly; the wording in the two 2026-08-02 sessions stands as the record of what was thought then, superseded by this.

**Still open:** the first typed command (`phase-resistance-measurement`) — which is also when a **parameter schema** on the descriptor has to be decided, deferred here because the only procedure with parameters today asks for raw command bytes, which no schema would describe usefully (the wrong *shape* to design against, not a lesser case) — and then offset detection, which proves composition. `hil/api` coverage of these four routes is deferred, not done.

**The console *Procedures* page was built next, ahead of those** (same day, at the user's call), because the catalogue had already made it generic — and building it against one procedure is what proves the descriptor carries enough. It is **master-detail, not a card grid**: a procedure list on the left, the selected one's detail on the right, with the selection in the URL (`/devices/:id/procedures/:name`) so a procedure can be linked to and Back works. The whole page — list, detail and live progress — runs off **one** query against `GET .../procedures`, with `refetchInterval` on only while some snapshot reads `running`; a page of finished results makes no traffic. Three things the build settled: **grouping was dropped** (NEXTGEN's Measurement/Calibration/Encoder/Advanced groups need a `group` field on the descriptor, and would render as mostly-empty headings today — add the field when the second procedure lands); the **raw OS command's parameter form is a named special case** in the page (`name === 'os-command'`), which is the honest cost of deferring the parameter schema and is confined to one branch; and the detail panel ends with a **wire section** — the three request lines plus the unformatted snapshot JSON — because the console is also the reference for whoever builds a purpose-built UI on these endpoints, so what to call and exactly what comes back belongs on the page. The device-level busy claim is surfaced too: a run on *any* procedure disables Run on the others with a note, rather than letting the user discover the 409.

## Session 2026-08-04 — Discovery is dropped, not deferred: no mDNS/Avahi, and the address is typed in (decision)

The Raspberry Pi arrived (a **Pi 5, 4 GB**), which turns the appliance from a plan into a board on the desk — and the first thing that settles is one item three previous sessions carried forward as "later".

**Discovery is a *won't do*, not an open item.** Sessions 2026-06-12, 2026-07-24 and 2026-07-31 all left it as the next rung of a ladder — manual IP entry today, mDNS/Avahi (`motion-master.local`) tomorrow. That ladder is removed. The reasoning is that its top rung was never worth much: **a `.local` name cannot be connected to**, because no CA issues certificates for a reserved TLD, so Avahi could only ever have *revealed an address the user then types into the Console by hand anyway*. What it replaces is one reading of `hostname -I` on the device, or one glance at the router's lease list. What it costs is a daemon running on every appliance, a role to write and maintain, and an advertisement broadcast on a network where the API has **no authentication** — a service announcing "an unauthenticated EtherCAT master lives here" is the wrong default even on a trusted LAN. The trade was never close once the `.local`-can't-connect constraint is taken seriously, and that constraint was already written down in 2026-06-12; it just wasn't followed to its conclusion.

**A factual correction to 2026-06-12 that made this easier to see.** That session's "Raspberry Pi OS ships Avahi, so mDNS is cheap" was true of Raspberry Pi OS and irrelevant to what got built: the image is the official Debian **cloud** `raspi-arm64` build, which ships neither Avahi *nor* `openssh-server` (the image build has to install sshd into the root filesystem through a binfmt chroot before anything can provision it). So "cheap" would have meant adding a package and a role, not flipping on something already present. `docs/LAN_DEPLOYMENT.md` had inherited the same wrong premise in its *Making it findable* section.

**The client-side variant is rejected on the same call: the scripts stay dumb.** `add-host.sh`/`add-host.ps1` could have taken a `--discover` flag — resolve `motion-master.local`, then write the `192-168-1-50.ip.…` hosts line itself, covering discovery and the hosts entry in one command. They will not. Their entire value is that they are short enough to read before running with administrator rights, on a machine that is not the server, fetched from the public repo. An Avahi dependency and resolution logic buys a saved lookup and spends the one property that makes them trustworthy.

**As-built:** the *Making it findable* section is gone from `docs/LAN_DEPLOYMENT.md` (replaced by a sentence stating plainly that nothing advertises itself and none is planned), the open item is gone from `rt/README.md`, and `CLAUDE.md`'s off-loopback paragraph now records discovery as deliberately unsolved rather than outstanding. The mDNS lines in the three earlier sessions stand as the record of what was thought then, superseded by this.

## Session 2026-08-04 — The Pi image, built and verified offline: every card ships a shared key and `root`/`root` (as-built + decision)

The build script written on 2026-08-01 ran end to end — that day, and again today. Today's image was then verified *offline*, by reading its filesystems out of the image file with `debugfs` rather than trusting the script's own in-guest checks: RT kernel `7.1.3+deb14-rt-arm64` with a matching `initramfs` line in `config.txt`, `isolcpus=managed_irq,domain,3 rcu_nocbs=3 nohz_full=3 irqaffinity=0,1,2` on the command line, `motion-master` installed and enabled, machine ID blank, build key gone. `rt/README.md`'s "has not been run end to end" was written ten minutes *before* the first successful run finished and had been stale ever since.

**The image had no way in at all, and that is the one thing the in-guest checks could not catch.** `/etc/shadow` carried `root:!unprovisioned` — locked, the stock cloud-image state — root was the only account with a real shell, `/root/.ssh` was empty, and the finalisation step had removed the build key and its sshd drop-in by design. No cloud-init to create anything either. The card would have booted, run Motion Master on `0.0.0.0`, and been sealed: no SSH, and a console offering a `login:` prompt nobody could satisfy. That finalisation is *correct* hygiene for a shipped appliance and exactly wrong for bring-up — and it fails hardest in the case that matters, because if the RT kernel does not bring up RP1's Ethernet then SSH is precisely what is unavailable and the console is all there is.

**Two keypairs, and conflating them is the mistake to avoid.** The `.cache` key stays what it was: throwaway, per-build, authorised only for the emulated provisioning run and removed before the image is finished. Beside it now sits a durable `motion-master-rpi` key whose public half is baked into every image and whose private half is **deliberately shareable** — the key handed to whoever owns a board. It lives in the invoking user's `~/.ssh`, not in the repository: a private key in a git working tree is one `git add -A` from a public repo (whose push protection would likely reject it anyway), and `.cache` is advertised as disposable, so deleting it must never cost the key that opens every card in the field. Only the public half is ever needed to *build*, so nothing is given up by keeping the private half out of the tree. `$HOME` is unusable for finding it — the script is routinely run under `sudo`, where that is `/root` — so it resolves the path through `SUDO_USER`.

**The root password is the account name, and root may log in over SSH.** Not an oversight and not a temporary state: the board's HTTP API has no authentication and binds every interface, so login credentials are not what stands between the drives and the network — the network's own trust boundary is, and these add nothing an attacker who can already reach port 61447 does not have. What they buy is a board that stays recoverable on the day Ethernet does not come up. `RT_IMAGE_ROOT_PASSWORD=` (empty) leaves root locked and key-only for whoever wants the opposite trade. **The honest cost, recorded so nobody has to rediscover it:** a distributed key cannot be revoked for cards already flashed — replacing it means rebuilding the image and reflashing every board.

**Two implementation notes.** The password is written as a SHA-512 crypt hash straight into `/etc/shadow` rather than by `chpasswd` in a chroot: the bind mounts that would need is torn down before the QEMU boot, and re-establishing `/proc` and `/dev` to run one command is more moving parts than a hash and a substitution — and replacing the field wholesale is also what clears the `!`. And the key install replaces the build key in the same block rather than in a later one, so no window exists in which an image carries both or neither.

**A finding worth keeping, because the obvious reading of it is alarming and wrong:** `/etc/systemd/network` in the image is **empty**, which looks like a card that will never get an address. It is not. The base image carries `netplan.io`, which generates the `systemd-networkd` configuration into `/run` at boot; its globs are `en*` and `eth*`, and `net.ifnames=0` on the kernel command line means the Pi's interface comes up as plain `eth0`. Matched either way. The emulated build guest reaching GitHub to download a kernel and a `.deb` was the evidence that settled it.

**`6.0.0-alpha.65` exists for this and nothing else.** No server, web app or client change — it is released so the appliance can install a current build instead of the `alpha.62` the role had been pinned to since that was the first release carrying `gameLoop.cpuAffinity`. That default is an exact **pin, not a floor**: provisioning installs the version named there, so a board moves to a newer release only by editing it, and the release must exist before the play can download its `.deb`.

**Still open, and only the hardware can close it:** whether the 7.1.3-rt kernel drives BCM2712 and RP1 on a real Pi 5. QEMU cannot answer that — it is not a Pi. Card in the slot, serial (`enable_uart=1` and `console=ttyAMA0,115200` are already set) or HDMI, and watch for Ethernet.

## Session 2026-08-07 — Kinematics sits *above* Motion Master: precomputed joint buffers, one server-authoritative solver, and the four closed-form machine classes (design, exploratory)

An exploratory conversation, recorded because the conclusions are load-bearing for two things already on the roadmap (`TrajectoryCyclicTask`, DC SYNC0) and because the shape they imply is easy to get wrong in a way that would be expensive to undo. **Nothing here is built or committed to a date.** It is gated behind the RT Linux work and SYNC0 activation, both of which are prerequisites rather than nice-to-haves: in free-run the drives act on frame arrival, so the loop's wake jitter *is* the actuation jitter, and axes latch their setpoints at slightly different moments — which for coordinated Cartesian motion is exactly path error. A perfect IK solution over an unsynchronised bus still traces a wobbly line.

**Scope the machine classes and the hard half of the problem disappears.** Gantry, SCARA, delta, and 6R-with-a-spherical-wrist all have **closed-form** inverse kinematics — algebra, microseconds, deterministic worst case. A general 7-DOF arm or an offset-wrist 6R needs an iterative solver, which is a latency hazard anywhere near a cycle budget and a convergence-failure mode anywhere else. Those four are the scope; the constraint is what makes everything below tractable, so it is a decision, not an accident of what came to mind first.

**IK is precomputed into joint buffers; it does not run per RT cycle.** This is the architectural call that shapes the rest. Solve the whole path up front on the HTTP thread, in ordinary allocating C++, and hand the RT loop a buffer of joint setpoints it plays back with no solver involvement — which is precisely what the planned `TrajectoryCyclicTask` already is (immutable program, a column per axis, one shared cursor; Sessions 2026-07-09 and 2026-07-13). Three payoffs, and the middle one is the real argument: the RT path gains no new code to bound or profile; **every point is validated against joint limits, velocity, acceleration and singularity proximity *before* anything moves**, so a path that would fault at joint 4 is rejected at plan time rather than discovered halfway through; and no new RT machinery is needed at all. Online per-cycle IK is only required for jogging and sensor-reactive motion, and jogging has a cheaper answer (below).

**One solver, and the server owns it.** The tempting shape — TypeScript IK for the 3D preview, C++ IK for execution — produces two implementations that will disagree at branch boundaries and near joint limits, which means the preview lies to the user; worse than having no preview. Instead the browser sends a pose, the server solves, and the browser renders the joint angles it gets back. Same solver, same limits, same branch selection as execution, correct by construction. At 30–60 Hz over loopback the round trip is sub-millisecond. If LAN latency later makes it feel sticky the same C++ compiles to WASM — as an upgrade, not a starting point, and never as a second implementation.

**The 3D view is confirmation, not the input device.** Dragging a gizmo is good for "roughly over there" and useless for "X = 250.0", which is what people actually teach. Three inputs side by side, as every real teach pendant has: numeric pose entry (used most), jog buttons — Cartesian and per-joint (used second), and the 3D view, which is what makes the other two comprehensible. Jogging is the one genuinely online case and it is a mild one: a constant-velocity ray from the current pose, solved at ~100 Hz off the RT thread with joint-space interpolation filling the cycles between. No planner, trivially abortable.

**The IK is the smallest item on the list.** What the work actually consists of, roughly by cost:

- **The robot model and a UI to enter it** — link lengths, joint limits, axis directions, gear ratios, increments per revolution, homing offsets, and the joint-index → slave-position map. The biggest hidden cost and the least glamorous. Unit conversion (increments → radians, through ratio and sign) causes more real-world faults than the kinematics ever will. URDF import is the standard escape hatch and brings meshes with it; parametric primitives sized from link lengths are an honest v1 and cost nothing in assets.
- **Calibration** — TCP relative to the flange, base frame relative to the workpiece. Without both, "move to X=100" has no defined meaning. 4-point TCP and 3-point base are the standard wizards.
- **Homing** — model zero must equal joint zero.
- **Branch selection and continuity** — a 6R arm has eight solutions. Silently switching branches between frames flips the arm 180°: confusing in simulation, dangerous on hardware. Always take the solution nearest the current configuration, and expose shoulder/elbow/wrist configuration as something the user can see and lock.
- **Reachability feedback** — colour the model on unreachable poses, joints at limits, and a degrading Jacobian condition number. This is the single thing that makes a drag UI feel intelligent instead of mysterious.
- **Path types and time parameterisation** — PTP and LIN at minimum, blending between segments, and a jerk-limited profile (Ruckig is the standard answer). Limits must be checked *after* IK: constant tool speed near a singularity produces joint speeds that are anything but.
- **Synchronised enable** — bringing six drives to Operation Enabled in CSP is not atomic; it needs a state machine, and a coordinated drop on fault.
- **Safety** — software joint limits, a Cartesian envelope, a reduced speed cap in teach mode, and an explicit statement that the model knows nothing about the fixtures.

**Three explicit won't-dos.** Collision checking (self or environment) is a separate project — collision geometry, a broad phase, FCL or Bullet — and is out of scope. So is obstacle-aware motion planning: direct paths between taught waypoints, no RRT. And no program language; an ordered waypoint list with per-segment move type and speed covers commissioning, and a language is a tar pit.

**Where it would live, and why that is the point.** A `libs/` module plus a route plug-in (`mm::api`, `HttpServer::addRoutes`) and a `web/apps/` PWA, with `Device` and `DeviceManager` untouched — the profile-ignorant seam from Session 2026-08-02 doing exactly what it was built for. Kinematics is a layer above the axis layer, not a change to it, and the moment it starts needing edits inside `node` something has been modelled in the wrong place.

**The v1 worth aiming at:** pick a class, enter or import a model, home, calibrate a TCP, jog in Cartesian and joint space, teach a waypoint list, replay it as one precomputed synchronised trajectory, and watch the 3D model track the real machine from the recorder ring. Commissioning-grade, not a robot controller — and the distinction is the whole reason it is achievable. Starting class is undecided: SCARA and gantry reach something demonstrable fastest, a 6R arm exercises branch selection, wrist singularity and orientation representation all at once and is the better forcing function if the architecture has to be right the first time.

## Session 2026-08-07 — Firmware installation, the first procedure that changes AL state (as-built)

The `BusProcedureBody` designed on 2026-08-02 was built, and with it the procedure it was designed for. Most of what follows is not the happy path — that part went as planned — but the four things the build corrected or turned up.

**The `firmware` config section was designed, then removed before it shipped, and removing it made the layer question disappear.** The plan had a `firmware` block (default skip list, cache toggle, directory) which forced a real question: `procedureCatalogue()` is a config-less free function returning a static table, so a config-derived *descriptor default* meant either turning the catalogue into a constructed object or threading a config through its four free functions. Neither is bad; both are a structural change bought for one default value. Dropping the config section removes the purchase entirely — the default skip list is the descriptor's `defaultValue`, caching is a request parameter (`cachePackage`), and the cache directory is `userCacheDir()/firmwares` resolved in the body. The catalogue is untouched, `main.cc` is untouched, and the settings are *more* discoverable than a JSONC block because they arrive with the procedure that uses them. The general shape worth keeping: **a per-run choice belongs in the request, not the config file, and reaching for config is what creates layering problems that a parameter does not have.**

**It lives in `somanet_procedures`, not a file of its own.** The first cut was `firmware_procedures.{h,cc}`, which is the wrong axis: procedure files here split by *scope* — `profile_procedures` for generic CiA301, `somanet_procedures` for vendor-specific — and every piece of firmware installation is SOMANET (the `app_`/`com_` naming, the `fs-remove=` pseudo-file, the dummy FoE filenames, the bootloader handover). Splitting by feature instead would have started a second organizing principle in the same directory. `firmware_package.{h,cc}` does stay separate, because it is an offline data model like `pdo_mapping` or `process_data_dump` rather than a procedure.

**The final state is a parameter, and the reason is the factory-reset case.** "Return to PRE-OP" was the plan and it is the right *default* — but a device whose application has been erased has nothing to reach PRE-OP with, so a hard-coded transition would report a failure for an operation that succeeded. Restoring the *previous* state was considered and rejected: it described a device running the old firmware, and "previous" is a snapshot nothing protects, since the body holds no lock between steps and a user can change state underneath it.

**`finalState` is an AL state number, and getting there took two corrections worth recording.** The first cut was a string enum of this procedure's own — `"preop" | "init" | "boot"` — which is a second vocabulary for a concept the API already names: `POST /api/devices/state` takes `state: 1|2|3|4|8` and every state read reports `alState` in the same ETG.1000.6 encoding, and no string form of an AL state exists anywhere on the wire (`toString(EtherCatState)` is for log lines). It is now `mm::comm::EtherCatState` end to end, integer on the wire, with the descriptor's `enum` options carrying the titles so the Console still reads "PRE-OP — loads and confirms the new firmware". **The general rule: a procedure parameter may invent a vocabulary (`group`, `mode`, `data` do) only where the API has none; where one exists, use it.** The second correction was the restriction to three states. It was justified by "SAFE-OP and OP re-map the whole bus", which does not survive scrutiny — the caller's own follow-up `POST /api/devices/state` performs the identical re-map, so refusing split one operation into two and bought nothing. The original argument had leaned on a power cycle being required first, which turned out to be false. Every AL state is accepted; PRE-OP is the default.

**What the drive firmware actually does, read out of `sc_somanet_ip/module_ethercat/libsrc/ethercat_service.xc` rather than assumed.** The bootloader and the application are **two builds of the same EtherCAT service**, selected by the compile-time `ETHERCAT_BOOTLOADER` flag; both implement the full AL state machine, so registers, SII and AL status work whichever is running. (Register communication needs neither — the ESC serves those in hardware, which is why they work in any state and on a device with blank flash.) **The handover is driven by the master's INIT → PRE-OP request**: in the bootloader build the PRE-OP case calls `foe_glb_buffer_request_restart()` and then waits `REBOOT_TIMEOUT`; if a valid application exists the device *restarts into it inside that wait* and the following lines never run. If the wait expires, the application did not start, and the code sets `al_status_code = AL_NO_VALID_FIRMWARE` (`0x0014`), disables mailbox and PDO services, and returns to INIT. BOOT is the mirror: the application build's BOOTSTRAP case calls `boot_to_bootloader()`. Three consequences: **no power cycle is needed for the firmware** (only for an SII, since the ESC reads EEPROM at reset); the exit from BOOT is a *reboot*, so its timeout is sized in tens of seconds rather than as a mailbox round trip; and the no-firmware case is a decodable `0x0014` rather than a hang, which is what makes `preop` a safe default even when it is the wrong choice. Still unknown: what a *cold* power-up does when a valid application is present — the code read covers only the master-driven transition, so the descriptor does not claim either way.

**`foe_error.h` finally has the caller it was written for, in 2026-07-17, and left deliberately unused for.** `readFile`/`writeFile` now return `FoeError`. Two branches earned it and neither can be written against a string without matching on wording: retry a `Transient` failure (packet desync, bootloader not yet warm) and abort at once on a `Permanent` one; and treat `FileNotFound` from the `fs-remove=` pseudo-file as success, because a removal with nothing to remove is the outcome the caller wanted. The prediction that the string face would confine the blast radius held exactly — six call sites changed by one word (`.error()` → `.error().message`), and twelve test fakes changed by their signature only.

**A mailbox drain went into the driver rather than into the flasher, and that placement is the point.** A timed-out FoE transfer leaves the slave's mailbox out of step so every later transfer reads the previous answer — a state that survives a Motion Master restart and historically needed a power cycle. Draining before each transfer costs one FPRD when the mailbox is empty (with a zero timeout `ecx_mbxreceive` checks the sync-manager status once and returns), which is well under a percent of an FoE transfer and nothing against the 700 ms `EC_TIMEOUTRXM` it prevents. It sits in `SoemFieldbusDriver` because **a wedge poisons every FoE caller, not just the one that caused it** — in the flasher's retry loop it would leave a wedge caused by a failed HRD read poisoning `/api/devices/{pos}/files/*` until something happened to retry.

**`discardIfRescanned` had to change, and the reason is sharper than "do not join a live thread".** A running thread holds a `shared_ptr` to its own `Run`, which owns the `jthread` it is executing on; if the map dropped the last other reference, the thread would destroy and join *itself* on the way out. A borrowing body makes this unreachable (a rescan cannot overlap one), a `BusProcedureBody` makes it reachable. So the sweep now runs on every call and compares each run's own recorded generation, rather than firing once on a change — a still-running entry is skipped and collected on a later pass. Firing-once plus skip-running would have leaked the entry forever; not recording a per-run generation would have collected runs started *after* the rescan.

**Two bugs the real fixtures and one question caught, both of which a synthetic test would have missed.**

- The SII category walk required a type *and* a size word at every step, but the `0xFFFF` end marker is a lone word — and in the real Circulo image it is the very last word in the file. A fixture shaped by my own expectations would have had a spare word after it. Committing the two real packages (`libs/node/tests/data/`) is what caught it on the first run, and the same argument applies to the packages themselves: the Circulo ships an SII and no COM binary, the ACTILINK the reverse, so between them they pin both shapes.
- Asking "why is the firmware base64?" turned up a **path traversal**. `firmwareCacheDir() / request.filename` joins a client-supplied string onto a server path on an unauthenticated API, and the SOMANET name grammar is *not* a guard: `package_../../x_b_c_v1.zip` splits into five perfectly legal underscore-separated fields. A read primitive on the parse path and a **write** primitive on the cache step. Fixed by resolving through `UserCache::resolve` — which already owns this validation for this very root, including the trailing-dot and Windows-device-name spellings — plus a separator check to keep the cache flat. The same question exposed a second, milder bug: reading a cached package was gated on the *name parsing*, so a package uploaded to `firmwares/` under any other name could never be installed. The naming rule governs **writing** to the cache, never reading from it.

**On base64 itself:** the generic procedure route takes JSON and JSON has no binary type, so a one-shot install pays 33% inflation. That is the convenience path, not the only one — the firmware cache *is* `/api/user-cache/firmwares/`, so `PUT`ing raw bytes there and starting the procedure with only `packageFilename` was already the zero-inflation route and now says so in the descriptor. One generator trap recorded: `format: byte` is the correct OpenAPI spelling for base64 but `swagger-typescript-api` maps it to `Blob`, which is not what goes on the wire and would serialise to `{}`. The field is a plain `type: string` with the encoding in its description.

**A Tools *Utilities* page exists now, and the reason it is plural matters.** `GET /api/firmware-package-name` decodes a package filename; the Console page behind it is explicitly a container for several small device-free helpers rather than a page for this one. The two existing Tools pages each justify themselves with one substantial artefact (an ESI file, an SII image), and a one-input-one-answer helper cannot — so without a shared page it would either get a sidebar entry out of proportion to it, or not exist. Two shape notes: the endpoint is a `GET` with a query where its siblings are `POST`s, because there is no file to upload, only a short string; and the decode runs on the server rather than in TypeScript, so the answer the page gives is by construction the one the installer will act on, rather than a second grammar that would disagree first on exactly the odd names worth checking.

**Hardware runs found one bug, and my first diagnosis of it was wrong — the wrong turn is worth recording, because it was avoidable.** On an ACTILINK: BOOT entered, `app_firmware.bin` (405504 B) written in 11.5 s, and then `com_firmware.bin` refused in under a second, five times over, before the always-attempted exit put the device cleanly back in PRE-OP.

*The wrong diagnosis.* The previous generation re-read the AL state before each binary and re-entered BOOT if it had been left; that guard was lost in the rewrite, so the obvious story was "committing the first image takes the bootloader out of BOOT". It was built and shipped. It was wrong twice over: the second run showed the re-entry log line **never fired**, and it could not have — **AL status is served by the ESC in hardware**, so it stays readable no matter what the bootloader's CPU is doing. A guard that reads AL status is structurally incapable of detecting a busy bootloader, which is the very fact this session had already written down two sections earlier when explaining why registers work in any state. The lesson is not "check the logs" but that a plausible mechanism copied from a previous implementation is not evidence, and the timing in the log was evidence all along: **every failure took ~700 ms, which is `EC_TIMEOUTRXM` exactly.**

*The second wrong answer.* FoE was passing `EC_TIMEOUTRXM` (700 ms) where the previous generation passed `kFoeTimeoutUs` = 10 s — a real difference, and a plausible one, since the timeout is per *packet acknowledgement* rather than per transfer. It was raised to 10 s. The next hardware run failed identically, just ten seconds slower per attempt: the slave was not answering at all, so the timeout had never been the constraint. **The value was reverted to `EC_TIMEOUTRXM` once the real cause was found** — with correct BUSY handling a bootloader that needs more time says so, so silence past 700 ms genuinely means something is wrong, and keeping 10 s would only have made a dead slave stall five times longer for no gain. The wrong BOOT guard was reverted too, rather than kept as belt-and-braces: it never fires, it costs a round trip per transfer, and leaving code whose stated rationale is false is how the next person inherits the same wrong model.

*A note on the retry loop.* Five retries against a 700 ms timeout turned a 0.7 s failure into a 4 s one and recovered nothing, because retrying cannot fix a timeout that is too short. Retry policy is only as good as the classification underneath it, and "transient" was the right classification of the wrong quantity.

*The actual actual cause: a bug in SOEM 2.0.* Raising the timeout moved the failure from 700 ms to 10 s and changed nothing else — the slave was not answering at all. `ecx_FOEwrite`'s `ECT_FOE_BUSY` branch is broken, and in a way that is unmistakable once read: it builds the resend through a `FOEp` still pointing at the buffer handed to the previous `ecx_mbxsend`, then calls `ecx_mbxsend(context, slave, (ec_mbxbuft *)&MbxOut, ...)` — and `MbxOut` is NULL by then, so it sends *the address of its own stack pointer variable* instead of the packet. The 1.x → 2.0 refactor from a stack mailbox buffer to a pooled one converted the neighbouring `FOE_ACK` branch and missed this one; in 1.x `MbxOut` **was** the buffer, so `&MbxOut` was right there. A bootloader replies BUSY while it erases and programs flash, which is why the application binary (acknowledged without a pause) wrote fine and the communication binary written straight after it never completed — and why the previous generation, on SOEM 1.x, writes the same file to the same drive in 3.5 s. Fixed in `ports/soem/portfile.cmake`, which already does surgical `vcpkg_replace_string` edits rather than carrying patch files.

Two things were asserted before they were checked, and both were only verified after being challenged. The 1.x comparison: `extern/SOEM/soem/ethercatfoe.c` in the previous generation declares `ec_mbxbuft MbxIn, MbxOut;` — **stack buffers** — so `&MbxOut` is genuinely correct there and `FOEp` stays valid because the buffer is reused rather than handed off; the claim was right, but it was made from memory. And the link from the bug to the symptom: there was no evidence the drive sends BUSY at all. There is now, and it is better than an inference — **the previous generation's vendored SOEM carries a local fix to this very branch**, marked twice with `https://github.com/OpenEtherCATsociety/SOEM/pull/627`. Someone had to make the BUSY resend work before firmware writes succeeded on these drives. That is the evidence that should have been found *before* patching a dependency, and it was two greps away in a repository already open.

A second defect in the same branch was fixed with it, and it is the more frightening one: a BUSY arriving *before* any data packet leaves `worktodo` FALSE, so the loop exits and `ecx_FOEwrite` returns the receive's positive work counter — **reporting success for a firmware write that sent nothing**. Upstream's comment says "otherwise ignore", which should mean "wait for the slave's next message", not "return success".

*Two wrong answers before the right one, and the pattern in them is the lesson.* BOOT state (a mechanism copied from the previous generation, never verified — and refuted by a fact this same session had already written down). Then the timeout (a real difference from v5, but not the one that mattered; reverted once that was clear). Only then the actual bug, found by reading `ecx_FOEwrite` instead of reasoning about it. Each wrong answer was plausible, each was cheap to ship, and each cost a hardware round trip. **Reading the source of the thing that is failing should have been the first move, not the fourth** — the failing call was twenty lines of a dependency we already vendor as an overlay port and had already patched three times for other reasons.

One diagnostic improvement came out of it: `EC_TIMEOUT` is `-5` and `EC_ERR_TYPE_FOE_ERROR` is `5`, so a `-5` return from an FoE call means *either* "the slave sent an error" or "the slave never answered", with nothing to tell them apart. Reporting only the first is what made three logs read as a refusal when they were a timeout; the message now names both.

**Reaching PRE-OP after an install is the point, and it needed a fix of our own.** Once the binaries actually wrote, the install ended with the device in INIT and the run failed: *the firmware was written; the device did not reach PRE-OP*. The previous generation "succeeds" here only because it re-initialises the entire master afterwards and lets a full rescan pick the device up — which is exactly what v6 exists to improve on, so matching it would have been the wrong lesson. **Bringing one device back up without disturbing the bus is the feature.**

The cause was ours, in `SoemFieldbusDriver::transitionToState`, and it took two passes to reach the bottom of it. The first pass saw that the mailbox sync managers were programmed exactly once — the slave being struck off `bootMailboxSlaves_` on the attempt — so nothing could reprogram them after the restart. True, and fixed, and not enough: the next run showed the reprogramming running thirteen times and failing every time, `PRE-OP mailbox - write 0x0000/0` with `SM0 wkc=0, SM1 wkc=0`.

**The restart clears the ESC's station address.** Everything the master does is addressed by that value, so the device did not merely lose its sync managers — it disappeared. The reprogramming was reading EEPROM from a slave that could not hear it, getting zeros, and writing those zeros into the master's own copy of the mailbox configuration; the state reads were reporting the last value SOEM had cached, which is why it looked like a device calmly sitting in INIT with no error. **A slave that refuses a state says why; a slave that cannot hear the request says nothing, and the two are indistinguishable from AL status alone.** The recovery needs no scan: auto-increment addressing reaches a slave by its position on the wire with no configuration whatsoever, which is exactly how `ecx_config_init` assigns the address in the first place, so one `ecx_APWRw` puts it back for that one device. The driver now probes reachability, re-addresses if needed, and only then reprograms.

The diagnostic that found it is worth keeping as a habit: on a failed transition the driver probes the slave **twice** — by configured address and by wire position — and says which answered. One line of output turned "the device is in INIT for some reason" into "it has been reset and lost its address", and the two want opposite fixes.

Two things this run taught beyond the fix. **The bug could only appear once the transfers succeeded** — every earlier failure stopped before the handover, so a working COM write was the precondition for discovering it; a feature can hide its next bug behind its current one. And **an error message that omits a zero field throws away the diagnosis**: the transition error only printed the AL status code when it was non-zero, so the single most informative fact — that the slave reported no error at all — was formatted out of existence. It is now always printed, with zero spelled out in words.

**Verified end to end on an ACTILINK-S, all the way to OP, in ~24 s.** BOOT; application binary in 11.7 s; communication binary through two `FOE_BUSY` resends in 2.4 s; the handover restart taking the device off the bus for ~6 s; the master noticing it had lost its station address and re-assigning it at its wire position; the mailbox reprogrammed; PRE-OP 100 ms later; then the whole-bus re-map — which **re-read the PDO mapping out of the firmware just installed**, the exact case the reactive-mapping design was written for — SAFE-OP, and OP. **One device went down and came back by itself; nothing else on the bus was touched.** That is the improvement over re-initialising the master, demonstrated rather than argued.

A worry that turned out to be unfounded, recorded because it looked serious: the restart clears *all* ESC configuration, so FMMUs are gone too, and PRE-OP only needs the station address and the mailbox sync managers. Reaching SAFE-OP/OP looked like it might need a separate recovery. It does not — `configureProcessData` reprograms FMMUs and sync managers wholesale on every re-map, so the climb out of PRE-OP repairs the rest as a side effect of what it already does.

**The three defects each hid the next, which is why this took four hardware runs.** The BUSY resend bug stopped the communication binary, which meant the handover never happened, which meant the address loss could not be discovered. Nothing was going to reveal the second problem until the first was fixed, and nothing the third until the second. That is worth expecting rather than being surprised by: on a multi-stage operation against real hardware, *fixing a bug is how you find the next one*, and a plan that budgets one round trip per bug will be wrong.

**Still open:** `hil/api` coverage of the new procedure. The install now runs on real hardware up to the second binary; whether the fix carries it through, and everything past it (the SII write, the PRE-OP handover), is still unproven.

## Session 2026-08-08 — Handlers that cannot block the loop: the HTTP surface becomes Request → Response (as-built)

A firmware install made the Console freeze for fifteen seconds, and the cause was not the Console. uWebSockets runs every handler on the app's single loop thread, so a handler that blocks blocks the whole API — and Motion Master is full of handlers that block for seconds by nature. The proof was one line: during an install, `/api/version`, which touches no hardware whatsoever, hangs too.

**The chain, all four links from code.** `RootLayout` polls `GET /api/devices/state` continuously for the sidebar AL badge → that handler takes the driver's `controlPlaneMutex_` → an FoE write holds it for the whole transfer → and with the loop thread parked inside that handler, every other request queues behind it. Not a Procedures-page bug; a property of the whole surface.

**Wrapping the blocking handlers was tried first, and is the wrong shape.** It leaves the dangerous thing (touch the bus in a handler) as the default and the safe thing as something to remember — across seventy-four routes and every future one. What shipped instead makes it structural: a handler is a plain function from a snapshotted `Request` to a `Response` value, run on a worker pool, with the framework writing the result. **It cannot block the loop because it never runs on it.**

The three uWS rules a handler no longer has to know: a response may only be touched on the loop thread (every write is inside `loop->defer`), a response dies with its connection (the `onAborted` flag is set *and checked* on that thread, so the two cannot interleave — it is serialised by the thread, not protected by the atomic), and a request is valid only synchronously (snapshotted before dispatch).

**Three things fell out that were not the goal.** Handlers became unit-testable with no server, socket or loop. Every response became backpressure-aware — only the `.mmpd` dump had a hand-written `tryEnd`/`onWritable` loop before, while the 3 MB ESI response used a plain `end()`; the framework's writer replaced the former and fixed the latter silently. And the ~20 copies of `onData`/`aborted` boilerplate that every body-carrying endpoint held are gone, taking `http_server.cc` from 2200 lines to under 1700 despite the comments growing.

**Sizing the pool is not about throughput.** Bus operations serialise on the control-plane lock regardless, so extra workers buy no parallelism on the wire. What they buy is that a request blocked on the wire cannot make a request blocked on something else — or needing nothing at all — queue behind it. Four was right when two routes used the pool and wrong once all of them did; sixteen gives the ~5 clients this API is sized for about three concurrent requests each. A worker parked on a mutex costs a stack and a kernel task.

**Running *every* handler on the pool is the contested call, and it was re-argued rather than assumed.** The standard advice for an event-driven framework is to split: fast handlers inline, blocking ones offloaded. That advice is correct, and its three objections are worth taking seriously — context-switch cost, the double thread hop, and quick requests starved behind slow ones for worker slots. The first two are microseconds against browser TLS round trips of milliseconds at the ~10 req/s this API sees; they are real and irrelevant. **The third is the only one with force, and it is the original bug in miniature** — but it bites only if the pool can be saturated, and here it cannot: HTTP/1.1 browsers cap at ~6 connections per origin, so the ~5 clients this serves can have at most ~30 requests in flight. A pool wider than that is never contended. That is a luxury of a bounded local client population, not a general result.

What decided it against splitting was **asymmetry**: misjudging a blocking handler as fast freezes the entire API — the bug that cost this session a day — while misjudging a fast one as blocking costs 30 µs. Six orders of magnitude, and the judgement is genuinely hard here, since 48 of 74 handlers reach `DeviceManager` and whether one blocks depends on what another thread is holding at that moment (`GET /api/monitorings` looks like an in-memory list and takes a mutex the sampler thread also holds). A boundary that erodes toward the catastrophic side is not worth microseconds. The argument does rest on an arithmetic claim, so the claim is checked at runtime rather than trusted: the Router logs, edge-triggered, the first request that ever has to wait for a worker.

**Why uWS ships no pool, since the question comes up.** Its model is one thread serving tens of thousands of connections, and it rests entirely on handlers never blocking; its answer to scale is more loops via `SO_REUSEPORT`, which scales connections and CPU but not blocking work — with N loops and N+1 blocked requests you are stuck again, just later. We break the assumption unavoidably, because EtherCAT I/O is synchronous. **libuv ships a pool for exactly this reason** (filesystem, DNS, crypto are blocking APIs), which is what Node uses; uWS is lower-level and leaves the policy to the layer above. This is the ordinary remedy, not an exotic one.

**Why uWS at all, since a framework would have handed all of this over for free.** The fair version of "do not hand-roll it" is not about `Request` — it is about the framework choice, and it has real answers: cpp-httplib is thread-per-request by design, so handlers may block and no `Router` need exist; Drogon and Crow both ship the pool *and* the value-typed request. uWS stays because it is already the WebSocket server on 62281, which is the harder half and the reason it was chosen; running a second HTTP stack beside it to save ~550 lines including documentation and tests is the worse trade. Two things make those lines smaller than they look. **`Request` is not an alternative to something uWS provides — it is the pattern uWS documents:** its own `examples/UpgradeAsync.cpp` heap-allocates a struct holding copied headers, the response pointer and an `aborted` bool, under the comment "*HttpRequest (req) is only valid in this very callback, so we must COPY the headers we need later on […] You must not access req after first return*". Ours is that struct with a name, written once instead of seventy-four times. And **every framework that lets a handler run off the I/O thread converges on the same shape** — Drogon's `HttpRequestPtr` plus a callback, Crow's `crow::request` and returned `crow::response`, cpp-httplib's `Request`/`Response` values — because snapshot in, value out is the only shape that survives a thread hop. Arriving there independently is evidence for the design rather than against it.

**Corking is the one facility the framework does hand you, and the Router missed it at first.** uWS corks the socket around a handler it invokes itself — `HttpContext` corks before dispatch and uncorks after — so writes made synchronously inside a handler coalesce into a single send. A `Loop::defer` callback runs from the loop's wakeup queue instead, outside that cork, so the deferred write issued a separate `us_socket_write`, and hence a separate TLS record and syscall, for the status line, for *each* header, and for the body: six or seven where one would do. The kind of the defect is worse than its cost, though: the shutdown a `Connection: close` response owes its client lives in exactly two places — `HttpContext`'s post-handler uncork and `HttpResponse::cork` — and a plain deferred write goes through neither; uWS's own comment there says that path exists to "match the one in HttpContext". `writeResponse` now corks, as does the resumed `onWritable` write, where uWS skips its close handling entirely for as long as an `onWritable` is registered. **The generalisable part: moving work off the loop moves it out of the framework's implicit scaffolding too, and what that scaffolding was doing is invisible at the call site.**

**Shutdown has to close dispatch before it drains, and the ordering is not the obvious one.** A worker defers its write onto the loop, so no worker may outlive the loop thread — hence draining the pool *before* closing the loop, which was right and is not sufficient. The loop goes on accepting for as long as it is draining, so a request landing after `wait()` returned would start a fresh worker that then defers onto a loop whose thread had since exited: a use-after-free rather than a late reply, and one needing only a sub-millisecond window to hit. The flag that closes dispatch is therefore set **on the loop thread**, and `stop()` waits for that round trip before draining — because the Router dispatches on that same thread, a store made there totally orders against every dispatch, which a store made from the stopping thread does not. Blocking on that round trip is safe only because of the Router itself: no handler runs on the loop thread any more, so nothing long can be in front of it. Requests arriving during the drain are answered `503`, which the loop thread may legally write itself. **A shutdown that drains its workers still has to stop making them.**

**The seam had to close or the bug walks back in.** `RegisterRoutesFn` now takes a `Router` rather than the raw app, and `mm::api::sendTimedJson` — the timed helper every device endpoint used — was deleted with the last of its callers. A shared header offering the blocking shape, plus `libs/example` demonstrating it, is precisely how a fixed bug returns.

**A near-miss worth recording, because its failure mode is invisible.** A scripted conversion silently deleted nine routes: the removal matched, the insertion anchor did not (clang-format had reflowed the file between runs), and **deleting routes still compiles and still passes every test**. Nothing caught it but two now-unused helpers. Recovered from the last commit; conversions then went through a helper that verifies the insertion anchor *before* removing anything and asserts the route count afterwards. That guard immediately found a second error: the inventory was 74, not 70 — four routes are written `.post(\n  "/path"` and the counting regex only matched the same-line form. **A refactor whose failure mode is invisible to the build and the test suite needs its own invariant**, and the invariant needs to be checked against reality before it is trusted.

## Session 2026-08-08 — The lock inventory: `busMutex_` carries two invariants, the recorder ring has three writers, and every module holds its own mutex across the bus lock (review)

A week of bugs that were all the same bug — cached reads stuck behind the control-plane lock, the monitoring stall, the `supportsCoe` poll, the shutdown race, the handler-blocks-the-loop freeze — is not a run of bad luck. It is the shape of locking nobody can hold in their head at once. So this session inventoried all of it instead of adding to it, from the call graph rather than the member declarations, because the declarations were the least informative input and the Doxygen comments were the actively misleading one.

**Seven mechanisms.** The driver's `controlPlaneMutex_` (one socket transaction; never taken by `exchangeProcessData`); `DeviceManager::busMutex_` (a `shared_mutex`: 5 exclusive sites, 18 shared, plus `withDevice`); `Device::parametersMutex_` (20 sites); `MonitoringManager::mutex_` + cv; `ParameterRefresher::mutex_` + cv; `ProcedureManager::mutex_` plus a per-run `errorMutex_`; and the atomics — `ProcessData::{image,exchanging,outputSlots,lastWkc}`, `ProcessDataRing::{head_,seqWords_}`, the two generation counters, `Router::stopping_`/`aborted`, the `GameLoop` health counters.

**The acquisition order is acyclic, and that is the point.** `monitoringMutex_ → {refresherMutex_, busMutex_}`, `procedureMutex_ → busMutex_`, `busMutex_ → parametersMutex_ → controlPlaneMutex_`. There is no deadlock to find here. **Every defect below is a scope or liveness defect, which is why looking for cycles would have found nothing** — and why the one deadlock this codebase did produce (the per-device `coeSequenceMutex_`, re-entered from a callee two levels down) came from a call graph and not from a lock-order chart.

**Defect 1 — the recorder ring has three writers, not one, and the third one frees the buffer.** `ProcessDataRing` documents "single writer (the RT loop), many concurrent readers". `allocate()` and `clear()` are the undocumented third: they run from the control plane (`remapProcessImage`, `scan`, `reset`) under `busMutex_` exclusively, and they *release the storage* — `buffer_.clear() + shrink_to_fit()`, and `seqWords_` swapped out. Every ring reader is covered by that lock except three: `DeviceManager::recorderHead`, `recorderOldestSeq` and `readRecord` take **no lock at all**, and their Doxygen says "Thread-safe, lock-free". They are the sampler's entry points. `serializeDumpSpan`, the other reader, takes it shared — so the inconsistency is inside one class, between two readers of the same structure.

So: an active monitoring plus a SAFE-OP→PRE-OP→SAFE-OP bounce (or `POST /api/reset`). `flushEntry` reads `head`, then decodes `[cursor, head)` — thousands of records, milliseconds of wall time, **a wide window and not a narrow one** — while `clear()` frees the buffer underneath it. `readRecord` then indexes `seqWords_[slot]` on swapped-away storage and reads `buffer_.data() + slot * stride_` on freed memory; the `capacity_ == 0` guard is itself a non-atomic read racing the write. **The lock-free reader protocol is correct against the producer it was designed against, and says nothing about the re-allocation it was not.** That the comment claims otherwise is the whole lesson: a thread-safety comment scoped to one adversary reads as a general guarantee.

**Defect 2 — `driver_` is read unsynchronised from the RT thread, by a check that does not need to exist.** `exchangeProcessData()` opens `if (!driver_)`, a plain `unique_ptr` read, while `init`/`reset` write it under a lock the RT thread never takes. Benign on real hardware, formal UB, TSan-visible — and redundant: no image can be published without a driver, and `reset()` calls `stopExchange()` before `driver_.reset()`, so the seq_cst image load two lines down already covers it. Deleting the check removes the race outright.

**Defect 3 — every module holds its own mutex across a call into `busMutex_`.** The sampler holds `MonitoringManager::mutex_` across `flushEntry` → `deviceExchangesProcessData`/`value`/`pdoSampleSpec` (shared). `ProcedureManager::startRun` holds `mutex_` across `withDevice`. So a multi-second `transitionToState` does not merely stall monitoring — it stalls `GET`/`POST /api/monitorings` and every procedure endpoint, because those threads queue behind a mutex whose holder is queued behind the bus lock. **The stall is one lock deep in the diagnosis and two locks deep in the symptom**, which is why the earlier hardware session found the sampler and missed the endpoints.

**The structural finding: `busMutex_` carries two unrelated invariants under one name.** **(A)** `devices_` is not being rebuilt, so a borrowed `Device&` stays valid — cheap, wants to be held for a long time, is exactly what `withDevice` needs. **(B)** exclusive use of the bus and the IOmap for a re-map — expensive, wants to be held briefly. Sharing one lock means a procedure holding A for minutes (auto-tuning through `withDevice`) blocks a `scan` needing B, and from there the outcome is decided by `std::shared_mutex` contention behaviour, which the standard does not specify and which differs across the three platforms we ship: libstdc++ maps to `pthread_rwlock_t`, whose glibc default kind is reader-preferring, so a stream of short reads can starve the pending writer indefinitely; Windows `SRWLOCK` is documented as unfair and can convoy readers behind the pending writer instead, stalling the whole device API for the procedure's duration. **Same code, opposite failure, and no comment can pick a side because there is no side to pick.** Splitting A from B is what makes the platform question stop mattering: no lock is then both long-held-shared and contended-exclusive.

**A documented bound that is true per transaction and false end to end.** `parametersMutex_` is described as held "across a single mailbox transaction … a concurrent cached read waits at most that long". But `readParameter` holds it while *waiting on* `controlPlaneMutex_`, which queues behind arbitrary other traffic — an object-dictionary sweep, an FoE transfer's per-packet locks. The transaction is bounded; the wait for the right to perform it is not.

**The plan, in order.** (1) Fix the ring use-after-free: smallest correct fix is a shared `busMutex_` acquire in the three recorder accessors, matching `serializeDumpSpan` — one acquire per flush, not per record, and it makes the comments true by making the code match them. The better fix, a generation/epoch inside `ProcessDataRing` so readers detect a re-allocation lock-free, belongs with (3). (2) Delete the `driver_` check. (3) Split `busMutex_` into a `deviceSetMutex_` (borrow lifetime; only `scan`/`reset` exclusive) and a bus-use lock. (4) Then the rule "never hold a module mutex across a call into `DeviceManager`" becomes enforceable rather than aspirational — `flushEntry` and `startRun` both violate it today and both are mechanical to fix by snapshotting under the lock and releasing before the call. The PDO-mapping write serialisation deferred in July lands inside (3), not before it.

**1 and 2 are unambiguous bugs and small; 3 is a design change and wants buy-in.** Recorded here so the split is argued once.

## Session 2026-08-09 — Splitting `busMutex_`: a lock over an *activity* and a lock over *lifetime*, and why a borrow no longer blocks an AL transition (as-built)

Steps (3) and (4) of the previous session's plan, plus the two mechanical scope fixes. The framing that made it tractable was noticing the two invariants are not two halves of one thing: **(A) is a guard over data, (B) is a mutual-exclusion token over an activity.** B protects no member at all — nothing is unsafe to read while it is held; it exists only so two operations cannot drive the bus at once. That asymmetry is why the pair could not be named symmetrically, and it decided the shape.

**`busOperationMutex_` (plain `std::mutex`) — one control-plane operation at a time.** Held for their whole duration by `init`/`scan`/`reset`/`configureProcessData`/`transitionToState`/`writeDevicePdoMapping`. No reader and no borrower ever takes it. The load-bearing consequence is a second-order one: **because the device set can only be rebuilt by an operation holding this, holding it is by itself sufficient to keep `devices_` and `driver_` stable** — so `transitionToState`, the multi-second offender, needs no reader-visible lock for its AL wait at all.

**`deviceSetMutex_` (`shared_mutex`) — `devices_` and the process-data runtime are not being rebuilt or freed.** The only lock readers and borrowers take. Exclusive in `init`/`scan`/`reset` (which genuinely invalidate every `Device&` in flight) and in the **publish window** inside `remapProcessImage` — the slot swap, ring re-allocation, generation append and image store. Everything else a re-map does, the IOmap rebuild and the per-device PDO-mapping SDO reads, now runs outside it, so the sampler keeps flushing through a re-map and only stalls for the ring allocation.

Order: `busOperationMutex_ → deviceSetMutex_ → parametersMutex_ → controlPlaneMutex_`. Still acyclic, one edge longer.

**The contested consequence, stated plainly: a borrow no longer excludes an AL transition.** A `withDevice` borrower holds `deviceSetMutex_` shared; `transitionToState` no longer takes it. So a minutes-long auto-tuning run and a user pressing "go to INIT" now interleave — the transition proceeds and the procedure's next bus transaction fails against the changed state, where previously the HTTP request hung for the procedure's whole duration. **This was accepted rather than tolerated.** The user asked for the bus to stop and the bus stops; the safety invariant that actually matters (a borrowed `Device&` never dangles) is untouched, because only `scan`/`reset` destroy devices and those still wait for the borrow. A hung request that might convoy the entire device API on Windows is not a feature worth preserving. The `withDevice` warning had to grow a second clause to say so, since the old one implied more exclusion than it ever promised.

**What the split does *not* buy, recorded because it is the natural thing to assume it buys.** It does nothing for process-data exchange during a firmware install. `exchangeProcessData()` takes no lock and never did — it is gated by the atomic image pointer alone — so no mutex has ever been able to stall the RT cycle. What pauses exchange is `stopExchange()` plus the whole-bus re-map when the flashed device rejoins SAFE-OP/OP, and that pause is the same length as before. **What improves is everything around the exchange**: monitoring keeps streaming and the read endpoints stay responsive across the BOOT and PRE-OP transitions, where before they stalled for each one. Observability during the install, not throughput.

**One lock's exclusive window shrank; one operation's lock got *stronger*.** `writeDevicePdoMapping` now takes `busOperationMutex_`. It rewrites `flatPdoMapping_` — the same field `remapProcessImage` rewrites and `buildProcessImage` reads — and a shared lock never excluded that. The PDO-mapping write serialisation deferred in July fell out of the split rather than needing its own change, exactly as predicted.

**Two latent unsynchronised readers surfaced while mapping the sites, neither of them the thing being fixed.** `processImageInfo` took no lock while walking `generations` and dereferencing `back()`, on the strength of a comment asserting a single control-plane thread. `deviceStates`/`deviceDiagnostics`/`dcSync` took none while dereferencing `driver_` and resolving positions against `devices_`. Both now take the shared lock. **An inventory finds these; reading each function in isolation does not, because each one's comment was locally plausible.**

**The two scope fixes, and the sibling that already did it right.** `MonitoringManager::run` held `mutex_` across `flushEntry` — the longest thing the class does, a walk over every recorded cycle — while `ParameterRefresher::pollDue`, structurally its twin, snapshots under the lock and releases before calling `DeviceManager`. Same file layout, same problem, one solved. The flush became snapshot / detach / commit: `takeDue` copies each due entry's flushable state, `flushDetached` does the ring walk and the publish with no lock held, `commitFlush` writes the advanced cursor back. Entries carry an `epoch` so a remove-and-re-create of the same topic mid-flush cannot have a stale cursor written onto the new registration — the one hazard releasing the lock introduces, and it wants an explicit answer rather than a reference-stability argument about `std::map`. `ProcedureManager::startRun` was the easier half: resolve the device and capture the topology generation *before* taking `mutex_`. Capturing the generation earlier is also the more accurate reading, since it is the generation the device was actually resolved under.

**On the names, because the first pair was wrong.** `controlMutex_` sat one word from the driver's `controlPlaneMutex_`, and the two appear in the same call stack constantly — `transitionToState` takes one, every SDO underneath takes the other. Names that close invite reading them as one concept at two layers, when the distinction is exactly what a reader needs: `controlPlaneMutex_` serialises a *single socket transaction*, the other serialises a *whole multi-transaction operation*. `busOperationMutex_` says operation, and "operation" is also the honest signal that it guards an activity rather than a field. `deviceSetMutex_` was kept despite slightly under-covering — it also guards `generations`, the ring storage and `outputSlots` — because every candidate spanning both was vaguer (`stateMutex_` collides with AL state; `runtimeMutex_` says nothing), and the unifying idea, "storage readers hold references into that the control plane reallocates," is a sentence and not a name.

**Still open, deliberately.** `parametersMutex_` remains held across a wait for `controlPlaneMutex_`, so a cached read of a device can still queue behind that device's multi-second FoE transfer — the "true per transaction, false end to end" bound from the previous session, untouched here because it is independent of the split. And `ParameterRefresher` could fold into `MonitoringManager`, deleting a mutex, a cv and a thread; it is the only genuine *reduction* available in the whole inventory, and a distant second in value to this split.

## Session 2026-08-09 — `parametersMutex_` is never held across bus I/O: snapshot, transfer, re-find (as-built)

The last item from the lock inventory, and the one whose sizing was wrong when it was first written up. The bound `parametersMutex_` documented — "held across a single mailbox transaction, so a concurrent cached read waits at most that long" — is true per transaction and false end to end, because the transaction first has to *acquire* the driver's control-plane mutex, which an FoE transfer holds for its entire multi-second duration.

**The scenario is the ordinary configuration, not an edge case.** The first estimate here was that this needs an unlikely coincidence, on the reasoning that a firmware install drives the device to BOOT, where `mailboxActive()` is false and the SDO path is never taken. That reasoning is right about firmware and wrong about the product: **any monitoring with an SDO-sourced parameter puts `ParameterRefresher` on that device continuously**, and reading files over FoE is a page users sit on. Background SDO polling plus a file read is the steady state. The refresher is then the thread *holding* `parametersMutex_` while parked on the control-plane lock, so the monitoring sampler's `value()`, `pdoSampleSpec`'s `dataType()`, and the Parameters page all queue behind it.

**What the lock was actually for, which the comment did not lead with.** Not the duration claim — *pointer stability*. `findParameter` hands back a `DeviceParameter*` and the code writes `p->value` after the bus call returns. One precision worth keeping: rehashing is not the hazard, since `std::unordered_map` is node-based and references to elements survive a rehash. The hazard is `initializeParameters` doing a wholesale `parameters_ = std::move(built)`, which destroys every node. So the fix is not "hold it for less time", it is "do not carry a raw pointer across the wire".

**Three phases, at four sites.** Decide under the lock (resolve the entry, serve from the live PDO image or the offline cache if that settles it, else capture the decode type), transfer with the lock released, then re-find and commit. `readParameter`, `writeParameter`, and the two `tryComplete` lambdas — which were near-identical copies and collapsed into one `Device::readObjectComplete`, so the split is written once rather than twice. It is the same shape as `ParameterRefresher::pollDue` and now `MonitoringManager::flushDetached`; the codebase has one answer to this problem in three places instead of three answers.

**The re-find forces a decision the old code did not have to make.** If the entry is gone, or its `dataType` differs from the one snapshotted, the dictionary was re-enumerated mid-transfer and those bytes were decoded under a type the object no longer declares. `readParameter` reports "re-enumerated during the read — retry" rather than caching a possibly-misdecoded value; the CA path drops the blob and falls back to per-subindex reads. Rare, but it is a real state that now has a written answer instead of an assumption. `writeParameter` is easier: it is cache-first, so the value is already committed and only `syncState` is provisional across the transfer — a reader mid-download sees the intended value, which is the contract it always claimed.

**A latent bug fell out of unifying the two lambdas.** `readAllParameters`' copy did not check `mailboxActive()` before attempting a Complete Access upload. Below PRE-OP that upload fails, and `CompleteAccessProbe::recordFailure()` latches `caSupport_` to `kUnsupported` for the device's whole lifetime — on evidence that says nothing about whether the slave supports CA. `readObject`'s copy did check. The shared helper checks, so the answer is now the same one in both.

**The tests were validated by re-introducing the defect, because otherwise they assert nothing.** Both new tests park a transfer inside the fake driver's `readSdo`/`writeSdo` and require a cached read to complete anyway. They passed immediately — which proves nothing on its own, so the lock was deliberately widened back over each transfer in turn and each test was confirmed to fail (blocking out its full five-second timeout) against its own defect and to pass against the other. **A concurrency test that has never been seen to fail is decoration.** A first attempt at the injection did not compile, and the run then silently used the stale binary and reported green — the failure mode of the whole exercise in miniature.

**The invariant is now checkable rather than asserted**, which is why it is stated as a rule on the member: every `parametersMutex_` scope in `device.cc` was scanned for a bus call and there are none. The rule it imposes on future callers is the one the fix turned on — a `DeviceParameter*` must never cross the release — and a miss on re-find must be handled rather than assumed impossible.

## Session 2026-08-09 — The value lives on the parameter: one lock-free cell, and a cyclic task that cannot tell PDO from SDO (design)

The goal is a user-authored `CyclicTask` that is production code — four wheels of an autonomous vehicle — written by a controls engineer rather than by someone who has read `docs/LOCKING.md`:

```cpp
void execute(const CycleContext&) override {
  auto* wheel = deviceManager_.findDevice(3);
  const int32_t speed = wheel->value<int32_t>(0x606C, 0);  // PDO-mapped
  const int16_t temp  = wheel->value<int16_t>(0x2030, 5);  // SDO, refreshed in the background
  if (temp > kLimit) { wheel->setValue<int32_t>(0x60FF, 0, 0); }
}
```

**The contract is one sentence: no lock, no allocation, bounded time, and no difference in the call between an object that is in the process image and one that is polled over SDO.** Whether a value is PDO-mapped is a *commissioning* decision — an engineer leaves temperature out of the image precisely because it changes slowly — and it must not leak into the control code. Nothing about `0x2030:05` being slow should change how the program that reads it is written.

**None of it is implementable today, and the gap is not small.** `findDevice` walks a vector `scan()` rebuilds. `withDevice` takes `deviceSetMutex_` shared, so a re-map's publish window blocks the RT thread on a non-RT one — priority inversion in the 1 ms loop, which disqualifies it for this outright. The PDO read path reaches the newest value through `ring.readRecord`, which fills a `Record` holding **two heap vectors of the whole IOmap** and then allocates a third in `extractBits`, to extract four bytes: three mallocs per parameter per cycle. The SDO path reads `parameters_` under `parametersMutex_`, a plain blocking mutex. Outputs are the one direction already close — `outputSlots` is lock-free and non-allocating from the store down, though `writeParameter` above it takes the mutex and allocates.

### The value belongs on `DeviceParameter`, and a side table was the wrong answer

The first draft of this design put the cells in a `ValueTable` owned by `DeviceManager`, published atomically and retained in generations, addressed by handle — the same machinery `ProcessImage` uses. **Rejected, and the reason generalises: the ownership chain already says where a value goes.** A parameter's value lives on the parameter; the parameter is held by its device; the device is held by the manager. A side table duplicates that chain, forces every reader to learn a second addressing scheme, and puts the manager in the business of holding values it has no other reason to know about. The object model was right and the design was arguing with it.

So `DeviceParameter` gains the cell:

```cpp
std::atomic<uint64_t> bits;   // ≤8 wire bytes, little-endian — the packing outputSlots already uses
std::atomic<uint64_t> stamp;  // monotonic; 0 = never written. RT cycle sequence, or refresher tick
```

**The cell records the value and never its origin.** That single omission is what makes the API source-agnostic: there is no branch for a caller to get wrong, because there is nothing to branch on. Every parameter gets one — sixteen bytes against a few thousand entries is not worth a registration step, and opt-in would mean a task's read silently returning nothing because someone forgot to subscribe.

**Scalars only.** Strings and byte arrays do not appear in a process image and are not what a cyclic program reads; they keep the existing variant path. For the end user this distinction never surfaces.

**One wrinkle to plan for:** `std::atomic` is neither copyable nor movable, so `DeviceParameter` needs a hand-written copy constructor and assignment that load the cell. `Device` stays movable regardless, because `std::unordered_map` is node-based and moving the map never moves an element.

### What makes it sound is the lifecycle, not machinery

The reason a raw `Device*` and a raw `DeviceParameter*` are safe here — and the reason the earlier "publish and retain the device set" phase evaporates — is that **the device set and the parameter maps are not rebuilt while the loop is running.** A program initialises the driver, scans, enumerates, brings the bus to OP and registers its SDO objects with the refresher, and *then* starts the game loop. Cyclic programs deal only with what happens during operation.

That is a precondition, written down and relied upon, rather than a guarantee bought with published generations and retained snapshots. It is what lets the RT fast path skip `parametersMutex_` at all: the map is not being replaced, so there is nothing to be raced against. **A `scan()` or a `initializeParameters` issued while the loop runs breaks it**, and the honest answer is that this is out of contract rather than defended against — the same posture the codebase already takes toward tearing down a driver mid-cycle.

The practical consequence for a task author is better than the general case anyway: resolve the device and the parameters **once**, at task construction, and hold the pointers. The per-cycle cost is then a single relaxed atomic load per signal, with no lookup at all. Binding signals once and then looping is how this code gets written regardless.

### Who fills the cell — and why every path does

Not only the two fast producers. **The ordinary control-plane `readParameter` / `writeParameter` update the cell too**, which is what keeps one value rather than two:

- **PDO-mapped inputs** — the RT loop, in `exchangeProcessData`, immediately after the wire exchange, while the raw input image is already in hand. Bounded by the image, no allocation, no lock.
- **SDO-sourced** — the `ParameterRefresher`, on its own thread at its own cadence, after each successful poll.
- **A user's read or write over HTTP** — the existing paths, which end in an SDO transfer, land in the same cell on the way through.

**A property falls out of doing the PDO decode on the RT thread that is worth having deliberately: within one cycle, every PDO cell a task reads comes from the same exchange.** The same thread wrote them earlier in that cycle, so they are sequenced before the task's reads. Four wheel speeds are one coherent snapshot with no snapshot machinery. SDO cells are "latest known", which is exactly what they are and all they can be.

**A write reads back as itself.** Setting target position must make the next read of `0x607A:00` return what was set, not the value from the last frame — so a write stores the cell as well as staging the wire bytes. That in turn suggests folding `outputSlots` into the cells altogether: the composer would read each output object's cell directly, keeping the single-composer property that makes bit-packed objects sharing a byte safe (Design B) while leaving exactly one home for a value. Not settled — `outputSlots` works, and this is a simplification rather than a fix.

### The access surface stays as it is

**`findDevice` goes back to public and stays non-locking**, because a cyclic task needs a device without taking a lock and that is the call that gets one. Making it private — done earlier in the day on the strength of a use-after-free in the HTTP layer — solved that bug by removing the API's reason for existing, and is to be reversed as the first step of this work. The HTTP fix that mattered is the one that stays: handlers borrow for the duration of what they do, which is correct for the control plane whatever the RT surface looks like.

**`withDevice` stays for the control plane and procedures**, where holding the lock across a multi-second operation is the point.

**No facade.** An `RtDevices` type exposing only the lock-free surface would prevent a task calling `scan()` from the RT thread by construction, but it is another class earning its keep only against a mistake the contract already forbids. Cyclic tasks get `DeviceManager&`, and the engineers writing them know what they are doing.

**No third-party task wiring yet.** `main.cc` remains the only place a task is constructed; an example task is a later piece.

### Phases

1. **Non-allocating primitives.** An `extractBits` overload writing into a caller-provided span, and a ring read that fills spans rather than building a `Record`. Purely additive, prerequisite for everything.
2. **The cell on `DeviceParameter`**, plus the copy constructor it forces, and `Device::value<T>()` / `setValue<T>()` as lock-free non-allocating typed accessors.
3. **Producers.** The RT decode after exchange (with each image entry's owning `DeviceParameter*` resolved at publish time, so the RT loop does no lookup), the refresher's store, and the control-plane paths.
4. **Read-back and possibly the `outputSlots` fold.**
5. **Documentation and a worked example task** — the authoring story, including the lifecycle precondition stated as a precondition.

### Open

The naming collision between the existing `Device::value(index, subindex)` (locked, returns a `DeviceParameterValue` variant) and the new typed lock-free `value<T>()`; the old one can be reimplemented over the cell for scalars, which is probably the migration. Whether a write to a non-output-mapped object from a cyclic task should be rejected, or queued for the refresher to push over SDO — it must never issue one inline. And the per-cycle cost of the decode loop, which lands in the same budget as the wire round-trip and will show up on `GET /api/game-loop`.

This partly supersedes Session 2026-07-09's "profile view is RT-callable", which routed RT access through `Device::readValue`/`writeValue` dispatching on bus state. A `Cia402Drive` over cell-backed reads becomes RT-callable without that dispatch, so the two want reconciling when this lands.

## Session 2026-08-09 — Sanitizers: the question is which *binary* is instrumented, not which preset exists (plan, nothing lands)

Four hours after ThreadSanitizer was reverted, the wider question: do ASan/UBSan/LSan/MSan/TSan, Valgrind and the profilers earn their keep here, and how would they be introduced. **Nothing in this session lands — it is written down so the argument is made once and the tiers can be picked up in any order later.**

**The revert is the input, and it indicts two different things.** The concurrency harness was worthless — it passed whether or not the locking was correct, and it exercised none of the paths that matter (`exchangeProcessData` against a re-map, `busOperationMutex_`, the real driver's `controlPlaneMutex_`). Deleting it was right. But the *tool* was never given a workload: TSan reports races on code that actually runs concurrently, and it was pointed at a suite where about 5% of tests start a second thread. Those are separate verdicts, and conflating them would retire a tool on the evidence of a harness.

**The constraint that decides everything below: a dynamic tool only inspects code that executes, and the code most in need of inspection here is the code `ctest` cannot execute.** The SOEM driver, the RT loop, the 32-worker HTTP pool racing the sampler thread and the procedure `jthread`s — all of it needs a bus. So the question is not which preset to add. It is **which binary is instrumented**. A preset wired to `ctest` mostly sanitizes the ESI parser; a preset whose *product* is the binary run on the bench sanitizes the thing that actually breaks. The on-device binary is already run by hand, so this is a build-flag change rather than a test-harness change — which is precisely what the reverted attempt was not.

**ASan — worth it, with one correction that must not be lost.** A real slice of the 697 tests are byte-buffer parsers: `process_data_ring`, `process_data_dump`, `firmware_package`, `base64`, `sii`, `device_parameter` decode, plus the ESI parser chewing a 1.9 MB vendor file it is *deliberately tolerant* of (wrong-length `hexBinary`, absent `<SubIdx>`, both the current and obsolete `Min/MaxData` branches). Tolerant parsers are where off-by-ones live. It also owns the heap-use-after-free class outright — the recorder-ring re-allocation freeing storage under the sampler is textbook ASan. **It would not have caught the FMMU bug.** That was a write past `ec_slavet.FMMU[EC_MAXFMMU]` into adjacent fields of the same struct; ASan places redzones *between allocations*, not between members, and intra-object detection needs `-fsanitize-address-field-padding` — clang-only, blocklist-dependent, and unable to relayout a third-party struct in any case. Recorded because "ASan would have caught our worst memory bug" is the tempting and false claim.

**UBSan is the best ratio of the set, and it was not on the original list.** Near-zero cost, composes with ASan in one preset, and it targets what this codebase does constantly: decoding wire bytes into typed values and enums (`enum`, `bool` — an AL state or object code loaded out of a slave's byte is exactly the check), shift-width and signed-overflow in index and bit math, null dereference on the raw `ProcessData*`/`ParameterCache*` handoffs, out-of-range float→int in scaling. The 25 `reinterpret_cast`s are the alignment candidates; the 17 `memcpy` sites are the *correct* idiom and will not fire. Run it with `-fno-sanitize-recover=all` so a finding aborts rather than logging, and drop `vptr`/`function` if third-party noise appears.

**LSan comes free with ASan, yields little, and has one useful side effect.** RAII discipline means the unit suite will find close to nothing. On the *binary* it will report the retained process-image generations — retained until `reset()` by design — as leaks. Either `detect_leaks=0` for bench runs or treat it as a forcing function to make shutdown release cleanly. Not worth effort beyond "it is on by default, do not fight it".

**MSan — won't do, and the reason is structural rather than budgetary.** It requires every library in the process rebuilt instrumented, including an MSan-built libc++ (not libstdc++): uWebSockets, usockets, OpenSSL, libcurl, SOEM, spdlog, pugixml, gtest, behind a custom vcpkg triplet. Days of work. And the payoff class is awkward here in a way that survives the effort: **the IOmap is filled by the kernel via `recvfrom`, which MSan cannot see without manual annotation** — the highest-risk buffer in the system is the one it would be blind to. Part of the class is already covered by `-Werror`, and Valgrind covers the rest for free. The trigger that would reopen it: an MSan-instrumented libc++ arriving in vcpkg *and* a real uninitialised-read bug that nothing else found.

**TSan — the preset should return, pointed somewhere else.** Same clang pin (GCC still refuses `atomic_thread_fence` under `-fsanitize=thread`), same single ring suppression, but aimed at the bench binary with `hil/api` hammering the HTTP surface rather than at a synthetic harness. Two things to know before that run. **The recorder ring can never be validated by it** — neither implementation models a standalone fence, so the seqlock stays permanently suppressed, and that is the most delicate protocol in the tree; the hole is real, not a formality. And **capabilities matter**: `mlockall(MCL_CURRENT | MCL_FUTURE)` against TSan's shadow mapping is a known OOM, so the sanitizer binary wants `cap_net_raw,cap_net_admin` **only**, with `cap_ipc_lock` and `cap_sys_nice` dropped. `setRealtimePriority()` is already best-effort and independent per step, so the lock simply fails, the existing warning prints, and the bus still works — the graceful-degradation design pays for itself here without a new flag. Raise `gameLoop.periodUs` to ~10 ms for the run; skips under instrumentation are expected and harmless.

**Valgrind memcheck is the cheap answer to the class MSan would have covered.** It finds uninitialised-value reads without rebuilding anything, because it instruments the binary rather than the build — no preset, no triplet, just `-g -O1`. The 20–50× slowdown makes the RT loop hopeless, but the unit suite and short binary sessions are fine. Never combined with ASan. Helgrind and DRD are skipped: noisier than TSan and weaker on C++ atomics, so they would add reports without adding confidence.

**Profiling is a different axis, and the RT question is already better instrumented than a profiler would answer it.** `hil/jitter_bench` plus `GameLoop::health()`'s task-time last/max/avg answer the latency question directly; a sampling profiler on a 1 ms loop mostly rediscovers that the blocking `ecx_receive_processdata` dominates, which is already written down. What is genuinely missing is elsewhere: `perf record -g` and a flamegraph over the *non-RT* CPU costs — `POST /api/esi/parse` (1.9 MB of XML in, 3.26 MB of JSON out), the sampler's per-flush decode of thousands of ring records, the JSON serialisation in the HTTP handlers; **heaptrack or massif against the Pi 5 RSS budget**, because the ring is ~38 MB per drive, `mlock`ed, on a 4 GB board and nobody has measured the steady state; and `cyclictest` in `rt/` provisioning, which measures the *machine's* achievable determinism — exactly the "your configured period is too aggressive for this hardware" question the overrun policy documents but cannot answer from inside the process.

**Two static complements, because static analysis covers the hardware-only paths no dynamic tool can reach.** `clang-tidy` is installed by `tools/install-deps.sh` and there is **no `.clang-tidy` file and no target** — a dependency being paid for and not used, which is the cheapest thing on this whole page to fix. *(Done — see Session 2026-08-10. That session also withdraws one argument made below: `-fsanitize=enum` does **not** recover the `EnumCastOutOfRange` coverage, because the enum in question fixes its underlying type and there is no UB to trap.)* And coverage (`gcovr`/`llvm-cov`) is arguably the prerequisite for every dynamic tool here: it is what says whether ASan reaches a line at all, rather than leaving a green run to imply it.

**The tiers, in the order they would be picked up.** *Tier 0 (~1 h)*: an `x64-linux-asan` preset — clang, `-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -O1 -g`, wired into `tools/test.sh`, with no suppression file until something fires. *Tier 1, the actual payoff (~half a day of bench time)*: run that binary against real drives with a `motion-master.asan.jsonc` raising the period, capabilities trimmed to net-raw only, and `MM_SKIP_DOCKER=1 pnpm --filter motion-master-api-tests test` pointed at it. *Tier 2, conditional*: TSan returns with the same preset and suppression, aimed at the Tier 1 workload; trigger is the next locking change, and once before the beta. *Tier 3, occasional*: Valgrind memcheck over the unit suite. **Won't do**: MSan, Helgrind, DRD.

**CI stays out of the push path**, which is the earlier revert's reasoning and it holds: the suite costs ~15 s plain against ~2 minutes under TSan, and what a sanitizer checks is not decided by one run per push. A `workflow_dispatch` plus a weekly `schedule` job for ASan/UBSan over the unit suite is the middle ground — it catches parser regressions without a human remembering, and taxes nothing.

**Platform notes, since Linux is not the only shipping target.** MSVC has `/fsanitize=address` and it is worth a pass on the Windows build specifically, because `CyclicTimer`'s QPC path and the bundled-config discovery are code no Linux run touches. macOS clang carries ASan, UBSan and TSan; LSan is unavailable on Darwin arm64, so a shared preset must not assume it.

## Session 2026-08-10 — clang-tidy: the bug-finding checks only, one real defect, and a check that had to be switched off (as-built)

Closes the cheap half of the previous session's "two static complements": there is now a `.clang-tidy` at the repo root, a `tidy` target driven through `run-clang-tidy`, and the tree has been swept clean — **99 translation units, zero findings, with `WarningsAsErrors: '*'`**, and the 697 tests still pass. The previous session ranked this first for a reason worth restating, because it is the one property no dynamic tool on that page has: **static analysis reads the hardware-only paths.** `SoemFieldbusDriver`'s error branches, the FoE and SDO decode, the AL-transition guard — all of it is invisible to ASan, TSan and Valgrind alike, because `ctest` cannot execute a line of it without a bus.

**The check list was chosen from a measurement, not from taste.** Over five representative translation units, `bugprone-*` + `clang-analyzer-*` + `concurrency-*` + `performance-*` + `misc-*` reported 560 findings — of which 497 came from three `misc-*` checks alone: `misc-include-cleaner` (228, an include-what-you-use pass that disagrees with how this codebase groups headers), `misc-non-private-member-variables-in-classes` (178, firing on every `Config` and every DTO, all of them public data by design), and `misc-const-correctness` (91). None of the three names a defect, so `misc-*` is off entirely — a check that reports 89 things in one file trains the reader to skim, which costs more than the check is worth. The style families are off for a different reason: layout is settled by `.clang-format`, includes and naming by `CPPLINT.cfg`, and the rest by `-Wall -Wextra -Wpedantic -Werror`, and a second voice on a settled question only disagrees.

**One real defect, and it is the kind only this class of tool finds.** `concurrency-mt-unsafe` on `std::strerror`, which returns a pointer into a single static buffer. The reason it matters here is not the general rule but *where the calls are*: the failures being described happen on the 32-worker HTTP pool and on the procedure `jthread`s, not only during single-threaded startup, so two concurrent failures could hand each other's text to two different clients. Fixed with a new `mm::core::errnoMessage()` — `std::system_category().message()`, an owned string over the reentrant `strerror_r`/`strerror_s` — rather than a suppression, and the shipped call sites in `platform.cc` and `soem_fieldbus_driver.cc` now use it. `apps/playground` keeps `std::strerror` behind a NOLINT with the reason written at the call site: it is a single-threaded scratch binary, and reaching for the helper would make it link `mm::core` for one message.

**The near-miss is worth more than the hit, because it is where the analysis was wrong about itself.** `clang-analyzer-optin.core.EnumCastOutOfRange` flagged `resolveSiiCategoryType` forming a `SiiCategoryType` from a raw EEPROM word before knowing the word was an enumerator, and the first draft of both the fix and its comment called that undefined behaviour. **It is not.** `enum class SiiCategoryType : uint16_t` fixes its underlying type, so the conversion is well defined and value-preserving for every `uint16_t`; UB on an out-of-range cast applies to enums *without* a fixed underlying type. The old switch-on-the-cast was correct and returned exactly the same answers. **The rewrite was reverted, and the reason is the more useful half of this session.** A lookup table is defensible on style grounds — it states intent at a validator boundary and forms no enum object from a non-enumerator — but that is hygiene, and it is a trade rather than a free win: **a `switch` over the enum is exhaustiveness-checked by `-Wswitch`, and a table is not.** Measured, not assumed: adding an enumerator that neither form handles makes the switch fail the build (`enumeration value 'NewlyAdded' not handled in switch`, and warnings are errors here) while the table compiles silently and simply returns `nullopt` — a real SII category read as unknown, with no crash and no failing test. That asymmetry decides it, because `SiiCategoryType` lives in the header and the resolver does not, so nothing else would prompt whoever extends the enum. The comment at the site now says both things, so that neither the exhaustiveness nor the safety of the wide cast gets "fixed" again; `.clang-tidy` records the report as this check's one measured example here, and a false alarm.

**That check is now off, and the coverage does not come back where the previous session said it would.** Its only remaining reports are inside uWebSockets' `TopicTree.h`, which we cannot annotate — and **clang-tidy's header filters do not apply to static-analyzer diagnostics.** Verified: neither `ExcludeHeaderFilterRegex` nor `--exclude-header-filter` suppresses them, and the vcpkg include is already `-isystem`. Two unfixable reports would leave the target permanently red, which is worth more than a check with no defect to its name here. And the tempting consolation is wrong: **UBSan's `-fsanitize=enum` is not a substitute**, because for a fixed-underlying-type enum there is no undefined behaviour to trap. This does not strengthen the sanitizer plan's UBSan case; it removes an argument from it.

**`bugprone-unchecked-optional-access` is a measurement about the test idiom rather than about the code.** 207 findings across the tree, *every one of them in a test and none in shipped code*. Its dataflow engine cannot see through gtest's `ASSERT_` macros, so the mandated idiom — `ASSERT_TRUE(r) << r.error();` then `*r` — reads to it as an unchecked dereference. A check that is silent on the code it was meant to guard and noisy on the code that already proves its own precondition is measuring the wrong thing, so it is off by name.

**Two NOLINT mechanics, both learned the hard way, both now in CLAUDE.md.** First, `NOLINTNEXTLINE` must be the **last** comment line before the code: it applies to the line immediately following the marker, so explanatory prose written *after* it pushes the code out of range and the suppression silently does nothing. That is the worst available failure mode, because the marker is still sitting there looking like it worked. Reason above, marker last. Second, **cpplint parses `NOLINT` markers too** and rejects a `NOLINTEND` whose category it does not recognise (`Not in a NOLINT block`), so a clang-tidy check name can never appear in the block form. Per-line markers are understood by both tools; the block form is unusable here.

**The suppressions that stayed are statements, not concessions.** Twenty markers, each with its reason at the site: mostly `concurrency-mt-unsafe` on `getenv` and `std::exit` in paths that run before any thread exists (the argument parser refusing to start, cert-path discovery); three `bugprone-exception-escape`, which are statements about the standard library rather than about this code — nothing here throws deliberately, but a `std::string` or `nlohmann::json` allocation can raise `bad_alloc`, and terminating at startup is the honest outcome where a catch-all could only print and exit non-zero; `bugprone-throwing-static-initialization` on `kValidStateTransitions`, whose only throw is allocating a five-entry table before `main`; `performance-trivially-destructible` on `~CyclicTimer`, which is trivial *only in the build being analysed* — the Windows implementation closes a waitable-timer handle there, so defaulting it would leak a kernel object on the one platform that owns one; and `bugprone-branch-clone` on `Cia402Drive::enable`, where merging `kQuickStopActive` with `kNotReadyToSwitchOn` into one fall-through would delete the distinction that makes the safety state auditable. Checks that fire only a handful of times are deliberately left **on** even where the current hit is defensible — `bugprone-command-processor` on the one `popen`, `bugprone-exception-escape` on `main`. A finding worth answering at the call site is not a reason to blind the check.

**One `CheckOptions` entry, and a version trap under it.** `bugprone-unused-return-value.AllowCastToVoid` is on, because an explicit `(void)` is how C++ spells "ignored on purpose" and the tests use it exactly that way — `(void)dm.init(...)` in a test whose subject is what happens *after* a failed init. Without it the check can only be satisfied by inventing an unused variable, which says less; a bare unignored call still reports. Worth knowing that `CheckOptions` has both a list form and a map form depending on clang-tidy version, and a form the binary does not understand is **silently ignored** rather than rejected — so the option was confirmed live with `clang-tidy --dump-config` rather than assumed from a clean run, which a clean run cannot distinguish from an option that never applied.

**The mechanical residue, valued honestly.** `reserve` before a push loop, `const&` on parameters that were taken by value, `const` removed where it silently blocked a move (`return normalized`, and `std::move` on a `const nlohmann::json` copying instead of moving), and redundant `optional` unwrap-then-rewrap. Individually near-worthless; collectively it is the price of `WarningsAsErrors: '*'` and it is paid once. **The optional rewrites are the only ones that needed care rather than pattern-matching**: each is safe only because the enclosing `if`/`continue` already proves the optional engaged, which is a per-site argument. Three of them (`esi.cc`, `somanet_drive.cc` ×2) were checked individually.

**What it does not buy, recorded because the assumption is natural.** It would not have caught the FMMU bug — the same blind spot as ASan, and for a related reason: that was a write past `ec_slavet.FMMU[EC_MAXFMMU]` into adjacent members of one struct, and no static check models a third-party struct's layout that way. It says nothing about the recorder ring's seqlock, which remains the most delicate protocol in the tree and is unvalidated by clang-tidy, cppcheck **and** TSan alike (no standalone-fence modelling). And it is single-translation-unit: it has no opinion on lock ordering across the four mutexes, which the lock inventory established by argument and no tool has confirmed.

**Build-system prerequisites, easy to undo by accident.** `CMAKE_EXPORT_COMPILE_COMMANDS` is `ON` in the **base** preset so every preset emits a compilation database. `CMAKE_CXX_SCAN_FOR_MODULES` is `OFF` in the root `CMakeLists.txt` — this codebase uses no C++20 modules, and with GCC the scan injects `-fmodules-ts`, `-fdeps-format=` and `-fmodule-mapper=` into `compile_commands.json`, where clang-tidy rejects the translation unit outright. `ExtraArgs: ['-Wno-unknown-warning-option']` handles the same class one level down: four test targets pass GCC-only `-Wno-stringop-overflow`, which cost 41 parse errors before that line existed. The target is capped at **8 jobs** for the same reason the build is — analysis is as heavy as compilation and full fan-out on this workstation starves the desktop. Also fixed here: a 207-column line in `cmake/lint.cmake` that failed cmake-lint, introduced by the `tidy` target's own commit and repaired with `string(CONCAT ...)` rather than a disable comment.

**Open, and the honest weak point: `tidy` is not in CI.** `lint.yml` runs clang-format, cpplint and cppcheck; the tidy target is local-only, which is exactly how a clean tree rots. The cppcheck job exists because a local check that passes while CI fails is worse than no local check, and that argument transfers wholesale. The blocker is cost: a full run is minutes rather than seconds at 8 jobs (the slowest single unit, `somanet_drive_test.cc`, takes ~140 s on its own), and it needs a configured build plus a matching clang — the analysis is only as good as the compilation database it reads. The natural shape, unclaimed: keep it off the push path exactly as the sanitizer plan concluded — `workflow_dispatch` plus a weekly `schedule` — or run it over only the translation units a diff touches.

## Session 2026-08-10 — The atomic cell *is* the value: an RT surface a controls engineer can use (design)

Yesterday's session ("The value lives on the parameter") established the goal and the cell. This one settles the four questions it left open — lifetime, storage, who resolves what, and what a Tier-3 author actually calls — and **supersedes its lifetime half wholesale**. Nothing here has landed yet; it is written before the first commit so the argument is made once.

The target is the Tier-3 extension point ANNOUNCEMENT.md promises: clone the repo, add a `CyclicTask`, run real machine control on an RT host. The program is written by a controls engineer, not by someone who has read `docs/LOCKING.md`:

```cpp
void execute(const CycleContext&) override {
  Device* wheel = deviceManager_.findDevice(3);
  if (wheel == nullptr) { return; }                          // not on the bus this cycle — do nothing
  const auto temp = wheel->value<int16_t>(0x2030, 5);        // SDO-sourced, refreshed in background
  const auto state = cia402::decodeState(wheel->value<uint16_t>(0x6041, 0).value_or(0));
  wheel->setValue<int32_t>(0x60FF, 0, temp > kLimit ? 100 : 200);   // PDO-mapped, sent next cycle
}
```

**The contract is one sentence, and the existing naming convention already encodes it.** CLAUDE.md's rule is *bare noun = named-property access, `read*`/`write*` = bus transfer*. So `value<T>()` / `setValue<T>()` are the **entire** RT surface — never block, never allocate, never touch the wire — while `readParameter` / `writeParameter` stay the synchronous control-plane calls they are today. An author never has to ask which one is safe inside a cycle; the verb says it. That the convention landed years before this design and fits it exactly is a small piece of evidence that the convention was worth having.

### The cell is the storage, not a mirror of it

`DeviceParameter::value` stops being a `std::variant` sitting in memory. A scalar lives in `std::atomic<uint64_t> bits` — the object's raw little-endian wire bytes, LSB-aligned and zero-extended, the same packing `outputSlots` already uses — beside a monotonic `std::atomic<uint64_t> stamp` (0 = never written). Strings and byte blobs, which cannot fit a `uint64_t`, live behind an `std::atomic<const std::string*>` into a per-device arena of immutable, retained values; an RT reader loads the pointer and gets a `string_view` with no lock and no copy.

**`DeviceParameterValue` survives untouched as the interchange type in signatures — it just stops being where the bytes sit.** Every non-RT caller that wants one (JSON, an HTTP response, `readParameter`'s return, a monitoring flush) gets it from a `currentValue()` that reconstructs it: one `switch` on `dataType` turning eight bytes plus a type code into the right alternative. Nothing is lost, because the variant's tag never carried information `dataType` did not — `defaultValueForDataType` has always *chosen* the alternative from it, and `dataType` is immutable once the object dictionary is enumerated.

**The alternative — a cell mirrored alongside the variant — was rejected on the failure mode, not the cost.** With two homes every writer must remember both, and the one path someone forgets shows HTTP a stale number while the loop sees a fresh one: a bug that reproduces only under load and only in one direction. Reconstruction leaves exactly one place a value lives, so the divergence class does not exist. The price is a `switch` per JSON serialisation, which lands nowhere near RT. The blast radius is smaller than it looks — direct uses of `p.value` are eight sites across `device_parameter.{h,cc}`, `device.cc`, `parameter_cache.cc` and two test files; everything else already goes through `getValue<T>()` / `numeric()` / `setValue()`, whose signatures do not change.

**No `std::expected<T, std::string>` anywhere on the RT path.** Building the error string allocates. The RT accessors return `std::optional<T>`, and this is the one place the codebase-wide error convention cannot follow. A read of a device that is not exchanging returns the **last known value**, not `nullopt`: silently swapping a real number for nothing is how a control loop ends up acting on a fallback it never asked for. `stamp` and `Device::exchangesProcessData()` are there for a task that wants to decide differently.

### Lifetime: devices and their parameters live until `scan()` or `reset()`

An earlier draft kept every `Device` and every parameter map alive forever — append-only arenas, absent devices retained, identity-matched rescans — so that a raw pointer could never dangle. **Rejected, and rightly: it buys pointer safety with unbounded memory bloat, to defend against a misuse the contract already forbids.** The model is the simple one: *rescan and you get a new list of devices; reset and everything is cleared.* A `Device*` or `DeviceParameter*` is valid until the next `scan()` or `reset()`, and calling either while the game loop is running invalidates every pointer a task holds. That is out of contract, documented, and not defended against — the same posture the codebase already takes toward tearing down a driver mid-cycle.

Restoring that precondition is what deletes the largest commit from the plan (a `std::vector<Device>` → stable-storage refactor across ~31 call sites in `device_manager.cc`), and what makes **`findDevice` and `findParameter` public and non-locking**: with no lock to take, a cyclic task can resolve a device in a cycle, which is the call's entire reason for existing. `withDevice` stays for the control plane and procedures, where holding the lock across a multi-second operation is the point.

### The one narrowing: a device's parameter map is insert-only for its lifetime

`initializeParameters` currently **replaces** `parameters_` wholesale. That is the single hazard that would make a `DeviceParameter*` held by anything else dangle without a `scan()` — and unlike a scan, re-enumeration is not gated by `stopExchange()`. It is also an ordinary HTTP operation (`?readValues=true` on a running bus) that is entirely benign today.

So re-enumeration becomes **insert-only**: it adds keys it does not already have and leaves existing entries untouched, never erasing and never overwriting a definition. `std::unordered_map` insertion does not invalidate references to existing elements (only iterators), so held pointers stay valid. Leaving existing definitions alone is deliberate rather than lazy — RT reads `dataType`/`bitLength` off the parameter while decoding, so rewriting those non-atomic fields in place would be a data race; insert-only means there are no writes to an existing entry's definition at all, and nothing to race. The rule applies **only within one device's lifetime**; the whole map still dies at `scan()`/`reset()`. The wart is that a firmware update which genuinely removes an object leaves a stale entry until the next scan — cosmetic, and the re-map that follows a firmware install rebuilds every back-pointer regardless.

### Who resolves what, and why the RT loop does no lookups

`ProcessImageEntry` gains a `DeviceParameter*`, resolved once in `buildProcessImage` and refreshed on every re-map — the only event that changes the mapped set. The insert-only rule above is exactly what makes that pointer safe.

**The alternative, a hash lookup per mapped object per cycle, does not survive the real bus size.** One device with 40 objects is 1–2 µs, which is nothing; **50 devices with 40 objects each is 2000 lookups, 60–100 µs of pure overhead against a 1 ms grid.** The number that matters is not the one on the desk.

A task author's own read *is* still a per-cycle hash lookup, and that is fine — it is per *signal the task reads*, not per mapped object. Ten signals is ~0.5 µs, and `findParameter` being public lets an author hoist it out of the loop when they care. Different scale entirely; optimising it now would be guessing.

**The decode is eager: every mapped object lands in its cell every cycle, whether anything reads it or not.** At 50 × 40 that is ~2000 extract-and-store operations, roughly 20–40 µs per cycle — affordable against a 1 ms grid where the wire round-trip is already 100–300 µs, noticeable if the period ever drops to 250 µs.

**It is a read-path decision, and that is the only argument that holds.** Eager makes `value<T>()` a hash lookup plus an atomic load; lazy would make every read load the published image, locate the object in it, and extract bits from the newest ring record — far more work, in the loop, where the budget is tight. Paying a bounded cost once in the producer makes every consumer cheap, and the consumers are not only the RT task: HTTP reads, the monitoring flush, and `readParameter`'s PDO branch all become a cell load instead of re-extracting from the ring with allocations.

**Two arguments that look like support and are not.** *"It avoids a branch in `value<T>()`"* — the PDO/SDO distinction is hidden from the **end user**, which is an API promise; internally a branch is just a branch and is not worth 20–40 µs. *"Every value a task reads comes from the same exchange"* — true, but it buys nothing over lazy, because the task runs on the RT thread after the exchange, so no new frame can arrive mid-`execute()` and lazy reads would have seen one consistent frame too.

**The honest shape of the trade is sparse-vs-dense, and eager loses the sparse case:** a task reading 10 signals off a 2000-object bus decodes 1990 values nobody wants, where lazy would have cost a few µs. That is exactly why the escape hatch is worth naming — a "someone has bound this" flag per entry, set when a task or monitoring resolves the parameter, collapses eager onto lazy's cost profile for sparse tasks while keeping the cheap read. Not built; it would be speculation today.

### `outputSlots` folds into the cells

With every parameter carrying a cell, the per-output staging slots are a second home for the same value. The composer reads each output entry's `DeviceParameter*` cell directly instead of a parallel `outputSlots` vector; the single-composer property that makes bit-packed objects sharing a byte safe without a lock (Design B) is untouched, since it was never about *where* the slot lived. A write then reads back as itself — setting `0x607A:00` makes the next read return what was set, not the value from the last frame — which is the behaviour a control program assumes without being told.

### Two doors for the SDO half

Background refresh of non-PDO objects stays where it is: **`ParameterRefresher` remains owned by `MonitoringManager`**, which is its rightful owner — a refresher is monitoring, only slower. A Tier-3 author who wants temperature kept fresh should not have to fabricate a WebSocket topic to get it, so `MonitoringManager` grows a `keepFresh(pos, index, subindex, period)` pass-through to the refresher it already owns. Registration happens off-RT, before or during the loop; the RT task itself can never register (the refresher's mutex and map allocation are both forbidden in a cycle), and that is the only restriction.

Writing a non-PDO-mapped object from a cycle gets an **explicit `writeValueAsync`** — fire-and-forget, stores the cell, marks the entry `Pending`, and a background sweep pushes it over SDO. Explicit rather than folded into `setValue`, because the caller should see that a bus transfer is implied. Offline behaviour needs no special case: the write simply fails until the device is in a state that accepts it, then succeeds. This is the piece expected to be used least, so it lands last and nothing depends on it.

### CiA402 needs nothing

`libs/node/cia402.h` is already pure `constexpr` — `decodeState(statusword)`, the `Command` bits, `isFaulted`, `toOperationMode`. A task reads `0x6041` from a cell and calls `cia402::decodeState` directly; the helpers a controls engineer wants already exist and are RT-callable today. `Cia402Drive` was only ever needed for the *bus-touching* parts, so making the view RT-callable (Session 2026-07-09) is deferred rather than folded in here — and it becomes a much smaller change once the cells exist, since the bus-state dispatch that session designed is what the cells replace.

### Phases — one commit each

1. **Docs** (this session + CLAUDE.md).
2. **Non-allocating primitives** — an `extractBits` overload writing into a caller-provided span, and a ring read filling spans rather than building a `Record` (three mallocs per parameter per cycle today). Purely additive.
3. **`findDevice` / `findParameter` public and non-locking**, with the lifetime contract on both. Insert-only `initializeParameters`.
4. **Value storage becomes the cell** — `bits` + `stamp`, retained pointers for strings/blobs, atomic `syncState`, the hand-written copy constructor the atomics force, and `currentValue()`. No new public API; the existing test suite must pass unchanged, which is the check that this is a storage change and nothing else.
5. **RT accessors** — `value<T>()` / `setValue<T>()` / `text()`.
6. **Producers** — the RT decode after exchange (via the entry's `DeviceParameter*`), the refresher's store, and the control-plane paths, all funnelled through one private store. `readParameter`'s PDO branch stops allocating as a side effect.
7. **`outputSlots` folds into the cells.**
8. **`keepFresh` + `writeValueAsync`** and its background sweep.
9. **The example Tier-3 task** in `libs/example/`, beside the Tier-2 route plug-in — one directory that is the copy-me for both extension tiers.

### Open

Where the `writeValueAsync` sweep runs: the only off-RT thread that touches parameters is the refresher's, inside `MonitoringManager`, so exposing it means a pass-through named for writing on a class named for monitoring — or a second small worker at the composition root. Deferred to phase 8, where it is cheap to decide with the code in front of us. And the per-cycle decode cost lands in the same budget as the wire round-trip, so it will show up on `GET /api/game-loop` — worth measuring on a real bus before the beta rather than trusting the estimate above.

## Session 2026-08-10 — The RT value path, as built: what landed, what changed on the way, and the one piece deferred (as-built)

Eight commits over one session took the design of earlier today from nothing to a working Tier-3 surface. A cyclic task now reads and writes live process data with one lookup and one atomic operation, and cannot tell a PDO-mapped object from an SDO-polled one:

```cpp
void execute(const CycleContext&) override {
  const DeviceManager::CycleGuard cycle(deviceManager_);
  if (!cycle) { return; }                                     // bus not activated / being rebuilt
  Device* drive = deviceManager_.findDevice(3);
  if (drive == nullptr) { return; }                           // not on the bus this cycle
  const auto status = drive->value<uint16_t>(0x6041, 0);      // decoded from the wire this cycle
  const auto temp   = drive->value<int16_t>(0x2030, 5);       // polled in the background — same call
  drive->setValue<int32_t>(0x60FF, 0, *temp > 55 ? 200 : 100);  // sent next cycle
}
```

`libs/example/example_cyclic_task.{h,cc}` is the copy-me starter, registered from `main.cc` behind three commented lines. `libs/example` is now the template for **both** extension tiers — the HTTP route plug-in and the cyclic task — in one directory.

### What the plan got wrong, and what replaced it

**The arena is gone, and with it the largest commit.** The design's first answer to "a rescan dangles every pointer" was to never destroy a `Device` or a parameter map — append-only storage, absent devices retained, identity-matched rescans. Rejected on the day: it buys pointer safety with unbounded memory against a misuse the contract already forbids. The model is the simple one — *rescan and you get a new device list; reset clears everything* — which deleted a `std::vector<Device>` → stable-storage refactor across ~31 call sites and made `findDevice`/`findParameter` public and non-locking with nothing to defend.

**The precondition it left behind was not satisfiable, and that was the session's real finding.** "Do not call `scan()` while the game loop runs" reads like a rule a user could follow. It is not one: `gameLoop.run()` blocks the main thread and every other subsystem starts before it, so **a scan is always concurrent with a running loop** — `POST /api/init` is an HTTP handler. It only looked harmless because the sole registered task held no device pointers. The fix is `DeviceManager::CycleGuard`, and the thing worth keeping is *how small it turned out*: the published image pointer already **is** the bus's "activated" state, and `stopExchange()` already unpublishes and drains. So the lock adds no state — it takes the same seq_cst raise-then-load handshake `exchangeProcessData` uses, one level up so it covers the task's own lookups. `ProcessData::exchanging` became a depth counter (`inCycle`) because the two now nest. This is the two-phase model every EtherCAT stack uses — IgH: configure, then `ecrt_master_activate`, and reconfiguring means deactivating first — and it is what `ProcessDataCyclicTask` already did implicitly by being a no-op until an image is published.

**An insert-only merge was designed, written, and reverted.** It was the answer to `initializeParameters` replacing the map under a lookup. It works, but it adds a rule, a collision error, and a "re-enumerate hardware you swapped without rescanning" case that produces a union of two dictionaries. Once `CycleGuard` existed the simpler answer was available: **re-enumeration pauses the RT cycle across the swap** and republishes — `Device::publishParameters` — which is the same protocol a re-map takes, costs a skipped cycle or two, and leaves no stale entries. The multi-second enumeration stays outside the pause; only the swap is inside it.

**`std::atomic<uint64_t>` lost to a plain `uint64_t` + `std::atomic_ref`, on the opposite argument to the one expected.** The design anticipated hand-written copy/move constructors as the cost of an atomic member. The decisive point is that those five special members are not a one-time cost but a *maintenance* one: `DeviceParameter` gains fields over time, and a field added but forgotten in a hand-written copy constructor loses data silently. Compiler-generated copies never forget. `atomic_ref` puts the lock-free guarantee on the access rather than the storage, which is the guarantee that matters; static asserts pin lock-freedom, the alignment precondition, and little-endianness.

**`stamp` was never built.** The design wanted a monotonic per-cell counter to distinguish "never written" from "the value is zero". `syncState` already records exactly that, and nothing else had a use for freshness, so the field would have been sixteen bytes and a write per cell per cycle for nobody.

### Two decisions about cost that the real bus size settled

**The back-pointer.** `ProcessImageEntry` carries the owning `DeviceParameter*`, resolved once in `buildProcessImage`. A hash lookup per mapped object per cycle is ~1–2 µs for one device and **60–100 µs for fifty devices × forty objects** against a 1 ms grid. The number on the desk is not the number that decides. A task's *own* reads are still per-cycle lookups, and that is fine — it is per signal read, not per mapped object.

**The decode is eager** — every mapped object into its cell every cycle, ~20–40 µs at that same bus size — and the only argument that holds it up is read-path cost: `value<T>()` stays a lookup plus an atomic load, where lazy decoding would put an image lookup and a bit extraction into every read, and every other consumer (HTTP, monitoring, `readParameter`'s PDO branch) gets the value for free. Two arguments that look like support are not: *"it avoids a branch"* — the PDO/SDO distinction is hidden from the **end user**, an API promise, and internally a branch is just a branch; and *"every value comes from the same exchange"* — true, but worth nothing over lazy, because a task runs on the RT thread after the exchange and would have seen one consistent frame either way. The honest shape is sparse-vs-dense, and eager loses the sparse case; the escape hatch, unbuilt, is a flag marking objects someone has actually bound.

### The identity half, deliberately not built

`CycleGuard` stops the crash. It does nothing about **positions shifting** — insert a device between 3 and 4 and every position after it moves, so a task bound to position 4 silently drives what used to be 5. `topologyGeneration()` exists for it and is documented on `findDevice`, but nothing enforces it and no machinery was added. That is the owner's call, and the reasoning is theirs: a Tier-3 program knows it has four wheels at known positions because that is the machine it was written for, and inserting a node is a commissioning act after which you rescan and restart. It also means `TrajectoryRun` needs no generation stamp.

### Deferred: `writeValueAsync`

**Writing a non-PDO-mapped object from inside a cycle is not implemented, and is not expected to be needed soon.** `setValue<T>()` stores the cell; for an output-mapped object that is the whole write, because the composer sends the cell. For anything else the value is stored and nothing transmits it — `writeParameter`, off the RT thread, is the way to reach such an object today.

Both implementations are worked out, so this need not be re-derived:

- **A lock-free queue.** `writeValueAsync` pushes `{devicePosition, index, subindex, bits}` into a bounded ring that the refresher's thread drains via `writeDeviceParameter`. ~40 lines of a primitive nothing else in the tree currently needs, plus drain logic. Precise, and overflow is detectable.
- **A `Pending` sweep.** `writeValueAsync` stores the cell and marks the entry `Pending`; the refresher sweeps for `Pending` entries and pushes them. ~15 lines, but `syncState` must become atomic, and — the real objection — **`Pending` already means something else**: an offline edit through the HTTP API sits `Pending` today and is deliberately never auto-pushed. This would start pushing those too.

**Take the queue when the day comes.** Not for the precision; because the sweep silently repurposes an existing state, which is the kind of coupling that surprises someone a year later. The extra ~25 lines buy a mechanism that does one thing.

### Superseded

This supersedes the phase list of the design session earlier today (phases 8's write-back is deferred as above), and the parts of Session 2026-08-09 describing `outputSlots` as the output path — the staging slots are gone, folded into the cells, and a re-map no longer seeds anything because the cells already hold the values. Session 2026-07-09's "profile view is RT-callable" is still open, and is now a smaller change than it was: the bus-state dispatch it was built around is what the cells replace, and `libs/node/cia402.h` is already pure `constexpr`, so a task decodes a statusword today without any view at all.

## Session 2026-08-10 — Object addresses generated from the ESI: the type belongs to the dictionary, not to the call site (as-built)

The trigger was three mistakes about one object in a single sitting. Writing the Tier-3 example's temperature interlock, `0x2031:01` was got wrong three times running — first the index, then the type (it is a `DINT`, not an `INTEGER16`), then the unit (milli-degrees, so a 60 °C limit is `60000`). Two of those failed **silently**: `value<int16_t>` on a 32-bit object returns nothing at all, and an interlock that never sees a temperature just looks like broken example code. The information needed to avoid all three was sitting in the shipped ESI the whole time.

So it is now carried in the type system. `ObjectAddress<T>` (`libs/node/device_parameter.h`) is an index, a subindex, and the C++ type the object's declared ETG.1020 data type maps to, travelling together instead of being retyped at every call site. It is an **address**, not a reference or a handle, because it holds no device and no pointer — it names a location in *any* dictionary. (ETG.1000.6 and CiA 301 have no collective noun for the pair; they just say index and subindex.) Four `Device` overloads take one and forward to what already existed: `value`/`setValue` lock-free for a cycle, `readValue`/`writeValue` blocking for the control plane. The blocking pair goes through the variant, so it serves strings and byte arrays too — objects a cycle cannot read at all — which is what makes an address worth having for **every** entry of a dictionary rather than only the scalar ones. Ten string addresses and seven byte-array ones in this dictionary would otherwise have no representation.

Then 826 of them, generated from `libs/etg/tests/data/somanet-v5.6.6.xml` by a `motion-master generate-object-addresses` subcommand into three headers — `profile_device_objects.h` (0x1xxx + the standard MDP objects), `cia402_drive_objects.h` (0x6xxx), `somanet_drive_objects.h` (0x2xxx + FSoE) — whose namespaces (`profile::`, `cia402::`, `somanet::`) mirror the view chain rather than the file names.

### The type resolution is `mm::etg`'s job, and it has a trap in it

The generator knows nothing about types on purpose. Resolving one is ESI knowledge, and the ESI has a trap: a vendor writes `ARRAY [0..24] OF BYTE` and the file resolves that to the *code* for `BYTE`. Trust the code alone and a 25-byte object is emitted as `uint8_t`, and every read of it returns its first byte — a silent wrong answer of exactly the kind the whole exercise is against. `mm::etg::resolveValueKind` cross-checks the declared width against the code and answers `std::vector<uint8_t>` when they disagree. Seven entries of this dictionary are that shape: OS command `0x1023:01`/`:03`, the four high-resolution-data buffers `0x20E1:01`–`:04`, and the FSoE unique device ID.

The generator's one piece of judgement is **naming**: `k` + PascalCase of the object, plus the entry name where a subindex names something different, and `k<Object>Count` for subindex 0 of a composite — that last rule is what keeps the six safety objects whose subindex 1 repeats the object's own name from colliding with their own entry-count field. It takes 826 rows to 826 distinct identifiers with no collisions; a duplicate would be a compile error in the output, so one is disambiguated deterministically *and* reported.

It lives in the `motion-master` binary rather than a second tool because it needs the ESI parser the server already links, and a separate binary to carry one function is another thing to build, package and keep in step.

### One header per index range, not per device — and the assumption that buys

Recorded a day later, in `docs`-only form, because it was folklore in three heads and nowhere in the tree. The generator merges every device in the file into **one** table keyed by `(index, subindex)`, which is sound only if an address names the same quantity, with the same data type and the same unit, on every device in the family. For 0x1xxx and 0x6xxx the standards guarantee that. For the manufacturer-specific area **nothing does** — ETG.1000.6 reserves 0x2xxx for the vendor and says nothing about stability across devices — so the merge rests on SOMANET's own convention.

That convention holds for a stronger reason than four descriptions agreeing: Node, Circulo, Circulo SMM and Integro all reference the **same** ESI module (`0x04020001`, "Default CiA402 object dictionary") for the bulk of their dictionary, so most of the union is one text merged with itself. Measured against the pinned file: of the ~40 objects each device declares at the device level, 40 indices appear on more than one device and **none** disagree on name or type, and the generator's type-disagreement warning never fires. The only merge collisions are the SMM's four mutually exclusive FSoE safety modules, already covered by the last-wins policy.

The gap is named in the same comments: one index reused for a different quantity of the **same type** merges silently. Nothing detects it, and it would mean a header per device instead of one — a second vendor's ESI being the likely occasion.

### Two mechanical decisions

**Regenerating is two steps** — the subcommand, then `tools/format.sh` — because clang-format wraps the declarations that overrun 100 columns and does it context-sensitively enough that reproducing its choices in the generator would be guesswork. The banner in each generated file says so, and the round trip is exact: regenerating the pinned ESI reproduces the committed bodies byte for byte, which is how the family-convention banner could be added as a 12-line diff per header with no body churn.

**There is no CI drift check, deliberately.** The ESI is pinned and regenerated on request, so a check would fail exactly when the lag is intentional — the one state it would report is the one that is fine.

### Not generated, and not planned

Only addresses. No accessors, no enums for the ESI's enumerated values, and **units stay in the trailing comment rather than becoming types** — a milli-degree type that made `60000` unwritable would be worth having and is a different project. The example task naming `somanet::objects::kDriveTemperatureMeasuredTemperature` instead of a raw index and a hand-written `int32_t` is the whole argument in one line; `libs/node/tests/object_addresses_test.cc` pins the three motivating cases, and most of its job is that 826 declarations compile at all.

## Session 2026-08-11 — Can it drive a machine in production? The RT path is no longer the answer; free-run DC and a missing motion layer are (review)

Asked directly, of the tree as it stands at `6.0.0-alpha.69` on an RT Linux host. Worth recording because the honest answer moved this week and the reason it moved is narrow: what the cells closed is **value access**. Before them there was no way to read a position or write a target inside a cycle without touching something that locks or allocates, so the question did not arise. Now `value<T>`/`setValue<T>`, `CycleGuard` and `keepFresh` make an in-process control task ordinary C++ — and every remaining obstacle is somewhere else. None of them are in the RT value path.

### The bus is not synchronised, and that decides most of it

`soem_fieldbus_driver.cc` calls `ecx_configdc` and deliberately does not call `ecx_dcsync0`: DC is initialised, propagation delays are measured, a reference clock is elected, and no SYNC0 pulse is ever generated. Drives therefore act on **frame arrival**, which makes the loop's wake jitter the actuation jitter. On a tuned PREEMPT_RT host that is small and, for one axis, unimportant. It is still not hardware synchronisation, so anything requiring two axes to act on the *same instant* — coordinated multi-axis, a gantry, an interpolated path — is out until SYNC0 lands together with the DC-locked cycle timer it needs to not be a regression.

This is the item that decides the verdict, and it is the one already scheduled.

### There is no motion layer, and the Tier-3 door is not a substitute for one

`TrajectoryCyclicTask` appears nowhere in `libs/` or `apps/`; `main.cc` registers exactly one task, `ProcessDataCyclicTask`, with the example commented out. `Cia402Drive::enable()` sleep-polls, so the view stays off-RT and a cyclic task must step the 402 state machine itself from the `constexpr` helpers, one transition per cycle.

So "it can drive a machine" today means *you write the setpoint generation, the enable sequencing and the fault handling*, and they are as good as you make them. For a machine builder extending the source that is a legitimate answer — it is what Tier 3 is for. It is not a product feature to lean on, and the difference matters: the shared, tested motion path that a trajectory task would make possible is exactly what a per-site reimplementation is not.

### The rest of the debt is validation, not code

The RT value path is days old and has not been soaked on hardware. There is no hardware-in-the-loop CI; `hil/api` and the unit tests cover the contract, not a week of continuous motion. Several shipped features still carry an outstanding hardware re-test of their own (the two-device firmware install, the partial-bus re-map watchdog).

Three deliberate accepted-risk behaviours belong in the same paragraph, because each is correct by design and each is something an unattended machine would eventually meet: `pauseCycle` gives up draining after 200 ms and **proceeds anyway** (logged, never silent — the alternative was hanging a control-plane call on a stalled RT loop), so a descheduled RT thread during a re-map is a real corruption window; a `withDevice` borrow does not exclude an AL transition; and a bus position is not a device identity across a rescan.

### Authentication is a non-goal

Raised as an exposure — the HTTP surface has no authorization of any kind, so anything that can reach the port has drive control, firmware installation and AL-state changes — and **decided against**: these are local programs, not network services. The default bind is loopback and that is the deployment being designed for.

Recorded rather than dropped for one reason: the off-loopback path exists in the tree (`server.bindAddress`, the two-SAN certificate, the Pi appliance, `docs/LAN_DEPLOYMENT.md`), and it is the condition that would reopen the question — a plant LAN, not a laptop. Until something ships that way as a matter of course, there is nothing to build.

### Safety is not on this axis at all

E-stop and STO are hardwired or FSoE, and nothing here is a safety controller — the FSoE support is dictionary access to a safety module's parameters. That is unchanged by SYNC0, by a trajectory task, and by any amount of soak testing, so it does not belong in the readiness argument in either direction.

### Where it stands

As a **commissioning and diagnostics tool** — parameters, monitoring and the lossless recorder, firmware, ESC diagnostics, PDO mapping — production-ready, which is what it was built to be first. For **motion**, pilot grade: a single axis or loosely-coupled axes, your own control task, on a tuned PREEMPT_RT host with `skippedCycles()` observed flat over hours, safety wired in hardware. Four things would change the verdict, in this order: SYNC0 plus the DC-locked timer; a trajectory task so the motion path is shared and tested rather than per-site; a real soak on the target hardware; and the alpha line settling, since the changelog's own promise is that the API may break between any two alphas.

## Session 2026-08-19 — Device lifetime is a refcount, not a lock: `DeviceSet` replaces the borrow, and the second procedure body shape disappears with it (as-built)

Asked as a challenge to the locking design: nine mutexes to be correct looked excessive, and `BusProcedureBody` plus the whole `withDevice`/`withDevices` apparatus looked like complexity for its own sake. Both readings were right, and they had one cause: **`devices_` was a `std::vector<Device>` that `scan()` destroyed, so lifetime was solved with a lock.** Everything that felt heavy followed from that line — the borrow (34 call sites), the "must not re-enter a control-plane operation" trap, the two-lock split in `DeviceManager`, and the second procedure body shape.

### What replaced it

`DeviceSet` — driver, devices, topology generation — built by `init`/`scan`, published once, never modified. Off the RT thread every caller holds a `shared_ptr` to one: `deviceAt(pos)` returns a `DeviceHandle` (device plus set), `deviceSet()` returns the set. The RT thread reads a raw `publishedSet_` pointer instead, swapped only with the cycle drained, because `std::atomic<std::shared_ptr<T>>` is not lock-free and libc++ does not implement it at all. **This is the pattern the process image already used** (`image` plus retained `generations`), applied to the device set — which is why it needed no new mechanism, only a different owner for the lifetime.

`deviceSetMutex_` is gone. Two leaves took its two unrelated jobs: `currentSetMutex_` (one `shared_ptr` copy) and `processDataMutex_` (the recorder storage and the retained generations, against `allocate`/`clear`). The count went from nine mutexes to ten, and that is the honest number — but the order chain went from four deep to three, no lock is held across a callable any more, and the only lock held across bus I/O is the driver's own.

### The behaviour that changed, and it is a real change

**A rescan no longer waits for whoever holds a device, and no longer invalidates them.** `scan` publishes a new set; the old one dies with its last holder. A procedure interrupted by a rescan keeps working against its retired device and fails on its next transfer, because the retired set still owns the driver object while `reset` has stopped the bus. The old design guaranteed validity by making the two mutually exclusive — a rescan blocked for the length of a procedure, or the procedure's `Device&` dangled. Both halves are now true at once, which is the point.

The cost is transient memory: a retired set — devices, parameter maps, driver — lives as long as its last holder. Bounded by the holder, not retained to `reset()` like image generations.

### Why the second body shape existed, and why it does not now

`ProcedureBody` ran inside `withDevice` holding `deviceSetMutex_` shared; `transitionToState` needs it exclusively; `std::shared_mutex` has no upgrade. So a body that installed firmware — a procedure *defined* by its AL transitions — could not be written in the ordinary shape, and `BusProcedureBody` was added for the one row of 23 that needed it. With no lock behind a held device, that constraint evaporated. One shape now: `(const ProcedureContext&, ProgressReporter&, std::stop_token)`, where the context carries the manager, the device, and the position. Any body may transition.

### Two smaller findings worth keeping

**`CycleLock` was renamed `CycleGuard`.** It never blocks, never waits, and can fail to enter the cycle, so "lock" claimed mutual exclusion it does not provide — and it contradicted the rule it exists to serve ("the RT thread acquires no lock"). Three documents carried a disclaimer explaining the name away; the rename deleted the disclaimer instead.

**`device(pos)` became `deviceAt(pos)`** because a member function named `device` shadows the local variable name `device` in nineteen of `DeviceManager`'s own methods, which cppcheck reports and which reads badly. The accessor was the cheaper thing to rename.

### What was not done

`withDevice` was not kept as sugar over the handle. A one-line lookup that no longer holds anything is not worth a name, and keeping it would have kept the borrow vocabulary alive in the docs. `ProcessDataRing` was not converted to `shared_ptr` storage either: its lifetime problem is real but orthogonal, and one leaf `shared_mutex` states it more plainly than a second refcount would.
