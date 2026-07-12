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

> **Superseded by Session 2026-06-06 — HTTP and WebSocket on separate ports/loops.** The WebSocket now runs on its own port (`wss://…:62281`, `--ws-port`) and event loop so a blocking HTTP handler can't stall it; the HTTP API is on `61447`. (Defaults moved off 8443/8444 — see that session.)

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

> **Superseded by Session 2026-06-05 — Device profiles as borrowed views.** The conclusion below (CiA402 as a subtype-or-component *of* `Device`; Somanet relegated to free functions because subclassing would force downcasting) was reasoned about the wrong ownership direction. The resolved model inverts it: profiles *borrow* a `Device&` rather than being owned *by* `Device`, which makes a full `SomanetDrive → Cia402Drive → ProfileDevice` inheritance chain correct. Read the later session; the notes here are kept only for the reasoning trail.

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

`App` is the only place that instantiates concrete types. `FieldbusDriver` is injected into `DeviceManager` and `NetworkScanner`; `DeviceManager` passes a `FieldbusDriver&` into each `Device` it creates. `GameLoop` and `HttpServer` both receive a `DeviceManager&` — `GameLoop` calls `exchangeProcessData`, `HttpServer` calls SDO/file/state methods. Inject `NotificationBus` into `Watchdog`, `DeviceManager`, `WebSocketServer`.

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

The GameLoop starts unconditionally regardless of whether a driver is present. `exchangeProcessData()` is a no-op when `driver_` is null, so the loop runs safely in the uninitialised state.

**Thread safety — open issue**

`POST /api/init`, `POST /api/scan`, and `POST /api/reset` run on the HTTP server thread and mutate `driver_` and `devices_`. `exchangeProcessData()` runs on the RT GameLoop thread and reads both. There is currently no lock guarding this boundary. This is safe only because `exchangeProcessData()` is not yet wired into the GameLoop. Before enabling live PDO exchange, the loop must be stopped (or drained for one cycle) before `init()` or `reset()` is called via the API.

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

*(The `~/.acmedns.json` step described here was a no-op — acme.sh reads `ACMEDNS_*` env vars, not that file. See Session 2026-06-06 — TLS cert auto-update for the fix and the rolling-release/self-heal additions.)*

*(The `gh secret set` / `TLS_CERT` / `TLS_KEY` / `GH_PAT_SECRETS` mechanism below was retired — see Session 2026-07-08. `cert-renewal.yml` now only publishes to the rolling `tls-cert` release, which is the single source of truth.)*

Runs on the 1st of every month via `schedule`. Installs `acme.sh`, writes `~/.acmedns.json` from the `ACMEDNS_CONFIG` secret, issues a fresh cert with `--issue --force --dns dns_acmedns --server letsencrypt`, then updates two repository secrets via `gh secret set` using a PAT (`GH_PAT_SECRETS`) with Secrets read/write permission:

- `TLS_CERT` — full-chain PEM (renewed cert + Let's Encrypt intermediate)
- `TLS_KEY` — EC private key

**release.yml**

Triggered by `v*` tag pushes. Builds with the `x64-linux-release` CMake preset, reads `TLS_CERT` and `TLS_KEY` from secrets, writes them as `cert.pem`/`key.pem` into the build output directory, then packages `motion-master`, `cert.pem`, and `key.pem` into `motion-master-<version>-linux-x64.tar.gz` and publishes a GitHub Release. *(Superseded — since Session 2026-07-08 each leg `curl`s the cert/key from the rolling `tls-cert` release instead of reading secrets; the release is now multi-platform, see that session and the README CI table.)*

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

## Session 2026-05-30 — Module ident reconcile (CoE Modular Device Profile)

**Problem.** A modular EtherCAT slave exposes two CoE Modular Device Profile lists (ETG.5001): the **Detected Module Ident List** (`0xF050`, what is physically plugged into each slot) and the **Configured Module Ident List** (`0xF030`, what the master expects). When the two disagree the slave reports a module mismatch and refuses to leave PRE-OP. With no ENI and no pre-engineered expected configuration, Motion Master's job is "talk to whatever drive is on the bus", so the configured list is meaningless to us — we clear the mismatch by copying detected into configured.

**`FieldbusDriver::writeSdo`.** SDO download was missing from the driver interface (only `readSdo` existed). Added:

```cpp
virtual std::expected<void, std::string> writeSdo(
    uint16_t slavePosition, uint16_t index, uint8_t subindex,
    std::span<const uint8_t> data) = 0;
```

`std::span<const uint8_t>` for the payload (read-only view, zero-copy, binds to vector/array/slice — matching `writeFile`/`writeRegister`), versus `readSdo` returning an owned `std::vector<uint8_t>`. `SoemFieldbusDriver::writeSdo` wraps `ecx_SDOwrite` under `socketMutex_` with the same SDO/mailbox/packet error decoding as `readSdo`. `Device::download(index, subindex, data)` is the per-device wrapper, mirroring `upload`.

**`reconcileDetectedModules(const Device&)`** — a free function in `mm::node` (declared in `device.h`):

1. Read `0xF050:00` (slot count). A slave with no detected-module list is simply not modular → return `0`, not an error.
2. For each slot `1..N`: read the detected ident `0xF050:sub`; skip empty (all-zero) and malformed (non-4-byte) entries; skip slots whose `0xF030:sub` already matches; otherwise write the detected ident into `0xF030:sub`.
3. Return the number of slots written, or an error string naming the slot(s) whose write failed.

Idempotent (the already-matches skip means re-running is a no-op) and **vendor-neutral**. This is the one notable departure from the "Somanet specifics live in `namespace somanet`" rule: the function is keyed purely on the standard MDP objects and never branches on vendor ID, so it belongs in the generic `mm::node` layer, not in `somanet`. It generalises the older single-slot, Synapticon-labelled approach to any modular EtherCAT device while still covering the Somanet case.

**Where it runs.** Wired into `DeviceManager::transitionToState`: when the target is PRE-OP, after the transition settles, the reconcile runs for every device that actually reached PRE-OP (`!error && alState == PreOp`). PRE-OP is the earliest point the write is possible — `0xF030`/`0xF050` are SDO mailbox objects, unavailable in INIT, and the mismatch must be cleared before SAFE-OP. Decided to make it **always-on** (no config flag, no separate HTTP route) and **best-effort**: failures are logged at warn level but never fail the transition.

**Open question — persistence.** The `0xF030` write is volatile on some firmwares (they require a `0x1010` store, or re-evaluate the list every boot). The current design re-runs on every PRE-OP transition, so it is self-healing regardless. If a mismatch is observed to reappear after a power cycle on real hardware, add an explicit store; until then the per-transition reconcile is sufficient and avoids unnecessary EEPROM wear.

## Session 2026-06-01 — DC sync diagnostics, and the remaining fieldbus surface

**DC sync health page.** Added a distributed-clock synchronisation diagnostic alongside the existing bus-health (ESC error-counter) one. `FieldbusDriver::readDcSync(positions)` (default "unsupported" for ESC-less transports; SOEM override) reads each DC-capable slave's **system-time delay (0x0928)** and **system-time difference (0x092C)** in one 8-byte FPRD. The reference clock is the first `hasdc` slave (SOEM elects it in `ecx_configdc`); its own difference is zero. 0x092C decodes as bits 0–30 magnitude + bit 31 sign → a signed-nanosecond deviation (positive = local clock ahead of the reference). Surfaced end-to-end: `mm::comm::DcSyncDiagnostics` → `DeviceManager::getDcSync` + `DcSyncInfo`(+`to_json`) → `GET /api/dc-sync?positions=` → a polling **"DC Sync"** page under the **Fieldbus** sidebar group.

The figures are meaningful only while exchanging in SAFE-OP/OP: this stack runs DC in **free-run** (`ecx_configdc` measures and elects a reference, but `ecx_dcsync0` is deliberately *not* called — no SYNC0 pulse), yet `ecx_send_processdata` still distributes the reference system time via the cyclic FRMW, so the slaves' drift-compensation loops run and 0x092C converges toward zero. A value that stays large or grows means a slave is not locked.

**Roadmap — fieldbus capabilities not yet exposed.** The exposed surface is now bus-level Control / Configuration / Process Image / Diagnostics / DC Sync, plus per-device FoE / Parameters (CoE OD + SDO) / PDO Mapping (read + write, shipped 2026-07-06 — see that session) / Registers (ESC) / SII (EEPROM read). What remains, ranked by value-vs-effort, deferred for a later session:

*Tier 1 — high value, mostly presentation of data the driver already caches (read-only, no RT):*
1. **Topology / cabling map.** SOEM already caches per-slave `topology`, `activeports`, `consumedports`, `parent`, `parentport`, `entryport`, and the `DCnext`/`DCprevious` chain (`extern/.../soem/ec_main.h`). Combined with the per-port link state already read in Diagnostics (DL Status 0x0110), this renders the physical bus tree — line/ring/branch, hot-connect groups, which port connects to which neighbour. The view a field engineer reaches for first; spots a miscabled port instantly. Shape: a `busTopology()` driver method (cached read) + a tree/graph UI.
2. **Frame / WKC health timeline (master-side).** Process Image shows `lastWkc`/`expectedWkc` as a point value; the GameLoop gets a WKC every cycle. Accumulate master-side stats over time — WKC-mismatch count, lost frames, longest cycle overrun, "drops in the last minute" — to catch *intermittent* faults a point-in-time reading walks past. Distinct from the slave-side ESC counters. Pairs with the delta-tracking follow-up already noted for the Diagnostics page.

*Tier 2 — genuinely new information, moderate effort, read-mostly:*
3. **Diagnosis History — CoE 0x10F3 (ETG.1020).** The standardised per-slave event log: a ring buffer of timestamped diagnostic messages the slave itself recorded (error/warning/info + parameters) — the slave's own words, categorically different from the master-side counters. Built entirely on the existing SDO read; the work is decoding the message format. Confirm SOMANET firmware populates 0x10F3 before committing to it.
4. **Explicit device identification ("locate"/blink).** Command a slave to flash its ID LED so a tech can physically find it in a rack. Small, installer-friendly.

*Tier 3 — real capability, but write/RT/risk; deliberate actions, not toggles:*
5. **DC SYNC0 activation** (`ecx_dcsync0`) — turn on true DC-synchronous operation with configurable cycle/shift. The natural *control* counterpart to the DC Sync diagnostic above; what you'd do for tight coordinated multi-axis motion. RT implications.
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

**Category 1 — RT cyclic procedures (SineWave / profile / ramp generators).** Must write a fresh target into the output region *every* cycle, phase-locked to the bus. These are `CyclicTask`s — but with **fixed membership**: registered once before `run()`, exactly like `ProcessDataTask`. They are **activated/deactivated at runtime via a control block** (an atomic active-flag + seqlock'd parameters: target position/axis, frequency, amplitude, waveform), written from the HTTP thread. This mirrors the pattern already in production — `ProcessDataTask` is registered unconditionally and is a *no-op until a process image is published*. An idle generator is the same: registered always, computes nothing until HTTP flips it active. So runtime variability is in task *behaviour*, never task *membership* — which deletes the add/retire/destruction problem wholesale.

- **Concurrency = a fixed pool, not runtime spawning.** Several devices running waveforms at once is served by registering a small fixed pool of generator slots at startup, each idle until claimed by a device. Membership stays static.
- **Output writes are direct, not via the staging seqlock.** A generator runs *on* the RT thread, so it writes the output PDO slots directly each cycle. The output-staging seqlock exists only to serialise *non-RT* (HTTP) setpoint writers; an on-RT generator does not use it. (Note this is the same producer the Design B output path must stay lock-free for — see that session's `writePdo`/`stagingMutex` wart.)
- **Target ownership / arbitration.** While a generator owns a device's target word, a concurrent HTTP setpoint to that same word would fight it. Activating a generator must claim authority over that target; HTTP target writes for that device are rejected (or redirected) until it is deactivated.
- **Ordering.** Generators run *after* `ProcessDataTask` in registration order: read this cycle's inputs, compute, stage the output for the next exchange.

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
- The single fixed-membership `SineWaveTask` (owned by App, holds `DeviceManager&`) iterates devices each cycle, `load()`s each control block, and for active ones computes `center + amplitude·sin(phase)` and writes the output slot directly (on-RT). It runs after `ProcessDataTask`. Bad params never reach it — all validation is on the launcher's `expected<>` path, so the RT side has no error branch.

**RT-only scratch state stays off the seqlock.** `phase_`, `center_` (latched on the rising edge of `active` so the wave starts from the current position), and `wasActive_` (edge detection) are written *and* read only by the RT task — they must not go in the control block (HTTP→RT only; readers must not see RT scribbling phase back). Phase is *accumulated* (`phase_ += 2π·f·dt`), not `2π·f·t`, so a mid-run frequency change stays continuous. Open question for implementation: this scratch lives either in a per-axis map inside `SineWaveTask` or as RT-only members on `Device`, and either way the task's per-cycle `findDevice()` re-resolution must sit behind the same `scan()`/`reset()` drain (`stopExchange()` + published-image pointer) that `ProcessDataTask` relies on today — iterating `devices_` while a rescan reallocates it is a data race that only the process-image path is currently guarded against.

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
- **`DeviceManager` owns `unique_ptr<FieldbusDriver>` + `std::vector<Device>` + `unique_ptr<ProcessData>`.** It hands each `Device` a `FieldbusDriver&` and a raw `ProcessData*` (non-owning; the manager outlives every device). The only concrete driver in the tree is `SoemFieldbusDriver`; `SpoeDriver`/IgH remain planned.
- **Profiles are borrowed views, confirmed in code:** `ProfileDevice ← Cia402Drive ← SomanetDrive`, each holding only a `Device&`, built via `createCia402Drive` / `createSomanetDrive`. There is **no** `Cia402StateMachine` owned by `Device` — that 2026-05-16 ownership model was already inverted by the 2026-06-05 borrowed-views session; the older diagram just hadn't caught up.
- **One RT task is registered:** `ProcessDataTask` → `DeviceManager::exchangeProcessData()`. `GameLoop` keeps fixed `CyclicTask` membership.
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

**Not a drain queue — a continuous circular recorder, and the source for the live stream too.** The decided shape (not an SPSC producer→consumer where the consumer advances a tail and frees slots) is a flight-data-recorder ring in master RAM. Crucially, **monitoring must deliver _every_ cycle for plotting** — the live WebSocket path has to be lossless, not just the bulk pull — so the ring is the source for *both*:
- **RT loop writes one record every cycle, forever**, advancing a head and overwriting the oldest only on wrap. "Very big" ⇒ a long rolling history is always resident.
- **One ring, two non-destructive readers, both reading every cycle.** (a) The **live streamer** (`MonitoringManager`'s thread) holds a per-monitoring **read cursor** — a read index, *not* a tail. Each flush it reads `ring[cursor..head]`, decodes that monitoring's parameters for **every record in the span**, packs them as rows, publishes the batch, advances *its own* cursor, frees nothing. `interval` stops meaning "sample one value per interval" and becomes the **flush cadence** (flush every 50 ms ⇒ ~50 cycle-rows/batch at 1 ms); the client plot gets a contiguous, gap-free series. (b) The **dump** is a second non-destructive reader — **deferred, see the parked section at the end of this note**. The existing WebSocket protocol already carries "one inner array per sample" — a batch now holds every cycle-row since the last flush instead of interval-samples, **no protocol change**.
- **The `SeqLock<ProcessBuffer>` snapshots are dropped entirely (settled 2026-06-09).** Earlier this note kept them "for point reads"; that's redundant — **`ring[head-1]` _is_ the latest coherent snapshot.** A point read (`readPdo`, current-value lookups) loads `head`, targets `seq = head-1`, reads `ring[seq % capacity]`, and re-checks the slot's sequence (the same per-record seqlock guard the live cursor/dump use) — identical coherence to the old seqlock. Wins: **the RT loop writes only `ring[head]` once per cycle** (not ring + input snapshot + output snapshot); one source of truth; and a single record holds inputs *and* outputs from the *same* cycle, so a combined read can't straddle cycles the way two separately-written seqlocks could. Health gating is unchanged (WKC atomic + published-image pointer, independent of the seqlock; `head == 0` ⇒ no cycle yet ⇒ same SDO fallback). Output *staging* is unaffected (`outputSlots` atomics, Design B); the output read-back just comes from `head-1` too. `ProcessData` stops holding `SeqLock<ProcessBuffer>` members — the generic `SeqLock` in `libs/core` stays (a ring slot is morally one). The ring is the single RT-written structure and the source for the full history, the live plot, *and* point reads.

**Two decisions taken (2026-06-08):**
- **Sizing: configurable *seconds*, in the JSONC config — not a CLI flag** (settled 2026-06-08). Ring depth is a persistent, per-machine RAM budget set once per install, which is what the config file is for — and the config (added but unused so far) gets its first real consumer, setting the pattern. A `recorder` block, **`historySeconds` only for now**: `{ "historySeconds": 300 }`. (`dumpDir` is **not** added yet — it belongs to the deferred dump, and a `/tmp` default is wrong cross-platform: Windows has no `/tmp`. When the dump returns, derive the default from `std::filesystem::temp_directory_path()`, not a hardcoded path.) `capacity = historySeconds × 1000 cycles/s`; the byte allocation can't be computed until the process image exists (record size = whole-image size, bus-dependent), so the **ring is allocated and `mlock`'d at `configureProcessData` time, not at startup** — its lifecycle is tied to the process image. At ~400 B/cycle: 1 min ≈ 24 MB, 10 min ≈ 240 MB, 1 hr ≈ 1.4 GB. Default leaning ~300 s ≈ 120 MB.
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
- **Synchronous write** off-RT on the HTTP thread (300 s ring ≈ ~120 MB, sub-second). Background job + progress only if rings ever get huge (1 h / 1.4 GB).
- **The hard open problem to solve on return — ring lifecycle vs. teardown & re-map:** the ring + its process-image header **must survive image teardown**, because tearing down the published image happens when the *last* device leaves OP/SAFE-OP — precisely the dump moment. So the ring is owned durably by `ProcessData` (which `DeviceManager` owns), retained across image republish *and* teardown; only `reset()`/`scan()` frees it. And because a **re-map changes the byte layout**, old records become undecodable under a new header — so a layout-changing re-map must **reset the recording** (clear the valid span, capture the new header); one span can't straddle two layouts. Lifecycle: *allocated & header captured at map → fills every cycle → frozen-but-retained when the device drops → reset & re-headered on the next re-map.*

**IMPLEMENTED (2026-06-09) — the live recorder.** `ProcessDataRing` (`libs/node/process_data_ring.{h,cc}`) is a lock-free circular recorder held as a member of `ProcessData`. The RT loop appends one record per cycle in `exchangeProcessData` (raw input + output IOmap, wall-clock epoch-ns timestamp, working counter) via a wait-free `write()` (a few `memcpy` + a per-slot release-stored absolute sequence number; readers re-check the sequence after copying to detect a write that raced the copy). It is allocated and best-effort `mlock`'d at `configureProcessData` for `recorder.historySeconds × (1e6 / gameLoop.periodUs)` cycles (default 300 s ≈ 120 MB), re-allocated on a layout-changing re-map (the recording restarts — old records are undecodable under a new layout), retained across image teardown, and freed only by `reset()`/`scan()`. The pair of whole-image snapshots that used to carry inputs/outputs across the RT boundary is gone — `readPdo` and point reads now read the newest record (`head()-1`); `DeviceManager` exposes `recorderHead()`/`recorderOldestSeq()`/`readRecord()` as the off-RT read surface. `MonitoringManager` is rewired to the cursor model: each monitoring holds a read cursor and, on each flush, ships every recorded cycle in `[cursor, head)` as one batch (lossless), advancing the cursor; `bufferSize` was dropped and `interval` is now the flush cadence; a cursor lapped by more than a whole ring is logged (not notified) and resynced. Wire row timestamps are **epoch microseconds** (JS-exact to ~2255, distinct per cycle at sub-ms periods). Config gained a `recorder` block (`historySeconds`, default 300); `DeviceManager::init` takes the history depth and cycle period. The follow-ups are also done: `swagger.yml`, the regenerated `hil/api/src/mm-api.ts`, and the UI (`MonitoringsPage.tsx`) all reflect the new contract; `CLAUDE.md`/`README.md` updated.

*Portability gotcha (fixed 2026-06-09, commit 243d849):* the ring's seq array is a `std::vector<std::atomic<uint64_t>>`, and `std::atomic` is non-movable/non-copyable, so it must never **reallocate** — `clear()` releases it by swapping in an empty vector, not `shrink_to_fit()`. `shrink_to_fit`/per-element reallocation compiles on Linux libstdc++ but is **rejected by libc++ (macOS) and MSVC (Windows)**, so it passed local Linux builds and only broke the macOS/Windows CI legs. Whole-vector move-assignment (sizing it once) is fine; per-element reallocation is not.

**Interval bounds widened (2026-06-09):** the flush cadence is bounded **5–2000 ms** (was 10–1000 ms in the original design above), default **16 ms** (≈ one batch per 60 Hz display frame — the rate the plot actually repaints). Rationale: interval governs message size, not resolution (lossless either way); 5 ms allows lower-latency plots without a message storm, 2000 ms keeps a heavy 40-param @ 1 ms monitoring under ~760 KB / ~11 ms to serialize+parse. Note the bound is cycle-period-dependent — at a sub-ms cycle every message scales up proportionally — so a byte-budget cap (or the deferred last-N mode) is the more robust long-term guard for the slow/fast extremes.

**Open / next:** the deferred dump **shipped 2026-06-10** (see that session); the two-delivery-mode *last-N telemetry* path is still a fast-follow (only the lossless mode shipped), as is the offline `.mmpd` viewer UI.

## Session 2026-06-08 — Config file: optional, code-owned defaults (the `recorder` block is its first consumer)

The JSONC config (`nlohmann::json::parse(stream, nullptr, true, true)`, `.jsonc`) has existed but had no consumer; the recorder's `historySeconds` is the first. Settled the config posture generally, since this sets the pattern:

- **Optional, with defaults in code.** A missing config file is **not an error** — it means "all defaults." Motion Master runs zero-config (terminal-only users, Docker, first-run all just work, like the server already binds default ports without a file). Code is the single source of truth for defaults.
- **Ship a fully-commented `motion-master.example.jsonc` in the package — *not loaded*, pure documentation.** Every key present at its default with a `//` comment. This is the one real benefit of "distribute a config with every instance" — *discoverability* of what's tunable — without making the file load-bearing or letting defaults drift into being required. JSONC comments are the reason this works. Because it is not loaded it needn't be a conffile (fine to overwrite on upgrade); only an *active* default config installed to `/etc` would need `%config(noreplace)` like `cert.pem` — deliberately avoided for now (active config stays purely user-created).
- **Partial override, not all-or-nothing.** A real config need only contain the keys it changes — set `recorder.historySeconds` alone and everything else stays default. The loader **merges the parsed file over a code-defaults object** (nlohmann: start from defaults, `merge_patch` the file on top), never requires a complete document.
- **Explicit `--config` only — no default search path.** Config is loaded *only* when `--config <path>` is passed; there is no implicit `/etc/...` or next-to-binary lookup. Absent `--config`, all defaults apply. (Keeps the active config purely explicit — nothing is silently loaded from a location the user didn't name.)
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

- **Threading correction — the one substantive point, not just transcription.** The first cut of the class-diagram prose claimed plug-in handlers "add no thread and cross no RT boundary." That overreached: it's true of the *example* plugin and of the registration/handler model (registration runs once on the HTTP loop thread; handlers run on that thread per request), but a plug-in is **ordinary C++ holding `DeviceManager&`** and may spawn its own off-RT `std::jthread` for long-running work — exactly as `MonitoringManager`'s sampler/refresher do. The honest framing: the built-in process runs **five** threads, but five is the built-in count, **not a hard ceiling**. Any plug-in-spawned thread is bound by the same rules as every non-RT thread — serialize bus access through `FieldbusDriver::socketMutex_`, never touch the RT path. Fixed in all three docs (`CLASS_DIAGRAM.md`, `THREADS.md`, and the `CLAUDE.md` mandate bullet, which now says the *registration fn* runs once on the loop thread rather than implying all plug-in work stays there).

No code change; `docs/` and `CLAUDE.md` only.

## Session 2026-07-01 — `CyclicTask` lifetime: non-owning is correct; the invariant is "outlive `run()`", not "outlive the loop"

A design-review question on `GameLoop`: it holds `std::vector<CyclicTask*>` (non-owning) and documented that a registered task "must outlive the loop." Is non-owning the right call, and is that the right contract? Two separate answers.

**Non-owning is correct, and for the same reason ownership lives at the composition root everywhere else.** `GameLoop` is a pure RT scheduler — it ticks a `CyclicTimer` and calls `execute()` on a fixed set of tasks. The tasks themselves are thin adapters whose *real* lifetime is coupled to the domain objects they wrap: `ProcessDataTask` holds a `DeviceManager&`, and `DeviceManager` is owned at the composition root (`main.cc`). Making `GameLoop` own the tasks via `unique_ptr` would put the RT loop in charge of a lifetime that actually belongs to the DI graph. It would also fight the fixed-membership rule (Session 2026-06-05 — *RT tasks are fixed-membership*): membership never changes at runtime, so there is no lifecycle churn that would argue for the loop taking ownership. `GameLoop` stays a mechanism, not an owner — consistent with "`main.cc` is the only place concrete types are instantiated."

**But the documented contract was one notch too strong, and `main.cc` actually violated its literal form.** The pointers in `tasks_` are only ever dereferenced *while the loop is executing* — never after `run()` returns. So the real invariant is "a task must outlive every call to `run()`," not "outlive the `GameLoop` object." The distinction mattered because the original `main.cc` declared `gameLoop` **before** `processDataTask`:

```cpp
GameLoop gameLoop{...};                // constructed first
ProcessDataTask processDataTask{...};  // constructed later
gameLoop.addTask(&processDataTask);
```

Reverse-order stack destruction destroys `processDataTask` first — so the task did **not** outlive the loop as the doc claimed. Harmless today only because `run()` has already returned by the time either destructor fires, but a latent footgun if anyone reordered these or touched tasks post-`run()`.

**Fix: both, not either.** Swapped the declaration order in `main.cc` (task before loop, so the task is destroyed *after* the loop and the language-level lifetime honours the contract) **and** restated the precise invariant in the two doc comments (`game_loop.h::addTask`, `cyclic_task.h`) — "must outlive every call to `run()`," with a note that the pointer is dereferenced only while the loop executes. Also corrected "owned by the caller (App)" → "the composition root" in `cyclic_task.h`, since there is no `App` class yet. Docs + one declaration reorder; no behavioural change.

## Session 2026-07-06 — PDO remapping (roadmap #6): read + write the cyclic mapping over CoE

Roadmap item #6 (fieldbus capabilities, Session 2026-06-01) shipped: the user can now change *which* objects are in the cyclic image, not just view the existing mapping. End-to-end — `Device`/`DeviceManager` methods, `GET`/`PUT /api/devices/:slavePosition/pdo-mapping`, swagger + regenerated TS client, and a **PDO Mapping** editor page in the console.

**One CoE read source, two shapes.** The mapping is read over the CoE mailbox by walking the PDO assignment objects (`0x1C12` outputs/RxPDO, `0x1C13` inputs/TxPDO) and the mapping objects they reference (`0x16xx`/`0x1Axx`) via SDO upload. That single reader (`readPdoAssignment`) now feeds two views:
- **Grouped** (`Device::readPdoMapping → PdoMapping`): each mapping object keeps its `pdoIndex` and its own entries (with derived `bitOffset`) — the round-trippable shape the editor loads and the write echoes back.
- **Flat** (`Device::readFlatPdoMapping → FlatPdoMapping`, cached in `flatPdoMapping_`): one flat list of `PdoMappingEntry` per direction, what the process-image builder consumes. Now *derived* from the grouped read rather than a separate walk.

This drove the rename that touches the most files: `PdoMappings → FlatPdoMapping`, `readPdoMappings() → readFlatPdoMapping()`, `pdoMappings() → flatPdoMapping()` — "flat" is now explicit everywhere the old whole-image view is meant, so the grouped view owns the plain `PdoMapping` name. The CoE mapping-word pack/unpack (`index<<16 | subindex<<8 | bitLength`) moved to `pdo_mapping.h` alongside the grouped `to_json`.

**The write follows the ETG.1000.6 §5.6.7.4.9 ordering rule, transactionally.** `Device::writePdoMapping(const PdoMapping&)` reconfigures both directions in PRE-OP: clear the sync manager's PDO assignment to zero (which makes its mapping objects writable) → clear each mapping object's entry count → write the entries as packed `uint32` words → restore the entry count → finally write the assignment listing the mapping objects and its own count. A mapping object present before but absent from the new mapping is just left unassigned (no explicit clear needed — off the SM, its contents are irrelevant).

**Why PRE-OP only.** The mapping and assignment objects are writable *only* in PRE-OP — INIT/BOOT have no CoE mailbox, and in SAFE-OP/OP the sync managers are active so the slave aborts the write. This is the "drop to PRE-OP, remap, climb back" flow. The write is deliberately **transport-agnostic and does not itself change AL state or re-map the image**: the caller (the UI, via `POST /api/state`) drives the device back to SAFE-OP/OP, and `DeviceManager::transitionToState` re-reads the mapping and rebuilds the whole-bus process image — the same reactive-mapping path a firmware update already exercised (the 2026-05-16 "manual re-map may have changed it" clause). No new re-map machinery.

**Retry the whole sequence, not per-write.** After writing, the mapping is read back (`readFlatPdoMapping`, which also refreshes the cache) and compared against the request; a mismatch or a transient SDO failure mid-sequence retries the **entire** mapping apply a few times before failing. A single dropped mailbox frame would otherwise leave the OD half-configured (some objects written, the assignment count stale); the apply is idempotent so a whole-sequence retry is safe, and read-back is the only trustworthy success signal. Related same-device concurrency fragility (shared `socketMutex_` serializes individual transactions but not the multi-write sequence) is noted but not yet guarded — see the standing memory on PDO-mapping write concurrency.

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

**Fix 1 — self-heal refreshes on expiring-soon, not just missing/expired.** `healCertIfNeeded` (`cert_updater.cc`) now computes three states (`certMissing` / `expired` / `expiringSoon`, the last being `daysRemaining < kCertExpiryWarningDays` = 7) and fetches on any of them; a cert with ample life left still makes **no** network call, so a healthy boot is unchanged. The fetch stays `autoUpdate`-gated and best-effort — a present cert that fails to refresh is still served (fatal only when the cert is missing *and* unfetchable). This is what makes an ephemeral container self-heal on start, and it also fixes dev images for free: the entrypoint's last-resort **1-day self-signed** cert falls inside the 7-day window, so it now triggers a real-cert fetch on first run instead of being served as-is (the PWA rejects self-signed cross-origin). No entrypoint reorder needed.

**Fix 2 — the Docker image never actually baked a cert.** The build stage `touch`ed empty `cert.pem`/`key.pem` at the repo root, but the runtime stage copied only the binary — a runtime cert `COPY` was **never** in the Dockerfile (checked history; even the "bake release certs" commit lacked it). So every image shipped certless and self-signed. Fixed: the build stage now **fetches** cert/key from the rolling `tls-cert` release (fetch-to-`.new` + `mv`, empty-placeholder fallback so offline builds still succeed), and the runtime stage `COPY`s them into `/opt/motion-master/`.

**Full retirement of the `TLS_CERT`/`TLS_KEY` secrets and the `GH_PAT_SECRETS` PAT.** They duplicated the rolling `tls-cert` release — both were written by the same `cert-renewal.yml` run — so the secret path was redundant. The **rolling release is now the single source of truth**: `cert-renewal.yml` only publishes there (default `GITHUB_TOKEN`, `contents: write` — no PAT); `release.yml`'s three build legs `curl` it into the artifacts instead of `echo`ing the secrets; the Dockerfile `curl`s it at build. The `gh secret set` step is gone. Because the binary now self-heals at runtime regardless of install method, a baked cert is just a fresh offline seed, not load-bearing — the hybrid we settled on (bake for offline + refresh when stale). Supersedes the secret-based description in the CI section above and in Session 2026-06-06. The now-unused `TLS_CERT`/`TLS_KEY` secrets and `GH_PAT_SECRETS` PAT can be deleted from repo settings.

Also this session: `tools/docker-build.sh` (build + tag from `VERSION`; bare version + `latest`/`next` by stable/prerelease) and the README restructure (running vs. development, config-driven CLI, real platform/artefact set).

---

## Session 2026-07-09 — Trajectory playback: a fixed-membership RT task fed a precomputed setpoint buffer (design)

**Extends** *Session 2026-06-05 — RT tasks are fixed-membership; off-RT procedures are background jobs*. A `TrajectoryTask` that plays back a vector of position points one-per-cycle (chirp, step sequence, replay, arbitrary offline-generated signal) is a textbook **Category-1 RT cyclic procedure** — it writes a fresh target into the output region every cycle, phase-locked to the bus — and so it obeys every rule that session set: **one fixed-membership `CyclicTask`** registered before `run()` in `main.cc` alongside `ProcessDataTask`, a **no-op per device until activated**, iterating devices each cycle and holding only `DeviceManager&`; a **per-device control block** flipped active/idle by the HTTP thread; **the launch lives on the view** (`Cia402Drive::startTrajectory(...)`/`stopTrajectory()`), which does the op-mode + enable handshake synchronously off-RT so the RT side has no state-machine logic and no error branch; and the fixed pool is **one control-block slot per `Device`**. This is the SineWave design with the generator swapped for a buffer reader — the trajectory is *precomputed* rather than computed from scalar params each cycle.

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

**The handoff, RT (`TrajectoryTask::execute`, per device).** Re-resolve the `Device` via `DeviceManager::findDevice` each cycle (never cache across a rescan). `active.load(acquire)`; if false, skip. `buffer.load(acquire)` — the wait-free raw-pointer read; never dangling because the control plane retains it in `generations` until `reset()`/`scan()`, exactly as `ProcessData` retains superseded images. Keep the **playback cursor as RT-only scratch** (a member on the task, off the published block — the same rule that keeps SineWave's `phase_`/`center_` off its seqlock: the block is HTTP→RT only, readers must never see RT scribbling the cursor back), reset to 0 on a `generation` change. Write the current point via the existing lock-free output path `ProcessData::writePdo(slavePos, 0x607A, 0, bytes)` (Design B, Session 2026-06-05 — safe from RT). At end of buffer apply `endPolicy` and store `completedGeneration`. Ordering: runs *after* `ProcessDataTask` in registration order like SineWave (one cycle of staging latency — fine for a chirp; register before it only if same-cycle staging is worth the coupling).

**Stop, completion, lifetime — all deferred to off-RT, none on the RT thread.** *Stop* is `active.store(false)` plus the launcher's off-RT disable walk (state changes are never done on RT); the buffer stays alive in `generations` until the next `reset()`/`scan()` so an in-flight RT cycle never reads freed memory. *Completion* can't be an RT notification (no I/O on RT), so RT only flips `completedGeneration`; an off-RT watcher (a small poller, or folded into the existing `MonitoringManager` sampler thread) observes it and emits the WebSocket notification and/or auto-disables — the job of the planned `NotificationBus`. *Rescan safety*: the control block dies with its `Device`, so a trajectory must be stopped before a rescan (like `stopExchange`), and the task's per-cycle `findDevice()` must sit behind the same `scan()`/`reset()` drain the `ProcessData` path relies on — the same open guard flagged for SineWave.

**When the whole-buffer publish is wrong.** It fits a **finite, precomputed** signal — a 1 kHz chirp for a minute is ~240 KB, trivial. If the trajectory is **open-ended / streamed** (generated live, does not fit in memory), replace the single buffer with an **SPSC lock-free ring**: HTTP thread pushes chunks, RT pops one point per cycle, a low-water mark notifies off-RT to refill, with explicit underrun handling. More moving parts, and only worth it when the buffer genuinely cannot be materialised up front — not the case here, so start with the buffer publish.

**The model in one line.** *Trajectory playback is SineWave with the per-cycle generator replaced by a cursor into a precomputed buffer; every fixed-membership / view-launched / one-slot-per-`Device` rule carries over unchanged, and the only substitution is the control-block transport — `SeqLock<scalars>` becomes atomic-pointer-publish-with-retained-generations because the payload is a `vector`.*

## Session 2026-07-09 — The profile view is RT-callable: `Cia402Drive(device)->state()` works verbatim in RT and non-RT (design)

**The non-negotiable requirement.** `createCia402Drive(device)->state()` — and every other profile operation (`setControlword`, `shutdown`/`switchOn`/`enableOperation`, `setOperationMode`, `setTargetPosition`, all the CiA402 bit work) — must run **from the same call site, unchanged, on both the RT loop and an HTTP thread.** The alternative is a second copy of the state machine, the controlword-bit composition, and the mode handshakes living inside every RT task — a large, drift-prone duplication of exactly the knowledge `Cia402Drive`/`SomanetDrive` exist to hold *once*. So the profile layer stays single-source and RT-safety is pushed *below* it, into the one seam every profile method already bottoms out in: `Device::readValue<T>` / `writeValue<T>`. This is what an RT `TrajectoryTask` / `SineWaveTask` uses to drive state instead of reaching for `ProcessData::writePdo(pos, 0x607A, 0, bytes)` raw (as the 2026-07-09 Trajectory session sketched) — the raw call works but bypasses the profile, which is the very thing we refuse to reimplement.

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

**Multi-axis is ONE program with a column per axis — not N per-device blocks.** Decomposing a coordinated move into independent per-`Device` control blocks re-creates three problems in the RT path that a single object avoids for free: **atomic start** (axis 1 must not begin on cycle *N* while axis 2 begins on *N+3* — separate blocks need a start barrier the RT side polls, fragile against a rescan or a mid-arm fault), **drift** (each block would carry its own cursor with nothing keeping them in lockstep), and **joint fault/completion** (a fault on one axis should quick-stop the whole group — with separate blocks "the group" is an emergent property you must reconstruct). Model the move as one immutable `MotionProgram` (participating axes + a **row-major setpoint matrix** + one shared cursor) and advance **one cursor once per cycle** applied to every column — synchrony is then true *by construction*, because there is only one clock.

```cpp
struct MotionProgram {                       // immutable once published; mlock'd off-RT
  std::vector<uint16_t> axes;                // participating slavePositions; disjoint across programs
  std::vector<int32_t>  setpoints;           // row-major: [cycle * axes.size() + axisIdx] → 0x607A
  size_t                cycles;              // == setpoints.size() / axes.size()
  cia402::OperationMode mode;                // e.g. CyclicSynchronousPosition
  enum class EndPolicy : uint8_t { HoldLast, Stop, Loop } endPolicy;
  uint64_t              generation;          // bumped per publish so RT detects a new program
};
```

**Ownership moves off `Device` onto `DeviceManager`.** A coordinated move spans devices, so its block cannot live on any single `Device` — it lives on `DeviceManager`, the same place as `ProcessData`, the tree's other cross-device RT/non-RT channel. `DeviceManager` holds a **fixed pool** of published program slots (fixed membership preserved: what varies is the *contents* and *which axes*, both runtime data — never the number of tasks or slots), each an `std::atomic<const MotionProgram*>` with a retained-generations keep-alive vector, exactly the wait-free transport the Trajectory note chose (**not** `std::atomic<std::shared_ptr<>>`, whose `load()` is not lock-free on libstdc++). Independent groups run concurrently — left leg (3 axes) and right arm (4 axes) as two programs with independent cursors — with the invariant that **a given axis appears in at most one active program** (disjointness enforced by the launcher, so two programs never both write one axis's controlword/setpoints).

**Launch graduates from a view to a coordinator — but still composes views, so CiA402 logic stays written once.** A `Cia402Drive` view binds one `Device&`, so single-axis launch stays on the view (`Cia402Drive::startTrajectory`, prior note). Multi-axis cannot bind one view, so it becomes a node-layer free function `startCoordinatedTrajectory(DeviceManager&, spec) -> expected<void,string>` that (1) resolves each axis and builds a per-axis `Cia402Drive` — *reusing the single-device op-mode + `enable()` handshake per axis, off-RT, so the RT side has no state-machine logic and no error branch*; (2) checks the requested axes are disjoint from every active program and claims a free pool slot; (3) builds the immutable `MotionProgram`, `mlock`s it, retains the `shared_ptr` in the slot's generations, and publishes with one `buffer.store(ptr.get(), release)` → `active.store(true, release)`. The HTTP handler just forwards and serialises the `expected<>` — it never sees the task or `GameLoop`. Single-axis is the `N=1` special case of the same coordinator (keep the thin `Cia402Drive::startTrajectory` for the common single-drive path).

**RT scratch is keyed by pool-slot index, not a pointer→cursor map.** The naïve `std::unordered_map<const MotionProgram*, size_t> cursor_` grows unboundedly and hashes every cycle (a heap op on RT). Instead the task holds a **preallocated array indexed by pool slot**, and detects a newly-published program by comparing the loaded pointer against the last-seen pointer for that slot — which gives free, correct cursor reset on every re-arm (the whole point of latest-wins):

```cpp
struct AxisGroupScratch { const MotionProgram* seen = nullptr; size_t cursor = 0; };
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

1. **Lifecycle status (started / progress / done / faulted / aborted) → depth-1 latest-wins atomics on the control block.** RT flips `progress` / `status` / a bumped `statusGen`; the watcher polls and emits one notification *on change*. A dropped intermediate is harmless — newest is current. This is the inbound mailbox running RT→off-RT; the same `MotionProgram` struct carries both directions (different fields, different writers).
2. **Per-cycle telemetry ("what is the drive doing right now") → not a new channel at all; it is already monitoring.** The trajectory's setpoints/actuals land in the recorder ring every cycle and `MonitoringManager` already streams every recorded cycle losslessly. A live plot of the move is a *monitoring subscription* on those objects — the task sends nothing.
3. **Must-not-drop discrete events (each waypoint reached, distinct fault codes) → an SPSC ring the RT task produces into.** The one case that warrants a queue: each event is a distinct occurrence that latest-wins would collapse. RT pushes fixed-size records (event id, cycle, axis, code — scalars), the watcher drains and emits one message per record. Mirror of the "chaining needs a queue" escape hatch, on the outbound side; only when latest-wins genuinely loses information.

End-to-end for tier 1 — RT stores scalars, the off-RT watcher does all formatting and the publish:

```cpp
struct MotionProgram {                    // ... axes, setpoints, cycles, generation as above ...
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

*Inbound (client → RT): no thread — the reusable pieces are the transport, the pool, and a launch skeleton.* Three write-once pieces replace the per-task control-block boilerplate:

```cpp
template <typename T>                         // T = immutable payload (MotionProgram, …)
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
      [&]{ return buildMotionProgram(s); });    // matrix → immutable, mlock'd MotionProgram
}
```

`DeviceManager` owns one `RtMailboxPool<T>` per task family (`trajectoryPool()`, `sinePool()`); the RT task iterates `pool[i].current()` each cycle. Both directions use **composition, not inheritance** — lambdas/policies, no single-impl interface (repo rule). And **do not** generalize the HTTP *surface* into a command-bus routing a generic `POST /api/rt-tasks {type}`: routes stay explicit per `swagger.yml` and the plug-in design; only the plumbing *beneath* the handlers is generalized.

| | Outbound (RT → client) | Inbound (client → RT) |
|---|---|---|
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
