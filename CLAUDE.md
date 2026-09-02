# CLAUDE.md

Guidance for Claude Code (claude.ai/code) working in this repository.

## Project Context

**Motion Master v6.0.0** — motion control software for SOMANET servo drives. A clean-sheet
rewrite of the previous codebase.

**`NEXTGEN.md` holds every design decision and its rationale, one section per session.**
This file holds the rules. When you need to know *why* a rule exists, read the session
`NEXTGEN.md` names. Read `NEXTGEN.md` before making a structural change.

## Writing

**Read `docs/WRITING.md` before you write or edit any documentation, code comment, or
commit message.** It is the style guide for everything written down in this repository.
The base is ASD-STE100 Simplified Technical English: short sentences, active voice, one
term per concept, plainest word available.

Three rules that catch most of it:

- **Clarity beats concision.** The sentence limits apply per sentence, not to the whole
  text. Never drop a fact or a caveat to make something shorter. Split the sentence.
- **Define a domain term at first use in documentation. Never define one in a code
  comment.** A reader of `README.md` may be new. A reader of
  `soem_fieldbus_driver.cc` is not.
- **A comment says why and states the present.** Never write what the code used to be.

Replies in an interactive session follow a different style. `docs/WRITING.md` does not
govern them.

## Build System

CMake 4.0+, Ninja, vcpkg. Initialise the submodule before the first build:

```bash
git submodule update --init --recursive
```

Presets: `x64-linux-debug` (default), `x64-linux-release`, `x64-windows-debug`,
`x64-windows-release`. Output goes to `build/<preset>/`.

C++23. Warnings are errors: `-Wall -Wextra -Wpedantic -Werror`, or `/W4 /WX` on MSVC.

### Scripts

Wrapper scripts live in `tools/`. Each takes an optional preset name as the first argument.

| Script | Does |
| --- | --- |
| `install-deps.sh` | Install OS packages (Debian/Ubuntu + Fedora). `--dry-run` to preview |
| `configure.sh` | `cmake --preset` |
| `build.sh` | Build. `--setcap` also grants raw socket + RT access via sudo |
| `build-dev.sh` | Build with `--setcap`. `--no-setcap` skips sudo |
| `run.sh` | Generate a temporary self-signed cert and run the binary |
| `test.sh` | `ctest --output-on-failure` |
| `format.sh` | clang-format all sources |
| `format-cmake.sh` | cmake-format all CMake files. Needs `pip install cmakelang` |
| `lint.sh` | cpplint. Needs `pip install cpplint` |
| `cppcheck.sh` | cppcheck |
| `tidy.sh` | clang-tidy, bug-finding checks only |
| `shellcheck.sh` | shellcheck every tracked shell script |
| `check.sh` | format + cppcheck + lint + cmake-lint + shellcheck |
| `clean.sh` | Remove `build/<preset>` |
| `package.sh` | Build .deb and .rpm. Needs cert.pem/key.pem in the build dir |

Raw equivalents:

```bash
cmake --preset x64-linux-debug
cmake --build --preset x64-linux-debug
ctest --test-dir build/x64-linux-debug --output-on-failure
```

## Versioning

`VERSION` at the repo root is the single source of truth. CMake reads it and generates
`libs/core/version.h` and `Doxyfile`. Never edit those two by hand.

```bash
./tools/bump-version.sh 6.0.0-alpha.1
```

The script also updates `vcpkg.json`, the root and per-package `package.json` files,
`hil/api/package.json`, `apps/motion_master/swagger.yml`, the assertion in
`libs/core/tests/version_test.cc`, the sidebar badge in
`web/apps/console/src/layouts/RootLayout.tsx`, and `motion_master_version` in
`rt/provision/ansible/roles/motion-master/defaults/main.yml`.

After bumping: commit, then push a `v<version>` tag. That triggers `release.yml` and
publishes `@synapticon/motion-master-client` to npm.

**Everything ships one version, on purpose.** The binary, the `web/apps/*` PWAs, and the
TypeScript client all depend on one contract: the HTTP API plus the WebSocket protocol.
One version number states they were built against the same contract, so there is no
compatibility matrix. A web-only fix still needs a version bump and a tag, because the
hosted PWAs are pinned to the latest tag rather than to `main`. To learn *what* changed,
read the commit scopes, not the number. Rationale: `NEXTGEN.md`, Session 2026-07-16.

**Any change to the HTTP API or its behaviour requires updating
`apps/motion_master/swagger.yml` in the same change.** It is the spec the PWA's API Docs and
the generated clients come from.

The generated client in `web/packages/motion-master-client/src/generated/` comes from
`swagger.yml` via `pnpm --filter @synapticon/motion-master-client generate` and **is
committed**. Regenerate and commit it whenever the API shape changes. The `api-client-drift`
CI job regenerates from `swagger.yml` and fails on any diff, so a stale client is caught.

## CI

Four build workflows run on every push and PR: `build-linux-x64.yml`,
`build-linux-arm64.yml`, `build-macos-arm64.yml`, `build-windows-x64.yml`. They cache
vcpkg binaries with `actions/cache@v5`, keyed on OS plus the `vcpkg.json` hash.

**Do not use the `x-gha` vcpkg binary caching backend.** It was removed in the pinned
vcpkg version.

Four more workflows:

- **`cert-renewal.yml`** — monthly. Issues a Let's Encrypt cert via acme.sh with the
  `dns_acmedns` plugin and publishes it to the rolling `tls-cert` release. Needs only the
  `ACMEDNS_CONFIG` secret.
- **`release.yml`** — on `v*` tags. Four build legs produce eight artifacts (tar.gz, deb
  and rpm for both Linux architectures, a Windows zip, a macOS tar.gz). Packages install
  to `/opt/motion-master/`. Certs are marked as config files so an upgrade never
  overwrites them.
- **`lint.yml`** — clang-format, cpplint, cppcheck, the API client drift check, and the
  Python client example.
- **`deploy-pages.yml`** — publishes `motion-master.synapticon.com`. Docs and landing
  track `main`; the web apps are pinned to the latest `v*` tag and cached per tag.

The arm64 release leg builds inside a `debian:13` container because Debian aarch64 is the
target. **The glibc floor is set by the symbols the binary references, not by the build
host.** Measure it with `readelf -V`. Both x64 and arm64 currently need `GLIBC_2.38` and
`GLIBCXX_3.4.32`. Do not quote the container's distro version as the requirement.

## Docker

Two-stage build. The binary lands in `/opt/motion-master/`, matching the deb and rpm path.
`docker-entrypoint.sh` mirrors the cert discovery order of `tools/run.sh`.

Docker ignores file capabilities, so `setcap` does nothing in a container. Use `--cap-add`:

| Capability | For |
| --- | --- |
| `CAP_NET_RAW` + `CAP_NET_ADMIN` | SOEM raw sockets and promiscuous mode |
| `CAP_SYS_NICE` | `SCHED_FIFO` on the game loop thread |
| `CAP_IPC_LOCK` + `--ulimit memlock=-1` | `mlockall()` to pin process memory |

Missing RT capabilities produce a warning and the loop runs non-RT. Missing EtherCAT
capabilities make `POST /api/init` fail for a SOEM driver.

## Architecture

### Directory Layout

```text
motion-master/
  apps/
    motion_master/     ← main executable; swagger.yml (the OpenAPI spec) lives here
    playground/        ← scratch binary
  libs/
    core/              ← version, CyclicTimer, CyclicTask/CycleContext, RT setup, utils
    etg/               ← mm::etg: ESI and ENI XML, both directions. Offline
    comm/              ← fieldbus interfaces; soem.cc, spoe.cc, igh.cc
    node/              ← Device, DeviceManager, CiA402, profiles, RT tasks. No HTTP
    api/               ← mm::api: HTTP glue. The only lib that knows uWebSockets
    example/           ← copy-me starter: a route plug-in and a cyclic task
  hil/
    jitter_bench/      ← RT jitter benchmark (Linux only)
    api/               ← HTTP + WebSocket integration tests (TypeScript / Vitest)
  cmake/lint.cmake     ← lint, cppcheck, format targets
  packaging/postinst   ← deb postinst; sets capabilities
  extern/vcpkg/        ← git submodule
  tools/               ← developer scripts
```

Flat layout inside each lib is intentional. Navigate by filename and grep.

### Design Rules

- **No exceptions.** Return `std::expected<T, std::string>`. `std::string` is the default
  error type. Promote to a structured `Error` (a POD enum plus a message, never a class
  hierarchy) only on the surface where a caller must *branch* on the failure reason.
  String-matching an error message to pick a branch is the signal that a surface has
  earned one. Do not sweep the codebase to a shared error type.
  `libs/comm/foe_error.h` is the worked example, and is deliberately unused.
  See `NEXTGEN.md`, Session 2026-07-17.
- **Two servers, two ports, two threads.** HTTP on 61447, WebSocket on 62281. Separate
  uWS apps and loops, so a slow HTTP handler cannot stall the WebSocket. No Protobuf.
- **No service layer.** SDO, file transfer, state control, and bus inspection are methods
  on `Device` and `DeviceManager`.
- **`Device` and `DeviceManager` are profile-ignorant.** `device_manager.h` names no
  profile type. Everything profile-shaped builds outward from them. The payoff: a new
  profile is a view class, an `X_control.h`, and route registration, with
  `device_manager.{h,cc}` untouched. See `NEXTGEN.md`, Session 2026-08-02.
- **`main.cc` is the composition root.** It is the only place concrete types are
  instantiated. `HttpServer::Config` carries `std::function` callbacks so the server names
  no concrete collaborator.
- **Namespaces mirror directories** (`mm::core`, `mm::comm::soem`, `mm::node`, `mm::api`).
  Do not use C++20 modules.
- **Config is JSONC.** Parse with `nlohmann::json::parse(stream, nullptr, false, true)` —
  the third argument disables exceptions (check `is_discarded()`), the fourth enables
  comments. Settings are config-file-only; there are no CLI flags for them.
  `--config <path>` wins, otherwise a `motion-master.jsonc` next to the executable is
  loaded, otherwise the built-in defaults apply. There is no `/etc` search path.

### Extension Points

Two tiers, both with a starter in `libs/example/`.

**Tier 2 — HTTP route plug-in.** A plug-in lib registers its own paths
(`/api/yourapp/...`, never `/api/*` or `/*`) via `HttpServer::addRoutes(fn)`, called from
`main.cc` **before** `start()`. The transport glue is `libs/api/web_api.h` — `RouteContext`
(references to `DeviceManager` and `MonitoringManager`, plus `corsOrigin`), `RegisterRoutesFn`,
and the `sendJson`/`sendError`/`sendStatus` helpers. **That header is the only place that knows
uWebSockets. Do not add HTTP or uWS to `mm::node`.** Keep domain logic in `*_logic.{h,cc}` (HTTP-agnostic and
unit-testable) and formatting in `*_routes.cc`. A plug-in may spawn its own off-RT
`std::jthread`. **Do not add plug-in routes to `swagger.yml`** — that documents the
built-in API only. See `NEXTGEN.md`, Session 2026-06-29.

**Tier 3 — cyclic task.** Add a `CyclicTask` and run control code inside the RT loop.
See *Cyclic Tasks and RT Value Access* below. `main.cc` registers the example task behind
three commented lines.

### Class Structure

```text
main.cc  (composition root)
 ├── Config (CLI options)
 ├── mm::node::DeviceManager
 │     ├── shared_ptr<DeviceSet>        published generation: driver + devices + generation
 │     │     └── shared_ptr<FieldbusDriver>  SoemFieldbusDriver | SpoeFieldbusDriver
 │     ├── unique_ptr<ProcessData>      published image + generations + recorder ring
 │     ├── ParameterCache               on-disk OD cache, control-plane only, stable pointer
 │     ├── vector<Device>               each borrows FieldbusDriver& + ProcessData*
 │     │     ├── slavePosition, name, vendorId, productCode, revisionNumber, serialNumber
 │     │     ├── parameters_            (index, subindex) → DeviceParameter
 │     │     ├── flatPdoMapping_        cached flat view
 │     │     └── parametersMutex_
 │     └── init(), scan(), reset(), configureProcessData(), exchangeProcessData(),
 │         transitionToState(), deviceAt(), deviceSet()
 ├── GameLoop  (RT thread, SCHED_FIFO, 1 ms default; blocks the main thread)
 │     └── CyclicTask[]  → ProcessDataCyclicTask
 ├── HttpServer      (port 61447, own loop and thread)
 ├── WebSocketServer (port 62281, own loop and thread)
 ├── MonitoringManager  (off-RT; owns ParameterRefresher and a sampler thread)
 ├── ProcedureManager   (off-RT jthreads, busy token, retained snapshot; poll-only)
 └── NotificationBus    (off-RT poll thread; Source[] → the "notifications" topic)
```

Planned, not in code: `SetpointCyclicTask`.

**The motion layer has a settled design and a name. Build to it.** `SetpointCyclicTask` plays a
precomputed buffer, one setpoint per axis per cycle, in CSP, CSV or CST. The mode picks the target
object: 0x607A, 0x60FF or 0x6071. The RT side holds a cursor and nothing else. It runs no state
machine, computes no waveform, and has no error branch, because the launch path does the op-mode
and enable handshake off-RT before it arms the mailbox. Waveform maths lives in pure functions in
`libs/node/setpoint_generators.{h,cc}`, which the API exposes twice: a preview endpoint, and a
`{generator, params}` pair the launch request accepts in place of an explicit `points` array. A
`relative` program is offset by the current value once, at arm time, on the launch thread. Skip
handling is a launch parameter: `Sequential` advances the cursor by one and preserves the shape,
`RealTime` computes `cursor = ctx.elapsed - startCycle` and preserves the timing. See `NEXTGEN.md`,
Sessions 2026-07-09, 2026-07-13, 2026-07-14 and 2026-08-24.

**Both directions already have a settled shape, so build to it rather than re-deriving one.**
Inbound is a `RtMailboxPool<T>`: a depth-1 latest-wins mailbox per slot, where a newer intent
supersedes an older one and nothing queues. The pool is **owned by the composition root** and
injected by reference into the RT task (which reads) and the launch path (which writes), so
producer and task never name each other and only `main.cc` names the concrete task. Outbound is
the `NotificationBus`, which ships. **The RT thread never touches the WebSocket, and `node` never
names `WebSocketServer`.** See `NEXTGEN.md`, Sessions 2026-07-09, 2026-07-13, 2026-07-14 and
2026-08-24.

**A notification source is a version counter plus a renderer.** The RT thread writes plain scalars
into storage that already exists, then bumps a counter with `release`. `NotificationBus` polls
every counter, and when one has moved it calls that source's `render` off the RT thread to build
the message. A counter, not a flag: several bumps between two polls coalesce into one message
carrying current state, and there is no clear step to lose an update against. `render` returns
`std::optional`, so a source that has gone idle says nothing rather than inventing an event, and it
may log as well as return a payload — `busHealthSource` does both, because a warning is what
reaches a support log from a machine nobody was watching. Membership is fixed before `start()`, the
same rule `CyclicTask` follows.

**Each source sets its own `interval`, and that one number is both its latency floor and its
message ceiling.** A source read once a second cannot speak more often than that, so no separate
rate limit exists. The bus sleeps until the earliest source is due, the way `MonitoringManager`'s
sampler does, so no source pays for another's cadence. Bus health reads every second.

**Every notification goes to the single `"notifications"` topic.** A client subscribes once and
keeps receiving events added in a later version. A topic per source would break that: uWebSockets
20.77 matches topic names exactly, with no wildcard, so "give me all notifications" is not
expressible and a new source would silently reach no existing client. Clients tell events apart by
`data.event`. Per-topic fan-out is for the monitoring streams, where a client chooses what it pays
for.

**Profile views are borrowed, not owned.** `ProfileDevice ← Cia402Drive ← SomanetDrive`
each hold only a `Device&`, which is the only data member permitted in the chain. Build
one with `createCia402Drive(Device&)` or `createSomanetDrive(Device&)`; both validate
before binding. A view must never outlive its `Device&` or be cached across a rescan.

**Each view header has a control header, 1:1.** `profile_device.h` → `profile_control.h`,
`cia402_drive.h` → `cia402_control.h`, `somanet_drive.h` → `somanet_control.h`. Domain
logic lives on the view. A control function borrows, binds, delegates, and nothing else.
Off-view code reaches a profile operation through a control free function, never through a
`DeviceManager` method. See `NEXTGEN.md`, Session 2026-08-02.

### Key Types

```cpp
using DeviceParameterValue = std::variant<
  int8_t, int16_t, int32_t, int64_t,
  uint8_t, uint16_t, uint32_t, uint64_t,
  float, double, std::string, std::vector<uint8_t>
>;
```

`DeviceParameterValue` is the interchange type in signatures, not where the bytes live. A
scalar lives in `DeviceParameter::bits`, a `uint64_t` of raw little-endian wire bytes read
through `std::atomic_ref`. Strings and blobs live in `rawValue`. The immutable `dataType`
decides which field holds the value, so a value has exactly one home. `currentValue()`
rebuilds the variant on demand; `scalar<T>()` and `rawValueBytes()` read storage directly.

`ObjectAddress<T>` (`libs/node/device_parameter.h`) carries an index, a subindex, and the
C++ type together, so a call site does not retype it.

### Locking

**Device lifetime is a refcount, not a lock.** `DeviceSet` holds the driver, the devices, and
the topology generation. `init` and `scan` publish a new one; nothing modifies a published
set. Off the RT thread, `deviceAt(pos)` returns a `DeviceHandle` and `deviceSet()` returns a
`shared_ptr<DeviceSet>` — either one keeps its devices constructed for as long as the caller
holds it, with no lock held. The RT thread reads a raw published pointer instead, replaced
only with the cycle drained, because `std::atomic<std::shared_ptr<T>>` is not lock-free.

**A rescan never waits for a reader, and never invalidates one.** `scan` publishes a new set;
the old set dies when its last holder drops it. A procedure that was running keeps working
against its own device, and its next bus transaction fails against hardware that has moved.
That replaced a design in which a long borrow and a rescan excluded each other.

Three locks, one order:

```text
busOperationMutex_ → Device::parametersMutex_ → controlPlaneMutex_
```

- **`busOperationMutex_`** (plain) is a token over an *activity*: one control-plane
  operation drives the bus at a time. It guards no member and no reader takes it. Holding it
  is what keeps the published set from changing under an operation.
- **`Device::parametersMutex_`** guards `parameters_` against the off-RT monitoring
  threads. **Never hold it across bus I/O** — snapshot, transfer, then re-find.
- **`FieldbusDriver::controlPlaneMutex_`** serialises mailbox, SDO, FoE, ESC register, and
  state access. Hold it for one socket transaction only, never across a sleep, a blocking
  wait, or a user callback.

Two leaf locks sit outside that order and are never held while anything else is acquired:
`DeviceManager::currentSetMutex_`, held only long enough to copy a `shared_ptr`, and
`processDataMutex_`, which guards the recorder ring's storage and the retained image
generations against `allocate`/`clear`.

**The PDO path runs lock-free.** `exchangeProcessData` touches the IOmap, which is disjoint
from the control plane, and SOEM's port layer is internally thread-safe. A slow SDO never
blocks the RT cycle.

Rationale and the full inventory: `NEXTGEN.md`, Sessions 2026-08-08, 2026-08-09 and
2026-08-19. Reference: `docs/LOCKING.md`, `docs/THREADS.md`.

### Game Loop and RT

`GameLoop::run()` blocks the main thread, and that thread *is* the RT thread. Every other
subsystem starts its own threads first. A signal sets an atomic flag checked after each
cycle.

At the top of `run()`, `mm::core::setRealtimePriority()` (`libs/core/realtime.{h,cc}`)
raises the thread to `SCHED_FIFO` priority 80 and calls `mlockall(MCL_CURRENT | MCL_FUTURE)`
so a page fault cannot inject a latency spike. On Linux the policy is ORed with
`SCHED_RESET_ON_FORK`. Both steps are best-effort and independent: the routine reports what
succeeded and the caller warns, so a process without `CAP_SYS_NICE` or `CAP_IPC_LOCK` still
runs, just non-deterministically. `hil/jitter_bench` calls the same routine, so it measures
what ships.

**Priority 80 is unverified against threaded IRQs under PREEMPT_RT.** Confirm with
`ps -eo pid,cls,rtprio,comm | grep irq` before DC SYNC0 relies on it. See `NEXTGEN.md`,
Session 2026-07-30.

**The period is runtime-adjustable.** `setPeriod()` stores it in a relaxed atomic the RT
loop reloads each iteration. Applying it re-anchors the deadline grid (otherwise the old
grid reads as a backlog and produces a phantom skip burst) and starts a fresh health epoch
on the RT thread. The change is transient and does not rewrite the config file. The HTTP
surface is `PUT /api/game-loop`.

**Task membership is fixed.** All tasks are registered before `run()`. `GameLoop` never
adds or removes one at runtime. Today only `ProcessDataCyclicTask` is registered.

**Task time is blocking wire I/O, not compute.** `health()` brackets the `execute()` loop,
which is dominated by the EtherCAT frame round-trip: a `sendto` syscall, then a blocking
receive. On a consumer laptop NIC 100–300 µs is normal and harmless — the core is parked on
the wire, not spinning. The signature of a real problem is `max` approaching `EC_TIMEOUTRET`
(2000 µs) with `skippedCycles` climbing. To improve it: a PREEMPT_RT kernel, an Intel
server NIC, `ethtool -C <iface> rx-usecs 0 tx-usecs 0`, and CPU isolation.

#### Cycle Timer

`CyclicTimer` (`libs/core/cyclic_timer.{h,*.cc}`) uses one absolute-deadline model on all
three platforms, so per-cycle jitter never accumulates into drift. Linux uses
`clock_nanosleep(TIMER_ABSTIME)`, macOS `mach_wait_until()`, Windows a per-cycle one-shot
`CreateWaitableTimerEx` with a relative due time computed from a QPC grid. The QPC grid is
what lets Windows honour non-integer-millisecond periods.

**Overrun policy is skip-to-grid, not catch-up.** When a deadline is already past,
`waitForNextCycle()` fast-forwards to the next future grid point, preserves the phase, and
returns how many cycles it skipped. A back-to-back burst would send stale frames the drives
cannot use. `GameLoop` accumulates the count into `skippedCycles()` silently — no logging on
the RT path.

**Sustained overrun is expected on coarse-timer machines**, which most Windows hosts are. A
1.5 ms timer floor against a 1 ms grid runs at ~667 Hz with ~333 skips per second, each
executed cycle still phase-locked. The fix is operational, not code: **a `skippedCycles()`
that climbs steadily right after start means the period is too aggressive for that hardware.
Raise it.**

### Cyclic Tasks and RT Value Access

The Tier-3 surface, designed so control code can be written by a controls engineer rather
than by someone who has read `docs/LOCKING.md`. See `NEXTGEN.md`, Session 2026-08-10.

**The contract is one sentence, and the naming convention already encodes it.**
`Device::value<T>()` and `setValue<T>()` are the entire RT surface. They never block, never
allocate, and never touch the wire. `readParameter` and `writeParameter` are the
synchronous control-plane calls. **A cyclic task cannot tell a PDO-mapped object from an
SDO-polled one**, because whether a value is in the process image is a commissioning
decision.

- **The cell is the storage.** Every `DeviceParameter` carries `std::atomic<uint64_t> bits`
  plus a monotonic `stamp` (0 means never written). Strings and blobs sit behind an
  `std::atomic<const std::string*>` into a per-device arena of retained immutable values, so
  an RT reader gets a `string_view` with no lock and no copy.
- **Use `std::optional<T>`, never `std::expected<T, std::string>`, on the RT path.**
  Building the error string allocates. This is the one place the error convention does not
  apply.
- **A read of a non-exchanging device returns the last known value**, not `nullopt`.
  `stamp` and `exchangesProcessData()` are there for a task that wants to decide otherwise.
- **The cells are the storage, and nothing destroys one while its device lives.** Each `Device`
  owns a deque of `DeviceParameter` cells; `parameters_` maps `(index, subindex)` to a pointer
  into it. A re-enumeration rebuilds the map and reuses every cell whose data type and bit
  length are unchanged, so a held `DeviceParameter*` survives it and a value written before it
  survives too. A changed declaration gets a new cell, because the RT decode reads `dataType`
  and `bitLength` without a lock.
- **Freed as soon as it is safe, never retained beyond that.** A retired device set goes away
  when its last `DeviceHandle` releases it, so memory does not grow with rescans. While a
  holder has it, the device is valid but no longer fed: reads serve the last values, writes
  reach no wire. `topologyGeneration()` is how a holder notices.
- **`GameLoop` enters the cycle; a task writes no guard.** The loop takes one
  `DeviceManager::CycleGuard` around the whole task list each cycle and calls no task when it
  is falsy. So a task's `findDevice` / `findParameter` results are valid for the body of its
  `execute()` by construction, and a task cannot forget to make them so. `findDevice` and
  `findParameter` stay public and non-locking because the RT thread must not block.
- **Lifetime: a pointer is valid for one `execute()`.** Resolve each cycle and cache nothing
  across cycles. A re-enumeration keeps the cells, so it cannot dangle a pointer; a `scan()` or
  `reset()` frees the retired devices once their holders let go, and the loop's cycle guard is
  what keeps a task out of that window.
- **The RT loop does no lookups.** `ProcessImageEntry` carries the owning
  `DeviceParameter*`, resolved at publish and refreshed on every re-map. A lookup per mapped
  object per cycle is fatal at bus scale: 50 devices × 40 objects is 60–100 µs against a
  1 ms grid. A task's own reads are per-signal, which is fine.
- **The decode is eager.** Right after the frame arrives, the RT thread copies every mapped
  object into its cell whether anything reads it or not. This is a read-path decision: it
  makes `value<T>()` a hash lookup plus an atomic load, instead of making every reader
  locate the object in the published image. Eager loses the sparse case. The escape hatch,
  if it ever bites, is a per-entry "someone has bound this" flag.
- **There are no output staging slots.** The composer reads each output entry's cell
  directly, so a write reads back as itself and a re-map has nothing to seed.
- **SDO objects are polled into their cells** by `ParameterRefresher`, owned by
  `MonitoringManager`. `keepFresh(pos, index, subindex, period)` and `stopKeepingFresh` are
  the Tier-3 door. Registration is off-RT; an RT task can never register.
- **Writing a non-PDO-mapped object from a cycle is deferred.** `setValue<T>()` stores the
  cell, and if the object is not output-mapped nothing transmits it. Use `writeParameter`
  off the RT thread.
- **CiA402 needs nothing new.** `libs/node/cia402.h` is pure `constexpr`, so a task reads
  0x6041 from a cell and decodes inline.
- **Position is not identity, and nothing enforces that.** Inserting a device shifts every
  position after it, so a task pinned to position 4 can silently drive different hardware
  after a rescan. `topologyGeneration()` is the mechanism. Inserting a node is a
  commissioning act: rescan and restart.

### Process Data

`ProcessData` (`libs/node/process_data.h`) is owned by `DeviceManager` and handed to each
`Device` by raw pointer.

**Inputs and output read-back come from the recorder ring** (`ProcessDataRing`), a lock-free
circular recorder the RT loop appends to **every cycle**: raw input and output IOmap, an
epoch-ns timestamp, and the working counter. It is the single RT-written structure and the
source for live monitoring, point reads of the freshest value, and the `.mmpd` dump
(`POST /api/process-data/dump`). The RT `write()` is wait-free; readers re-check a per-slot
release-stored sequence number after copying.

The ring is allocated and `mlock`'d at `configureProcessData` for `recorder.capacity`
cycles. The size is a fixed row count, independent of the loop period, because records carry
absolute timestamps. Roughly 128 bytes per cycle for a single drive, so the default 300000
cycles is about 38 MB and about 5 minutes at 1 ms. It is re-allocated on a layout-changing
re-map, retained across image teardown, and freed only by `reset()` or `scan()`.

**Outputs are written lock-free into the owning parameter's cell.** Writers store into
different objects without contending; same-object writes are last-writer-wins. The RT loop
is the only thread that composes cells into the packed wire image, which is what makes
bit-packed objects sharing a byte safe without a lock.

An atomically published `image` pointer gates all of it. Readers load it lock-free.
`readPdo` falls back to SDO when no image is published, nothing has been recorded, or (for
inputs) `lastWkc < expectedWkc`.

**Control-plane versus RT.** `init`, `reset`, and `configureProcessData` run on the HTTP
thread and mutate `driver_`, `devices_`, and the IOmap. `exchangeProcessData()` runs on the
RT loop. The boundary is the published image pointer — control-plane operations publish
`nullptr` first, so exchange becomes a no-op — plus `stopExchange()`, which drains an
in-flight cycle. Published images are retained until `reset()`, so the RT thread never reads
freed memory.

### AL State and Re-mapping

Changing AL state is the user's job, via `POST /api/devices/state`. Motion Master reacts in
`DeviceManager::transitionToState`. The image is one whole-bus layout, but the logic supports
partial-bus operations.

- **Entering SAFE-OP or OP** re-maps when there is no published image, or when any targeted
  device is rejoining from a non-exchange state. Its PDO mapping is re-read, because a
  firmware update may have changed it. A device already exchanging that is merely
  re-commanded skips the re-map.
- **Leaving SAFE-OP or OP** tears the image down only when no device will remain exchanging.
  Otherwise the leaving device drops out and `updateExpectedWkc()` removes its share.

So a device can go to BOOT or PRE-OP while the others keep exchanging. Re-mapping briefly
pauses the whole bus, which is the accepted cost.

**Two rules the re-map depends on:**

1. **Illegal AL transitions are rejected up front** (`kValidStateTransitions` in
   `device_manager.cc`). This is not UX. The re-map reads PDO mapping over the CoE mailbox,
   which is only live from PRE-OP up, so a device commanded straight from BOOT would reach
   that read while still in BOOT and **segfault inside SOEM** before the slave could reject
   it.
2. **Per-slave FMMU state is reset first.** `configureProcessData()` runs
   `ecx_config_map_group` without an `ecx_config_init`, but SOEM's mappers start at
   `FMMUunused` and append, trusting that init memset it. The driver zeroes each slave's
   `FMMU[]` and `FMMUunused` and FPWR-clears the ESC registers before every map. Without it,
   a re-map after a BOOT excursion writes an out-of-bounds `ec_fmmut` past the array and
   corrupts adjacent `ec_slavet` fields.

### Procedures

Off-RT command-and-wait work (offset detection, auto-tuning, firmware) runs on a cancellable
`std::jthread` owned by `ProcedureManager` (`libs/node/procedure_manager.{h,cc}`), which
holds the per-device busy token and the retained `ProcedureSnapshot` a client polls.
**Poll-only** — it names no WebSocket and holds no publish callback.

A body is a plain callable `(Device&, ProgressReporter&, std::stop_token)`. Adding a
procedure never touches the manager, and the manager names no profile type.

**The registry is one table**, `libs/node/procedure_catalogue.{h,cc}`. Each
`ProcedureCatalogueEntry` is a `ProcedureDescriptor` (name, title, description, caveats, `movesMotor`, `requiresEnabled`, step
template), an `applies(Device&)` predicate, and a `makeBody(json)` factory. Four handlers
serve every procedure, so **adding one is a row in that table and touches no route.**

Two rules the catalogue enforces:

- **`applies` may only consult state that exists as soon as the device does** — the vendor
  ID from SII, not `createSomanetDrive`, whose CiA402 check needs the object dictionary to
  have been enumerated. Binding to that would report a real drive as having no procedures.
- **Never-run is a state, not a 404.** The singleton `GET` returns 200 with an idle
  snapshot, because the manager learns a step template only when a run starts, but the
  catalogue holds it.

`ProcedureError` maps to status in one place: `kBusy`→409, `kUnknownDevice`→404,
`kUnknownProcedure`→404, `kInvalidRequest`→400.

**There is one body shape, and a body may do anything.** It takes a `ProcedureContext`
(`manager`, `device`, `devicePosition`), and the run holds a `DeviceHandle` rather than a
lock — so a body may call `transitionToState`, which firmware installation is defined by.
Two consequences: a rescan can interleave any run, so `discardIfRescanned` must skip running
entries; and nothing blocks a concurrent `scan()`. See `NEXTGEN.md`, Session 2026-08-02.

### Bulk Data for an OS Command (`fs-buffer`)

A command that carries more than the eight bytes of 0x1023:01 moves it through `fs-buffer`, the
third argument of `SomanetDrive::runOsCommand`. **`fs-buffer` is not a file.** The firmware matches
the name exactly and connects the FoE transfer to 2024 bytes of memory shared with the command that
is running.

Three rules, and none is a preference:

- **The transfer sits between the write to 0x1023:01 and the first poll.** The drive holds the
  command in progress until every byte has moved, and a command whose output is larger than 2024
  bytes stalls on a full buffer until the master drains it. Circulo with a safety module sends 2517
  bytes, so a transfer deferred until the command finished would hang on that device and work on
  every other one.
- **A failed transfer is recorded, not returned.** A command the firmware does not support produces
  nothing, and the slave answers a read of an empty buffer with *silence* — no `FOE_BUSY`, no error
  — so `ecx_FOEread` fails on a 700 ms mailbox timeout while the drive is about to say "unsupported
  command". The poll runs anyway and the drive's verdict wins. The transfer's own error surfaces
  only when the drive reports success and the payload is still missing.
- **Firmware 5.2 or newer only.** Older firmware wants the payload written before the command. That
  path is deliberately not built, and nothing reads 0x100A to detect it.

`decodeObjectDictionaryValues` splits OS command 21's output, which is a bare concatenation of
values ordered by `(index, subindex)` with no lengths on the wire. It takes the widths from
`Device::parametersOrdered()`, so **one width the master and the drive disagree about shifts every
value after it** — the widths must sum to exactly the bytes received or the whole transfer is
refused. Rationale, with the firmware citations: `NEXTGEN.md`, Session 2026-08-25.

### Networking and TLS

Binds to `127.0.0.1:61447` (HTTP) and `127.0.0.1:62281` (WebSocket), separate loops and
threads. `server.bindAddress` (default `"127.0.0.1"`) is **validated non-empty**, because
uWebSockets reads an empty host as every interface. The API is unauthenticated, so binding
all interfaces must be spelled out, never fallen into.

CORS allows `https://motion-master.synapticon.com`. The PWA connects to
`https://local.motion-master.synapticon.com:61447` and `wss://…:62281`.

**One certificate carries both names**: `local.motion-master.synapticon.com` (loopback) and
the `*.ip.motion-master.synapticon.com` wildcard (off-loopback; `192.168.1.50` is reached as
`192-168-1-50.ip.…`), issued as two SANs. Every host serves the same file, so there is
nothing per-deployment to configure.

**The `ip.…` names are deliberately not in DNS.** The client resolves them with a hosts-file
entry, which is sufficient because TLS validates a certificate against the *name*, never
against how it was resolved. This keeps the scheme infrastructure-free and removes the abuse
surface, which matters because the private key ships in every public release. The cost is one
hosts line **on each machine running a browser**, so it rules out phones and tablets. The
console's Connection page shows the line; `add-host.sh` and `add-host.ps1` write it. Clients
that cannot edit a hosts file use the plain IP and accept an exception once per origin.

Renewal is automated in `cert-renewal.yml` via DNS-01 with acme-dns delegation. acme.sh reads
its credentials from `ACMEDNS_*` environment variables, not a file. The renewed pair is
published to the rolling `tls-cert` release, which is **the single source of truth** — the
binary's self-heal, the Dockerfile, and `release.yml` all read from it.

**Cert self-heal.** An expired cert blocks the PWA's cross-origin `fetch()` with no browser
click-through, so the API cannot fix the failure it would address. `cert_updater.{h,cc}`
downloads a pair, validates it (parses, covers every name we serve via `X509_check_host`, not
expired, key matches cert), and installs atomically. At startup a missing, expired, or
expiring-soon cert (within 7 days) triggers a fetch before binding TLS. Missing plus
fetch-failure is fatal; a present cert that fails to refresh is still served.
`tls.autoUpdate: false` opts out. `--update-cert` fetches and exits. `GET /api/cert` reports
validity for the UI banner; `POST /api/cert/refresh` is the manual button.

`tools/run.sh` finds a cert in this order: next to the binary → `~/.acme.sh/…` → self-signed.

Runbook: `docs/LAN_DEPLOYMENT.md`. **Discovery is a won't-do** — nothing advertises itself,
so the address is read off the device and typed in.

### WebSocket Protocol

Two message types:

```json
{"type": "monitoring", "topic": "left-leg", "data": [[1735821000123456, 39, 0, 12345]]}
{"type": "notification", "data": {"event": "short-working-counter", "shortWkcCycles": 3}}
```

**A client receives nothing until it subscribes**, and the two kinds of topic differ in how wide
they are. A monitoring's topic is **narrow**: one monitoring, one topic, and subscribing delivers
that monitoring's batches and nothing else. `"notifications"` is **wide**: it always exists, every
source publishes to it, so one subscription covers every event including kinds added later. That is
why it is one shared topic and not one per source — uWebSockets matches topic names exactly, so a
client could not ask for all of them. To act on only some events, filter on `data.event`. The
TypeScript client's `onNotification` subscribes to it on the first listener.

A monitoring's topic is whatever the client chose, validated by `mm::core::isUrlSafeId`
(1–64 of `[A-Za-z0-9._-]`). **`notifications` is reserved**: `MonitoringManager::create` refuses it,
because it satisfies the same character rule and a monitoring taking it would push sample batches
at every client subscribed for faults.

**`data.event` is kebab-case, and every name is a `constexpr std::string_view` beside the source
that sends it** — `kShortWorkingCounterEvent` in `bus_health_source.h`, matching how procedure names
and step ids are declared. It is protocol: a client switches on it, so it must survive a rewrite of
the message around it. Nothing enforces the case, so a new source is where it drifts. The rest of
the payload is camelCase like every other JSON body this API serves.

`data` is an array of cycle rows, one per recorded cycle since the last flush. A row is
`[timestampUs, v0, v1, ...]`: epoch **microseconds**, then one value per parameter in a fixed
order. No keys in the high-frequency path. A value is `null` while its device is not
exchanging.

Clients fetch the order once and cache it:

```text
GET /api/monitorings/{topic} → { "parameters": [{"devicePosition":1,"index":24676,"subindex":0,"source":"pdo"}] }
```

The order is stable for the lifetime of a monitoring.

**`interval` is the flush cadence, not a sample rate** (bounded 5–2000 ms). A longer interval
ships more rows per message, never fewer cycles. Throughput is roughly constant. Sized for
about 5 simultaneous clients, which is a budget rather than an enforced cap.

**Monitoring is lossless and off-RT.** It is not a `CyclicTask`. `MonitoringManager` owns a
sampler thread; each monitoring holds a read cursor into the recorder ring and ships every
cycle in `[cursor, head)`. A cursor lapped by more than a whole ring is logged and resynced
to the oldest record. PDO parameters are decoded from each record; SDO-only parameters come
from `ParameterRefresher`'s cache, one value per flush.

### Fieldbus Surface

**Bus-level:** AL state control, static configuration (SM/FMMU/DC/mailbox/addresses), process
image and WKC health, `.mmpd` recorder dump, live ESC diagnostics (error counters, link
state, watchdog), DC sync deviation (0x092C).

**Per-device:** FoE, CoE object dictionary and SDO, PDO mapping read and write
(`GET`/`PUT /api/devices/:slavePosition/pdo-mapping`; the write reconfigures 0x1C12/0x1C13
and 0x16xx/0x1Axx in PRE-OP), ESC registers, SII, hardware description, Integro variant, and
firmware compatibility.

Multi-subindex objects are read with one CoE Complete Access upload, probed once per device,
falling back to per-subindex reads. `parameters.useCompleteAccess` (default on) gates it.
**The advertised mailbox capability bytes are a hint, not a gate** — SOMANET advertises
`completeAccess=false` while CA works, so the runtime probe is authoritative. They are exposed
raw on `GET /api/bus-config` and decoded client-side.

**A record's declared size is not on the wire, so the enumeration probes for it.** SOMANET
firmware answers Get Object Description with the *runtime value of subindex 0* rather than the
object's declared size. For a PDO mapping object that is the number of entries currently mapped,
so an unmapped 0x1603 reports itself as having none while its storage holds ten, and walking only
`0..MaxSub` reads a dictionary short by every unused slot — 62 entries on an Integro.
`readObjectDictionary` therefore keeps asking past `MaxSub` until the slave refuses, for records
and arrays only. **Each probe is a single attempt with no retries**, because a refusal is the
answer it wants, and the first refusal that takes longer than 100 ms turns probing off for the
rest of that device: an abort comes back in milliseconds while silence costs a 700 ms mailbox
timeout per object. The same firmware behaviour makes a Complete Access upload of such an object
short, which is expected and falls back per subindex rather than warning. See `NEXTGEN.md`,
Session 2026-08-25.

**A lossy enumeration is never cached.** `readObjectDictionary` returns an `OdRead` — the entries
plus `missingEntries`, the count of subindices the slave was asked about and never answered. A
lost entry does not fail the enumeration, because the device still comes up and every entry that
was read is correct. It does bar the write: a `ParameterCache` file is keyed on vendor, product and
revision alone and is never checked against the device again, so a short dictionary written once is
what that host believes until somebody deletes the file. `Device::enumerateParameters` calls
`store` only when the count is zero. Delete an existing file with
`DELETE /api/parameter-cache/{id}`. See `NEXTGEN.md`, Session 2026-08-25.

Configuration (master-programmed, cached, all slaves) and SII (raw EEPROM of one device, read
live) stay separate. They overlap in category, not in source.

**DC runs in free-run.** `ecx_configdc` elects a reference and disciplines the slaves' clocks,
but `ecx_dcsync0` is deliberately not called, so drives act on frame arrival and the RT loop's
wake jitter is the actuation jitter. SYNC0 activation is deferred and has no committed date. It
is necessary but not sufficient for hard coordinated multi-axis; a PREEMPT_RT host is the other
half.

Out of scope for SOMANET: cable redundancy, and the non-CoE mailbox protocols (EoE, SoE, AoE,
VoE). Deferred work is ranked in `NEXTGEN.md`, Session 2026-06-01 — read it before adding a
fieldbus view.

#### Reading an SDO Failure

The message shape is diagnostic. `SoemFieldbusDriver::readSdo` formats one base line and
appends a suffix only if `ecx_poperror()` has something queued.

- `… failed (SDO abort 0xCCCCCCCC: <reason>)` — the slave **answered** with a CoE abort. A
  definitive refusal in about 6 ms.
- `… failed (no response — mailbox timeout)` — the slave sent nothing. About 703 ms.
- Bare `… failed` — a mailbox-send failure.

**The CoE `wkc` return is a hybrid and already distinguishes these.** Greater than 0 is a real
working counter. `EC_TIMEOUT` (`-5`) is returned uncleared when the slave never answers, so it
is self-describing. `0` is the only ambiguous value: either a send failure, or the slave
answering with an abort, which also enqueues the detail. `sdoErrorSuffix(ctx, wkc)` prefers the
queued reason and decodes the sentinel otherwise.

**This reading is CoE-only.** FoE and EoE overload the same return with a negated
`EC_ERR_TYPE_*`, so a negative FoE return is an error *code*, not a working counter, with
nothing queued. `readFile`/`writeFile` decode it into a `FoeError` via `switch (-wkc)`.

FoE uses `EC_TIMEOUTRXM` (700 ms) per *packet acknowledgement*, not per transfer, so a bare
failure costs about 700 ms per attempt. **A bootloader needing longer is expected to reply
`FOE_BUSY` rather than go silent**, which is why the timeout does not need raising and why
`ports/soem` patches the `FOE_BUSY` branch instead. Upstream 2.0 broke it: the resend went to
the address of a NULL pointer, so a BUSY mid-transfer stalled until timeout, and a BUSY before
the first data packet returned success having sent nothing.

**Practical upshot: probing unknown subindices is expensive, and a bare failure means "no
answer", not "no such object".** See `NEXTGEN.md`, Session 2026-07-20.

### ESI Parsing (`libs/etg`)

`mm::etg` parses a vendor's EtherCAT Slave Information XML and flattens one device into a flat
`std::vector<EsiEntry>`, one row per `(index, subindex)`. It exists because CoE SDO-Information
returns almost no metadata: `unit`, `defaultValue`, `minValue`, and `maxValue` are structurally
supported and always empty, and descriptions and enum labels have no CoE representation at all.

`parseEsi`/`parseEsiFile` produce the fidelity model, `buildDeviceEntries` the flat table, and
`buildEsiResponse` the `POST /api/esi/parse` view — **every device with its own table in one
response; there is no device selector.**

**The library is offline.** A pure transform over text. It deliberately does not depend on
`mm::comm`. `node` may depend on `etg`, never the reverse.

Rules that are easy to get wrong and are pinned by tests:

- **Type structure mirrors the ESI element structure.** Nested where the parent owns the type;
  namespace-scope and unprefixed for vocabulary with several parents; flat and `Esi`-prefixed
  for types in public signatures. The prefix survives on that last group because
  `mm::etg::Device` next to `mm::node::Device` would be a trap.
- **Flag inheritance is not per-flag fallback.** A `<Flags>` element on a `DataType/SubItem`
  shadows the object's block wholesale. A flag the SubItem lacks falls back to the spec
  default, never to the object's value. `SdoAccess` is the carve-out.
- **Classify before pairing.** An ARRAY DataType has two SubItems while its `<Info>` has N+1.
  Pairing positionally before classifying corrupts every array. ARRAY subindices are
  positional and subindex 0 is `USINT`; RECORD subindices come from `<SubIdx>` and pair by
  name.
- **Object-level annotation lives on subindex 0 only.** That row *is* the object. Copying the
  description onto every subindex made one device's JSON 4.7 MB, 83% of it the same HTML
  repeated. Stored once, all four devices cost 3.26 MB instead of 18.1 MB, which is why the
  endpoint can return everything.
- **Values are raw little-endian bytes plus an ETG.1020 code.** Decoding belongs to
  `mm::node::decodeSdoBytes`. A byte-wise comparison is wrong for a signed type, hence
  `EsiEntry::isSigned`, and **padding a short value must be sign-aware**: 0x6086 is an `INT`
  written `MinData=80 MaxData=00`, which zero-filled reads 128…0 (nonsense) and sign-extended
  reads −128…0 (a real range).
- **`<UnitTypes>` is a dictionary-local override** of the ETG.1004 catalogue and wins over the
  built-in table.
- **Slot relocation has four interacting mechanisms**, and `index + slot * increment` is wrong
  on all four. Per-slot attributes override the `<Slots>`-level ones; PDO-area objects use
  `SlotPdoIncrement` and everything else `SlotIndexIncrement`; relocation is gated on
  `Index/@DependOnSlot`, set on nothing in Synapticon ESIs; and `<ModulePdoGroup>` is not a
  relocation at all but an additional aligned PDO the group contributes. Both `rawIndex` and
  `index` are always recorded.
- **Merging is last-wins by default**, over every `ModuleIdent` any slot references. Which
  module is fitted is unknowable without a bus, so the union is the honest answer for an
  offline tool. Collisions are tallied per source pair. `Index/@OverwrittenByModule` outranks
  the policy. Pass `EsiEntryOptions::moduleIdents` to model one concrete configuration.
- **Tolerant, never fatal.** Only three things fail a parse: XML syntax, a non-`EtherCATInfo`
  root, and a missing `Descriptions/Devices`. Everything else is a warning.

`libs/etg/tests/data/somanet-v5.6.6.xml` is a real 1.9 MB Synapticon ESI, reached through the
`MM_ETG_TEST_DATA_DIR` compile definition. It pins the parser against a document nobody shaped
for it.

### ENI (`libs/etg`, `libs/node`)

An **ENI** (EtherCAT Network Information, ETG.2100) is the vendor-neutral configuration a
third-party master replays to bring a bus up. Where an ESI describes one device family, an ENI
describes one assembled network.

**An ENI is a script, not a description, and that decides everything about handling one.** Every
configuration step is an EtherCAT datagram or a CoE download tagged with the AL-state transition it
belongs to. So a viewer must decode that script or show nothing, and a writer must get element order
right, because every ENI complex type is an `xs:sequence`.

Four pieces, split along the dependency rule:

| Where | What |
| --- | --- |
| `libs/etg/eni.{h,cc}` | The model, `writeEni`, and `to_json`. Pure; no `mm::comm`. |
| `libs/etg/eni_reader.{h,cc}` | `readEni`. Tolerant: only bad XML, a wrong root or a missing `Config` fail. |
| `libs/node/eni_collector.{h,cc}` | `collectEni` — reads a live bus into the model. Drives the bus. |
| `libs/node/eni_request.{h,cc}` | `buildEniResponse` — reads a document and annotates each datagram. |

`GET /api/eni` exports, `POST /api/eni/parse` reads. The Console page is **Tools → ENI**.

Rules that are easy to get wrong:

- **The reader is more permissive than the writer, on purpose.** Reading a document *we* wrote is
  nearly worthless. A previous port of `A` is accepted by the reader and refused by the writer,
  because ETG.2100 Table 29 allows it and ENI Schema 1.7 enumerates only `B`, `C` and `D`.
- **`Ccs` 1 is a download.** ETG.2100 Table 20 says the opposite. ETG.1000.6 owns the CoE protocol,
  and every CoE command in ETG's own samples carries a payload under `Ccs` 1, which an upload has
  none of.
- **ETG's four sample documents do not validate against ETG's own schema.** Two omit `AutoIncAddr`
  and `Physics`; all four give one of `InputOffs`/`OutputOffs` where both are required. They are a
  reader fixture, never a reference for the writer, which emits the schema-valid superset.
- **`CycleTime1` is not the SYNC1 cycle time.** ETG.2100 Table 32 makes it `SYNC1 cycle − SYNC0
  cycle + SYNC0 shift`, which can be negative. The DC times are signed for that reason.
- **`ProcessData/RxPdo` and `TxPdo` are the only place a mapped value's object address appears.**
  The process image carries a name, a size and an offset and nothing else, so a document without the
  PDO declarations cannot tell a reader that the value at bit 16 is `0x607A:00`.
- **Export needs the bus mapped.** FMMUs and logical addresses come into being at the SAFE-OP
  transition. PRE-OP answers 409 rather than being guessed at.
- **The cyclic frame is an LWR plus an LRD, never one LRW.** A read-write datagram is only correct
  where a device's two FMMUs share a logical address, and this master lays them out disjointly.
- **A Sync Manager's `type` does not survive `decodeSyncManager`.** What a channel carries is not in
  the register; it is the master's classification from the SII. The decode returns zero rather than
  inventing one.
- **The collector never fills `DC`, so an export is free-run.** The element is read and written
  correctly. Producing one needs an `AssignActivate` word, which a SOMANET drive does not carry in
  its SII.

**The ETG schemas and sample documents are not in the repository.** ETG's download terms forbid
redistributing them and this repository is public. `MM_ENI_SCHEMA`, `MM_ESI_SCHEMA` and
`MM_ENI_SAMPLES_DIR` (see `cmake/schema_validation.cmake`) name them, each defaulting to a
gitignored directory. Drop a copy in and the tests turn themselves on; leave it out and they are
skipped with the path they wanted. xmllint validates every generated document, because pugixml
cannot.

Rationale: `NEXTGEN.md`, Session 2026-09-02.

### Generated Object Addresses

Three headers carry one `ObjectAddress<T>` constant per SOMANET dictionary entry:
`profile_device_objects.h` (0x1xxx and standard MDP), `cia402_drive_objects.h` (0x6xxx),
`somanet_drive_objects.h` (0x2xxx and FSoE).

Generated from the pinned `libs/etg/tests/data/somanet-v5.6.6.xml`:

```bash
motion-master generate-object-addresses --esi <file> --out libs/node
./tools/format.sh   # required — clang-format wraps declarations over 100 columns
```

There is deliberately **no CI drift check**. The ESI is pinned and regenerated on request, so a
check would fail exactly when the lag is intentional. Regenerate only when asked.

**One header per index range rests on a vendor convention.** The generator merges every device
into one table keyed by `(index, subindex)`, which is sound only if an address names the same
quantity with the same type and unit on every device. The standards guarantee that for 0x1xxx
and 0x6xxx. **Nothing guarantees it for the manufacturer-specific 0x2xxx range** — it holds
because SOMANET devices draw the bulk of their dictionary from one shared ESI module, so most
of the union is the same text merged with itself. The generator warns when two devices declare
different types for one address and keeps the first. What it cannot see is one index reused for
a different quantity of the same type. If that appears, the family needs a header per device.

## Dependencies

Managed by vcpkg (`extern/vcpkg` submodule, pinned in `vcpkg.json`). To add one: edit
`vcpkg.json`, then `find_package` plus `target_link_libraries` in the relevant
`CMakeLists.txt`.

| Package | Version | Used in | CMake target |
| --- | --- | --- | --- |
| `cli11` | 2.6.2 | `motion_master` | `CLI11::CLI11` |
| `gtest` | 1.17.0 | test targets | `GTest::gtest`, `GTest::gtest_main` |
| `neargye-semver` | 1.0.0-rc | `mm_core` | `semver::semver` |
| `nlohmann-json` | 3.12.0 | `motion_master` | `nlohmann_json::nlohmann_json` |
| `pugixml` | 1.15 | `mm_etg` | `pugixml::pugixml` |
| `spdlog` | 1.17.0 | `motion_master` | `spdlog::spdlog` |
| `uwebsockets` | 20.77.0 | `motion_master` | `unofficial::uwebsockets::uwebsockets` |

**Never commit a private key or certificate** (`*.key`, `*.pem`).

## Testing

Tests live in a `tests/` subdirectory beside their library. CTest discovers the binary via
`gtest_discover_tests`.

```bash
./tools/test.sh
ctest --test-dir build/x64-linux-debug -R VersionTest
./tools/tsan.sh                       # the concurrency tests under ThreadSanitizer
```

**`libs/node/tests/concurrency_test.cc` is the only machine check on the locking design.** It drives
one `DeviceManager` from several threads — one standing in for the RT loop, the others as HTTP
workers, samplers and a procedure. Run it under `tools/tsan.sh` after any change to `DeviceManager`,
`Device`, `ProcessData`, the game loop or a lock, and widen it to cover what you changed. The
`x64-linux-tsan` preset uses clang, because Fedora ships clang's TSan runtime and GCC's needs a
separate `libtsan` package. `tsan.supp` holds one documented suppression, for the recorder ring's
sequence lock; read it before adding to it.

## Hardware-in-the-Loop Tests

`hil/` holds standalone binaries that run on a pre-configured RT machine. They are not CTest
unit tests and need elevated privileges.

### jitter_bench

Measures how far each actual cycle interval deviates from the target period. Runs the same
`CyclicTimer` loop `GameLoop` uses, with `SCHED_FIFO` 80 and `mlockall`.

```bash
sudo ./build/x64-linux-debug/hil/jitter_bench/jitter_bench [options]
#   --duration <s>   run duration            (default: 30)
#   --period <µs>    cycle period            (default: 1000)
#   --workload <µs>  per-cycle busy-wait     (default: 0)
#   --output <file>  CSV path                (default: jitter.csv)

python3 hil/jitter_bench/plot_jitter.py jitter.csv -o report.png
```

`--workload` simulates task load with a CPU-bound spin, so you can test whether a realistic
budget causes overruns on a given kernel. The CSV has `cycle`, `elapsed_ms`, `jitter_ns`.

### api

TypeScript integration tests (Vitest) for the HTTP API and the WebSocket. They drive the
published client library against a real server, so one run exercises the binary, the contract,
and the client together. The global setup manages the Docker lifecycle.

```bash
pnpm install                                 # repo root, first time only
pnpm --filter motion-master-api-tests test
```

The image is built from the repo root and run with `--network host`, which is required because
the server binds to `127.0.0.1`. Set `MM_SKIP_DOCKER=1` to test against an already-running
instance.

## Code Style

Enforced by `.clang-format`: Google layout, 100 columns. Run `./tools/format.sh` or
`ninja -C build/<preset> format`.

Headers use `.h`, sources use `.cc`. Always `#pragma once`, never include guards. Always brace
an `if`, even a single statement.

### Naming Conventions

| Category | Convention | Examples |
| --- | --- | --- |
| Classes, structs, enums, aliases | `PascalCase` | `NetworkAdapter`, `GameLoop` |
| Functions, free and member | `camelCase` | `isMacAddress()`, `addTask()` |
| Variables, parameters, members | `camelCase` | `macLinux`, `adapterName` |
| Private data members | `camelCase_` | `period_`, `running_`, `tasks_` |
| Files | `snake_case` | `game_loop.cc` |
| Namespaces | `snake_case` | `mm::comm`, `mm::core` |
| Macros | `SCREAMING_SNAKE_CASE` | `MAX_RETRY_COUNT` |

Repo and folder names use hyphens. Naming is enforced in review; no tool checks it.

**SOEM and SPoE casing:** uppercase in prose, `Soem` PascalCase in type names, lowercase in
config tokens, filenames, and namespaces.

**Class names encode the base only for polymorphic interface implementations.** A class held
and passed as its abstract base — via `Base&`, `unique_ptr<Base>`, or `vector<Base*>` — embeds
the **full** interface name: `SoemFieldbusDriver`, `ProcessDataCyclicTask`. A **specialization
chain**, refined by is-a and used as its own concrete type, names the concept with no ancestry:
`ProfileDevice ← Cia402Drive ← SomanetDrive`. Standalone classes just name the concept. Test
doubles take a `Fake` prefix.

> Deciding test: held via its base pointer and swappable with siblings → embed the base name.
> A refinement used as itself → name the concept.

Instance names mirror the class in `camelCase`, abbreviating only where conventional
(`WebSocketServer wsServer`).

**Accessors are bare nouns; mutators take `set`.** `period()` / `setPeriod()`, never `get*`.
This holds even when the pair wraps bus I/O, because the `set` prefix is what makes a hardware
write read as an action. Same-name get/set overloads are **not** used — `guardTime(100)` hiding
a flash write is the failure this rule prevents, and overload sets break `&Class::name` in
callbacks.

Raw or parameterised bus transfers use `read*`/`write*` (`readSdo`, `writeValue`, `readFile`).
Operations stay verbs (`scan()`, `enable()`, `transitionToState()`).

One sanctioned exception, not a template: `DeviceParameter::getValue<T>()`, because bare
`value()` would collide with the data member. `HttpServer::Config`'s `std::function` members are
named as actions matching their HTTP verb, because a callback field is an action, not a
property.

## Static Analysis

`lint`, `cppcheck`, `tidy`, and `format` are CMake targets in `cmake/lint.cmake`.

```bash
./tools/cppcheck.sh    # or: ninja -C build/<preset> cppcheck
./tools/lint.sh
./tools/tidy.sh
```

cpplint is configured by `CPPLINT.cfg`. cppcheck runs `warning,style,performance,portability`
with `--std=c++23` and exits non-zero on findings.

**clang-tidy runs only the bug-finding checks** — `bugprone-*`, `clang-analyzer-*`,
`concurrency-*`, `performance-*`. No style families, because `.clang-format` settles layout,
`CPPLINT.cfg` settles includes and naming, and `-Werror` settles the rest. What it adds is
path-sensitive analysis none of them can do. `misc-*` is off on measurement: over five files it
produced 497 of 560 findings from three checks, none naming a defect. Further exclusions are
named in `.clang-tidy` with the reason beside each.

`WarningsAsErrors` is `'*'`. **A finding is fixed, or the check is argued off the list. Never
silently suppressed.** Ignore files are for third-party code only.

## Gotchas

Traps that have already cost time. Each is load-bearing.

- **Suppress with `// NOLINTNEXTLINE(check)` on its own line, never `NOLINTBEGIN`/`NOLINTEND`.**
  Two reasons. The marker must be the **last** comment line before the code, because
  clang-tidy applies it to the very next line — prose written after it pushes the code out of
  range and the suppression silently does nothing. And cpplint parses `NOLINT` markers too,
  rejecting a `NOLINTEND` whose category it does not recognise, so a clang-tidy check name can
  never appear in the block form.
- **`CMAKE_CXX_SCAN_FOR_MODULES` is `OFF` in the root `CMakeLists.txt`.** With GCC the scan
  injects `-fmodules-ts` and friends into `compile_commands.json`, where clang-tidy rejects the
  translation unit outright. `CMAKE_EXPORT_COMPILE_COMMANDS` is `ON` in the base preset. Both
  are easy to undo by accident.
- **A `std::vector` of atomics cannot be reallocated or `shrink_to_fit`'d.** libstdc++ hides
  this from a local Linux build; libc++ and MSVC do not.
- **A queued CoE emergency fails the next SDO or FoE.** The signature is an operation that
  always succeeds on the second attempt.
- **Never assume a vendor object's type or name.** Read the live object dictionary or the ESI.
- **`tools/code-stats.sh` embeds a single-quoted `awk` program**, so an apostrophe inside those
  comments terminates the string. SC1112 there is real, not a nit.
- **Two shellcheck suppressions are deliberate and should stay**: SC2034 in `rt/vm/common.sh`
  (a sourced config file) and SC1112 source=/dev/null in `clients/python/setup.sh`.
- **The running server instance is shared with the user's Console session.** Ask before
  mutating its state.
