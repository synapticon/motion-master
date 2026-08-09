# Locking and Synchronization

> **Hand-maintained.** Derived by reading the source, not generated. Every claim carries a
> `file:line` citation so it can be re-checked cheaply. When you add, remove, or re-scope a
> synchronization primitive, update this file in the same commit.
>
> Companion documents: [THREADS.md](THREADS.md) (which threads exist and what each does),
> [CLASS_DIAGRAM.md](CLASS_DIAGRAM.md) (ownership), [RT_SCHEDULING.md](RT_SCHEDULING.md)
> (`SCHED_FIFO` / `mlockall`).

Motion Master has **nine mutexes, two condition variables, and eight lock-free protocols**. The
lock-free protocols are documented here alongside the mutexes on purpose: in this codebase they are
not an optimisation applied to a mutex-protected design, they *are* the design on the paths that
matter (the RT cycle, the recorder ring, the AL-state mirror), and reasoning about a mutex without
them is reasoning about half the machine.

## Contents

- [The one-page summary](#the-one-page-summary)
- [Who runs concurrently](#who-runs-concurrently)
- [Mutex inventory](#mutex-inventory)
- [Lock ordering](#lock-ordering)
- [The mutexes in detail](#the-mutexes-in-detail)
- [Lock-free protocols](#lock-free-protocols)
- [Invariants](#invariants)
- [Review findings](#review-findings)
- [Checklist when adding a lock](#checklist-when-adding-a-lock)

## The one-page summary

Five rules carry almost all of the correctness:

1. **No lock-protected state is reachable without the lock.** `DeviceManager` hands out no
   reference or pointer into `devices_`; `Device` hands out none into `parameters_`. You borrow
   (`withDevice` / `withDevices`, which hold the lock for the callable's whole duration) or you get
   a copy (`parameter`, `parametersOrdered`, `value`). The raw-pointer lookups still exist —
   `DeviceManager::findDevice`, `Device::findParameter` — but both are **private**, so the rule is
   enforced by the compiler rather than by a comment that a later change can quietly invalidate.
   This is the rule the other four rest on: it is what makes "which lock does this need?" a question
   with an answer at the call site.
2. **The RT thread takes no lock, ever.** `DeviceManager::exchangeProcessData`
   (`libs/node/device_manager.cc:323`) is gated by an atomic image pointer, not a mutex. Everything
   it touches — the IOmap, the output staging slots, the recorder ring — is lock-free by
   construction.
3. **`FieldbusDriver::controlPlaneMutex_` is per *transaction*; `DeviceManager::busOperationMutex_`
   is per *operation*.** The first serialises one socket round-trip, the second serialises a whole
   multi-transaction activity (a scan, an AL transition, a re-map). They are not tiers of the same
   thing and a caller often holds only one.
4. **`deviceSetMutex_` guards lifetime, not data.** Shared means "the `Device` objects and the
   process-data runtime will not be freed while I hold this". It is legitimate to hold it shared for
   minutes, because its exclusive holders are rare and brief.
5. **No lock is held across bus I/O except `controlPlaneMutex_` itself**, and that one is held for
   exactly one transfer. `Device::parametersMutex_` in particular is taken, released for the
   transfer, and re-taken to commit (`libs/node/device.h:658-670`).

## Who runs concurrently

The thread inventory matters more than usual here, because several comments in the tree were written
against an older, smaller one.

| Thread | Count | Created | Notes |
| --- | --- | --- | --- |
| RT game loop | 1 | the main thread becomes it, `main.cc:330` | takes no lock |
| HTTP event loop | 1 | `http_server.cc` | dispatch and response writes only; never runs a handler |
| **HTTP worker pool** | **32** | `BS::light_thread_pool pool_{32}` (`apps/motion_master/http_server.h:203`) | **every route handler runs here** (`libs/api/router.cc`, `pool->detach_task`) |
| WebSocket event loop | 1 | `ws_server.cc:26` | |
| Monitoring sampler | 1 | `monitoring_manager.cc:171` | |
| Parameter refresher | 1 | `parameter_refresher.cc:87` | |
| Procedure runs | 0..N | `std::jthread`, `procedure_manager.cc:128` | one per in-flight procedure |

**The consequence worth internalising: "the HTTP thread" no longer exists.** Since route handlers
moved onto `pool_`, two REST requests genuinely run in parallel. Any invariant of the form "both
callers are on the HTTP thread, so they are serialised" is false today. Several such comments remain
in the tree and are listed under [Review findings](#review-findings).

## Mutex inventory

| # | Primitive | Type | Declared | Guards | Taken by | Held across bus I/O? |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | `DeviceManager::busOperationMutex_` | `std::mutex` | `libs/node/device_manager.h:772` | nothing — a token over an *activity* | `init`, `scan`, `reset`, `configureProcessData`, `transitionToState`, `writeDevicePdoMapping` | **yes**, for the whole operation (seconds) |
| 2 | `DeviceManager::deviceSetMutex_` | `std::shared_mutex` | `libs/node/device_manager.h:784` | `devices_`, `driver_`, `pd_->generations`, `pd_->outputSlots`, `pd_->ring` storage | exclusive: `init`/`scan`/`reset` + the re-map publish window. shared: every position-based read/write, `withDevice`, the recorder accessors, `serializeDump` | shared: **yes** (a whole procedure). exclusive: no |
| 3 | `Device::parametersMutex_` | `std::mutex` (via `unique_ptr`) | `libs/node/device.h:675` | `parameters_`, `caSupport_` | every typed parameter read/write | **no** — released for every transfer |
| 4 | `FieldbusDriver::controlPlaneMutex_` | `std::mutex` | `libs/comm/fieldbus_driver.h:818` | SOEM `ctx_` and the whole control plane (SDO, FoE, ESC registers, SII, AL state) | every driver method except `exchangeProcessData` and `slaveState` | **yes**, one transaction — except FoE, which holds it for the whole transfer |
| 5 | `MonitoringManager::mutex_` (+ `cv_`) | `std::mutex`, `condition_variable` | `libs/node/monitoring_manager.h:200-201` | `entries_`, `running_`, `nextEpoch_`, `publish_` | sampler thread + the `/api/monitorings` handlers | **no** — dropped for the flush |
| 6 | `ParameterRefresher::mutex_` (+ `cv_`) | `std::mutex`, `condition_variable` | `libs/node/parameter_refresher.h:94-95` | `entries_`, `running_` | refresher thread + `acquire`/`release` from the sampler path | **no** — dropped for the poll |
| 7 | `ProcedureManager::mutex_` | `std::mutex` | `libs/node/procedure_manager.h:257` | `runs_` | `start`/`snapshot`/`cancel`/destructor | no |
| 8 | `ProcedureManager::Run::errorMutex_` | `std::mutex` | `libs/node/procedure_manager.h:224` | one `std::optional<std::string>` | the run's own thread (write), pollers (read) | no |
| 9 | `ProgressReporter::mutex_` | `std::mutex` | `libs/node/procedure.h:360` | the step array | the run's own thread (write), pollers (read) | no |
| — | `RingLogSink` (spdlog `base_sink::mutex_`) | `std::mutex` | `apps/motion_master/ring_log_sink.h` | the log ring buffer | every logging thread | no |
| — | single-instance lock | `flock` / named mutex | `libs/core/platform.cc:125,143` | the *process*, not a data structure | startup only | n/a |

Entries 8 and 9 exist for one reason worth stating: **a finishing procedure thread must never need
`ProcedureManager::mutex_`**, because the destructor collects the running threads under that lock and
joins them after releasing it (`procedure_manager.cc:24-38`). Anything the thread writes on its way
out is therefore either an atomic or has a per-run mutex of its own.

## Lock ordering

```text
busOperationMutex_
      ↓
deviceSetMutex_
      ↓
Device::parametersMutex_
      ↓
FieldbusDriver::controlPlaneMutex_
```

Declared at `libs/node/device_manager.h:782-783` and restated at `libs/node/device.h:670`.

The three subsystem mutexes (5, 6, 7) sit **above** this chain — `MonitoringManager::mutex_` is
released before any `DeviceManager` call (`monitoring_manager.cc:219`), `ParameterRefresher::mutex_`
likewise (`parameter_refresher.cc:143`), and `ProcedureManager` resolves the device *before* taking
its own lock (`procedure_manager.cc:95-103`). None of them is ever held while a `DeviceManager` lock
is acquired, so they cannot participate in a cycle. Keeping that true is the single most important
rule for anyone adding to those classes.

`std::shared_mutex` has **no upgrade path**, and one consequence is load-bearing rather than
incidental: a `ProcedureBody` runs inside `withDevice` holding `deviceSetMutex_` shared, so it cannot
call `transitionToState` — which is precisely why `BusProcedureBody` exists as a second body shape
(`libs/node/procedure_manager.h:41-62`).

## The mutexes in detail

### 1. `DeviceManager::busOperationMutex_` — one control-plane operation at a time

*Type:* plain `std::mutex`. *Guards:* no member at all.

This is a mutual-exclusion token over an **activity**: "only one thing drives the bus through a
multi-step operation at a time". It is what distinguishes a whole `scan` from the individual socket
transactions inside it, which `controlPlaneMutex_` serialises separately.

**Why it is separate from `deviceSetMutex_`.** A single lock doing both jobs would be both
long-held-shared (a procedure) and contended-exclusive (an AL transition) at once — the exact state
in which `std::shared_mutex` contention behaviour decides the outcome. That behaviour is
unspecified by the standard and *opposite* across the supported platforms: glibc's
`pthread_rwlock_t` is reader-preferring and can starve a writer indefinitely, while Windows
`SRWLOCK` is documented unfair and can instead convoy readers behind a pending writer
(`libs/node/device_manager.h:755-760`). Splitting removes the situation rather than betting on a
platform.

**The payoff is concrete:** `transitionToState` (`device_manager.cc:714`) takes *only* this lock, so
its multi-second AL wait blocks no reader — the monitoring sampler keeps flushing and every
`/api/monitorings` endpoint keeps answering throughout. Likewise `configureProcessData`
(`device_manager.cc:243`) does its IOmap rebuild and per-device PDO-mapping SDO reads under this lock
alone, and takes `deviceSetMutex_` exclusively only for the brief publish window
(`device_manager.cc:302-317`).

**Because only a holder of this lock can rebuild `devices_`/`driver_`, holding it is by itself
sufficient to keep them stable.** That is why `remapProcessImage`, `updateExpectedWkc` and the
`transitionToState` body read `devices_` without `deviceSetMutex_`.

**The trade it accepts, and it is deliberate:** a borrow does **not** exclude an AL transition. A long
procedure and a user-driven state change interleave; the procedure's next bus transaction fails
against the changed state rather than the transition's HTTP request hanging for the procedure's whole
duration (`libs/node/device_manager.h:278-283`).

### 2. `DeviceManager::deviceSetMutex_` — lifetime, not data

*Type:* `std::shared_mutex`.

Shared means: **`devices_` and the process-data runtime are not being rebuilt or freed while I hold
this.** That is the whole contract. It says nothing about the *contents* of a `Device` (see
`parametersMutex_`) and nothing about what state the bus is in.

Exclusive holders are few and brief by design:

- `init` / `scan` / `reset` (`device_manager.cc:119,157,200`) — these genuinely invalidate every
  `Device&` in flight;
- the publish window inside `remapProcessImage` (`device_manager.cc:302-317`) — slot-vector swap,
  ring re-allocation, generation append.

Everything else takes it shared, including `withDevice` and `withDevices`, which are the **only**
ways to reach a `Device` from outside the class. Holding it shared for a multi-second procedure is
*correct*, not merely tolerated: a rescan mid-operation is exactly what would dangle the reference.

`findDevice` is private for that reason. Its returned pointer is valid only while the lock is held,
and that obligation cannot be expressed in the signature — so the pointer never leaves the class,
and a caller who wants a device either borrows it (lock held for the borrow's whole duration) or
calls a position-based method that takes the lock itself.

**The recorder accessors take it for a reason that is easy to get wrong.** `recorderHead`,
`recorderOldestSeq` and `readRecord` (`device_manager.cc:412-429`) take it shared even though
`ProcessDataRing` is lock-free. The adversary is *not* the RT producer — the ring's per-slot sequence
re-check handles that. It is `allocate`/`clear`, which **release the storage** and run from the
control plane. Reading the ring without this lock is a use-after-free, not a torn read
(`libs/node/process_data_ring.h:35-41`) — and the window is wide, since a sampler flush walks
thousands of records.

`readRecord` takes the lock **per record rather than per span** (`device_manager.cc:427`) so a
sampler flush walking thousands of records never becomes a milliseconds-long shared holder that an
exclusive `scan` queues behind. A span was never atomic anyway — each record self-describes and a
lapped one returns `false`.

### 3. `Device::parametersMutex_` — the parameter map, never across a transfer

*Type:* `std::mutex`, held by `unique_ptr` only because `Device` must stay move-constructible for
`std::vector<Device>` (`libs/node/device.h:672-674`).

**The rule that matters: it is never held across bus I/O.** Every method that transfers
(`readParameter`, `writeParameter`, `readObjectComplete`, `readAllParameters`,
`initializeParameters`) takes it, decides what to transfer, **releases it**, transfers, and re-takes
it to commit.

That is not a micro-optimisation. A mailbox transfer queues behind `controlPlaneMutex_`, which an FoE
file transfer holds for its whole multi-second duration — so a lock held across "one mailbox
round-trip" is in fact held for as long as any unrelated bus traffic takes. With a background
parameter refresh and a user on the FoE page — the ordinary configuration, not a rare one — that
would stall every cached read of the device (`libs/node/device.h:658-666`).

**The obligation this imposes on callers:** a `DeviceParameter*` must never be carried across the
release, because `initializeParameters` replaces the whole map. Re-find after the transfer and treat
a miss — or a changed `dataType` — as "re-enumerated mid-transfer" rather than assuming it cannot
happen.

**Nothing hands out a pointer into the map.** `parameter()` returns a copy of one entry,
`parametersOrdered()` a snapshot of all of them, `value()` / `dataType()` a single field —
each taken under the lock. The raw lookup survives as the private `findParameter()`, whose contract
is "the caller already holds `parametersMutex_`". That is enforced by access control rather than by
a comment, which matters because the previous public raw-pointer accessor was documented
"control-plane thread only" and the control plane silently became 32 threads.

### 4. `FieldbusDriver::controlPlaneMutex_` — one socket transaction

*Type:* `std::mutex`, `mutable` so `const` accessors can lock.

Serialises the entire control plane: SDO, FoE, ESC registers, SII, AL state. **The PDO path
deliberately does not take it** — `SoemFieldbusDriver::exchangeProcessData`
(`soem_fieldbus_driver.cc:497`) runs lock-free, because SOEM's port layer is internally thread-safe
and PDO touches disjoint state (the IOmap) from the control plane. That is the single decision that
keeps a slow SDO from ever stalling the 1 ms cycle.

Held for one transaction, never across a sleep, a blocking wait, or a user callback. Two multi-second
operations demonstrate the discipline:

- `readObjectDictionary` (`soem_fieldbus_driver.cc:934`) takes it per SDO-Info transaction and
  releases it across `retrySdoInfo`'s back-off sleeps.
- `transitionToState` (`soem_fieldbus_driver.cc:1427`) takes it around each discrete socket
  transaction, never across the poll sleep or the `tick()`/`shouldAbort()` callbacks.

**The accepted caveat, and it is uniform rather than specific to those two:** dropping the lock
between transactions means `ctx_` is not held stable for the operation's duration. A concurrent
`scan`/`reset`/`stop` landing in a gap frees `ctx_` and the next transaction dereferences a dangling
context. Within Motion Master this cannot happen — the callers exclude a rescan for the whole call
(`initializeDeviceParameters` holds `deviceSetMutex_` shared, `transitionToState` holds
`busOperationMutex_`, and `scan`/`reset` need both). **An embedder driving `SoemFieldbusDriver`
directly must provide the same guarantee** (`soem_fieldbus_driver.cc:941-955`).

**FoE is the one long hold.** `readFile`/`writeFile` hold the lock for the entire transfer, which is
why the AL-state mirror exists (see below): without it, every reader asking "what state is device 3
in?" would wait out a 12-second firmware write.

### 5–6. `MonitoringManager::mutex_` and `ParameterRefresher::mutex_`

Both follow the identical shape, for the identical reason: **snapshot under the lock, work with the
lock released, commit back under the lock.**

`MonitoringManager` calls it `takeDue` → `flushDetached` → `commitFlush`
(`monitoring_manager.cc:211-227`); `ParameterRefresher::pollDue` does the same with a key list
(`parameter_refresher.cc:109-158`). Doing the flush under the lock meant a control-plane operation
holding the bus lock stalled not just the sampler but every `/api/monitorings` endpoint
(`libs/node/monitoring_manager.h:152-160`).

The subtle part is **`commitFlush`'s epoch check** (`monitoring_manager.cc:248-255`). Releasing the
lock lets a `remove` + re-create of the same topic land mid-flush; writing the old flush's cursor
onto the new registration would skip it past cycles it never delivered. The epoch — stamped from
`nextEpoch_` at create — is what makes releasing safe. `ParameterRefresher` gets the same property
more cheaply by re-finding the entry by key after the poll.

### 7–9. Procedure locks

`ProcedureManager::mutex_` guards only `runs_`. The design constraint driving everything else is that
**a finishing run must not need it**: the destructor collects joinable threads under the lock and
joins them after releasing it, so a thread that needed the lock on its way out would deadlock against
its own collection (`procedure_manager.cc:24-38`, and the rationale at
`libs/node/procedure_manager.h:216-225`). Hence `status`, `finishedAt` and `running` are atomics, and
the one field that cannot be — the error string — gets its own per-run, uncontended mutex.
`libc++` implements neither `std::atomic<std::shared_ptr<T>>` nor an atomic string, so this is a
portability fact and not a preference.

`discardIfRescanned` (`procedure_manager.cc:40-54`) is subtle in one way worth flagging: it **never
drops a still-running entry**, and that is a correctness requirement. A running thread holds a
`shared_ptr` to its own `Run`, which owns the `std::jthread` it is executing on; releasing the last
*other* reference would have the thread destroy its own jthread as it exits and self-join. This is
reachable only because a `BusProcedureBody` holds no bus lock between steps, so a `scan()` can land
mid-run.

## Lock-free protocols

These carry the RT path. Each is a *protocol*, not just an atomic — the ordering is the mechanism.

### The published-image gate

`ProcessData::image` (`libs/node/process_data.h:39`) is the **only** thing standing between the RT
loop and a control-plane teardown. `exchangeProcessData` raises the `exchanging` flag **before**
loading the image, then re-reads it; `stopExchange` stores `nullptr` **then** waits on the flag. Both
sides use `seq_cst` on that pair specifically because it is a StoreLoad, which nothing weaker
prevents from reordering (`device_manager.cc:332-344` and `:388-401`). The resulting total order
guarantees that for any concurrent teardown, *either* the RT cycle observes the null image and backs
out, *or* `stopExchange` observes the flag and drains it — never both missing each other.

Note what is **not** checked: `driver_`. Testing it on the RT thread would be an unsynchronised read
of a `unique_ptr` that `init`/`reset` write under a lock the RT thread never takes. A non-null image
already implies a live driver, established by ordering rather than by a check the RT thread has no
safe way to make.

Published images are **retained** in `generations` until `reset()`, so a lock-free reader can never
dereference a freed image after a re-map republishes.

### The recorder ring

`ProcessDataRing` (`libs/node/process_data_ring.{h,cc}`) is single-writer / many-reader. The RT
`write()` is wait-free: invalidate the slot's sequence word, `memcpy` the payload, release-store the
real sequence, release-store the new head. A reader checks the sequence, copies, then **re-checks it
after an acquire fence** — which detects a producer that lapped it mid-copy
(`process_data_ring.cc:113-143`). A lapped reader is a normal, detected outcome, not a bug: it
returns `false` and the caller resyncs to `oldestValidSeq()`.

**The guarantee is scoped to `write()` and that scoping is the whole point.** `allocate` and `clear`
are a third kind of writer — they release the storage, so no reader-side sequence check can make them
safe. The owner must exclude readers around them, which is what `deviceSetMutex_` does.

### Output staging (NEXTGEN "Design B")

`ProcessData::outputSlots` (`process_data.h:49`) is one `std::atomic<uint64_t>` per output object.
Any number of non-RT writers store into *different* objects' slots without contending; the same
object is last-writer-wins. **The RT loop is the sole thread that composes those slots into the
packed wire image each cycle** — and that single-composer property is what makes bit-packed objects
sharing a byte safe without a lock. A `static_assert` pins the slot to be always-lock-free
(`process_data.h:50-51`).

Consequence worth knowing: a batch stage (`stageProcessDataOutputs`) writes slots sequentially on a
non-RT thread, so a batch can straddle two consecutive RT cycles — ≤1 ms skew, documented and
accepted (`device_manager.h:621-624`).

### The AL-state mirror

`SoemFieldbusDriver::slaveStates_` (`soem_fieldbus_driver.h:259`) is a lock-free array of
`std::atomic<uint16_t>`, published by `publishSlaveStates()` at the end of every locked scope that
could have changed a cached AL status. It exists so `slaveState()` can honour the **"must not
block"** contract on `FieldbusDriver::slaveState` (`fieldbus_driver.h:379-385`): the real value lives
in SOEM's slavelist, reachable only under `controlPlaneMutex_`, which a firmware transfer holds for
its whole duration — and `slaveState` is asked once per device per monitoring flush.

Relaxed ordering is correct rather than a shortcut: each entry is self-contained with no companion
state to order against, and a reader is by definition unsynchronised with the transition publishing
it. Allocated once at `EC_MAXSLAVE` and never reallocated, so a reader may index it for the driver's
whole lifetime — which is also why it is not a `vector<atomic>`, whose reallocation paths are
ill-formed on libc++ and MSVC.

### The remaining atomics

| Atomic | Where | Pattern |
| --- | --- | --- |
| `GameLoop::period_` | `game_loop.h:155` | many writers, single (RT) reader; reloaded at the top of each iteration |
| `GameLoop::running_` | `game_loop.h:156` | `stop()` is async-signal-safe — `static_assert`ed lock-free at `game_loop.cc:119` |
| `GameLoop` counters | `game_loop.h:157-165` | single RT writer, relaxed; diagnostics only, never synchronisation |
| `topologyGeneration_` / `processImageGeneration_` | `device_manager.h:786,788` | version counters an off-thread consumer compares to know its captured state is stale |
| `lastWkc` / `expectedWkc` | `process_data.h:65-66` | RT writes the first, control plane the second |
| `HttpServer`/`WebSocketServer` `loop_`, `app_`, `running_` | `http_server.h:157-160`, `ws_server.h` | cross-thread `defer()` targets |
| `Router::stopping_` | `http_server.h:166` | set on the loop thread, read on the loop thread — serialised by the thread, not by the atomic. It closes the window where a request arriving *during* the pool drain would dispatch a fresh worker that then defers onto a dead loop |
| `Run::status`/`finishedAt`/`running` | `procedure_manager.h:196-198` | written last-to-first so a poller that sees "finished" also sees the outcome |

## Invariants

Things that must stay true. Each is currently relied upon somewhere.

1. **The RT thread acquires no mutex.** Verify by inspection of `exchangeProcessData` and everything
   it calls.
2. **No subsystem mutex (monitoring, refresher, procedure) is held while a `DeviceManager` method is
   called.** This is what keeps the lock graph acyclic.
3. **`parametersMutex_` is not held across any bus transfer.**
4. **A `DeviceParameter*` never crosses a `parametersMutex_` release.**
5. **`deviceSetMutex_` is not recursive**, and `withDevice`'s callable must not re-enter `scan`,
   `reset`, `init`, `configureProcessData` or `transitionToState`.
6. **Every reader of `pd_->ring` holds `deviceSetMutex_` shared.**
7. **`stopExchange()` precedes every mutation of the IOmap, the ring storage, or `driver_`.**
8. **Published `ProcessImage` generations are retained until `reset()`.**

## How these rules are checked

**By review, and by nothing else. Know that before trusting a green test run.**

The test suite is ~95% single-threaded, so a locking mistake does not fail it. That is not an
oversight to route around casually — a race detector was tried and deliberately dropped, because the
coverage it could actually give was narrow (it sees only what runs concurrently, which here is the
`DeviceManager` read surfaces and `ProcedureManager`'s run lifecycle) and it left the parts that
matter most untouched: nothing anywhere exercises `exchangeProcessData` against a concurrent re-map,
or `busOperationMutex_` against anything, or the real driver's `controlPlaneMutex_`.

So the practical rules for a change on this page are:

- Read the [invariants](#invariants) above and say which one your change relies on.
- Prefer a design where the mistake cannot compile — private raw accessors, borrow-or-copy — over one
  that merely documents the obligation. That is what rule 1 buys, and it is the only enforcement here
  that does not depend on someone remembering.
- Treat "the tests pass" as evidence about behaviour, not about synchronisation.

## Review findings

Ranked by severity, and **all fixed** — the record is kept because the reasoning is what stops them
coming back. Line numbers are from before the fix.

### F1 — `findDevice` hands a raw `Device*` to 32 concurrent workers

**Severity: high. Use-after-free.**

`DeviceManager::findDevice` (`device_manager.cc:224,231`), `devices()` (`:222`) and `busConfig()`
(`:639`) take **no lock**. The header is explicit about it — "Not internally synchronised: call on
the control-plane (server) thread" (`device_manager.h:256-260`) — and that warning was accurate when
the HTTP server was a single event-loop thread.

It no longer is. Since route handlers moved onto `pool_` (`http_server.h:203`, dispatched at
`router.cc`'s `pool->detach_task`), roughly 25 handlers call `deviceManager_.findDevice(pos)` and
then do bus I/O through the returned pointer — `http_server.cc:609, 682, 706, 728, 766, 1150, 1175,
1216, 1240, 1271, 1285, 1295` and the `== nullptr` guards at `:784, 811, 841, 860, 882, 913, 937,
963, 992, 1013, 1047`. Meanwhile `POST /api/scan` (`:1388`) and `POST /api/reset` (`:1396`) run on a
*different* worker, take `deviceSetMutex_` exclusively, and `devices_.clear()`.

Concretely: `GET /api/devices/1/sdo/0x6041/0` resolves a `Device*`, and before it issues the SDO
another worker serves `POST /api/scan`. The vector is cleared and rebuilt; the handler then reads a
destroyed object and calls into a `FieldbusDriver&` that may itself have been replaced.
`GET /api/devices` (`:566` → `to_json(dm)` → `devices()`) and `GET /api/bus-config` (`:574`) iterate
the vector as it is cleared.

`busConfig()` is additionally the only `DeviceManager` read surface with no lock at all — its
siblings `deviceStates`, `deviceDiagnostics`, `dcSync` and `processDataWatchdog` all take
`deviceSetMutex_` shared for exactly this reason (`:904, 927, 954, 981`).

**Fixed.** Both `findDevice` overloads are now **private**, so the compiler enforces what the comment
asked for. `devices()` is gone, replaced by `withDevices(fn)` — the whole-set analogue of
`withDevice`, lending the vector under the shared lock. `busConfig()` and `initialised()` take the
shared lock like their siblings. A new `hasDevice(pos)` serves the 404 guard, which is all that a
dozen of those handlers wanted `findDevice` for.

The handlers that genuinely need a `Device&` go through one file-local helper in `http_server.cc`,
`withDeviceOr404`, which borrows for the handler's whole duration and maps the one failure
`withDevice` itself can report — an unresolved position — to a 404. The handler shapes its own
response, because these endpoints do not agree on one (content negotiation, FoE's octet-stream, the
409-on-state-precondition cases).

### F2 — `parameter()` / `parameters()` race a concurrent re-enumeration

**Severity: high. Data race on an `unordered_map` (rehash under a reader).**

`Device::parameter()` (`device.cc:886`) and `Device::parameters()` (`:413`) read `parameters_`
without `parametersMutex_`. `Device::initializeParameters` replaces the entire map under that mutex
(`device.cc:157`). Three call sites do not exclude each other:

- `DeviceManager::processImageInfo` (`device_manager.cc:485-487`) — holds `deviceSetMutex_`
  **shared**;
- `DeviceManager::serializeDump` (`:560-563`) — holds it **shared**;
- `DeviceManager::initializeDeviceParameters` (`:1016`) — also holds it **shared**.

Two shared holders exclude nothing. `GET /api/process-image` on one worker and
`POST /api/devices/1/parameters` on another therefore rehash and traverse the same map concurrently.

`remapProcessImage` (`:284`) is a fourth: it holds only `busOperationMutex_`, which
`initializeDeviceParameters` never takes. `Device::isCia402()` (`device.cc:69`) was a fifth — two
unlocked lookups, reached from `to_json` on every `GET /api/devices`.

**Fixed.** The raw-pointer `parameter()` is gone. `parameterCopy()` took its name and its place, so
there is now one accessor, it returns `std::optional<DeviceParameter>` under the lock, and the
unlocked lookup survives only as the private `findParameter()` (whose callers all hold the mutex).
`parameters()` — which returned the map by reference — is replaced by `hasParameters()`. `isCia402`
takes the lock for both lookups. `remapProcessImage` now calls `valueAsBytes`, which does the lookup
and the encode in one hold of the device's own lock, and is shorter for it.

### F3 — `MonitoringManager::create` touches the device set with no lock

**Severity: high, same class as F1.**

`monitoring_manager.cc:90-91` calls `deviceManager_.findDevice(...)` and then
`device->parameters().empty()`. It runs on an HTTP worker (`POST /api/monitorings`), so it races both
a rescan and a concurrent re-enumeration.

**Fixed.** The "has this device been enumerated?" question now goes through a `withDevice` borrow
calling `Device::hasParameters()`, so both the lookup and the map read happen under a lock.

### F4 — `stopExchange()`'s 200 ms bound fails silently

**Severity: medium. Latent use-after-free, no diagnostic.**

`stopExchange` (`device_manager.cc:396-400`) waits up to 200 ms for an in-flight cycle to drain, then
**proceeds regardless**. The caller then calls `pd_->ring.allocate()` (which frees `buffer_`) and
`driver_->configureProcessData()` (which rewrites the IOmap) — while the RT thread may still be
inside `ring.write()`'s `memcpy` or the driver's `memcpy(map_, …)`.

The bound is right in intent: a stalled or absent RT loop must never hang a control-plane call. But
CLAUDE.md itself documents sustained overrun as *expected* on coarse-timer Windows hosts, and a
non-RT thread preempted mid-exchange for >200 ms under load is not fantasy. Today the timeout is
indistinguishable from a clean drain.

**Fixed** (the diagnostic, not the bound). `stopExchange` now logs a warning when the deadline
expires, naming what it is about to do anyway. The bound itself stays: never hanging a control-plane
call on a stalled RT loop is the right trade, and the point is that taking it should not be silent —
memory corruption arriving with nothing in the log to explain it is the failure mode that costs
days.

### F5 — `start()` assigns `thread_` outside the lock

**Severity: medium. `std::terminate` in a narrow window.**

`ParameterRefresher::start()` (`parameter_refresher.cc:80-88`) sets `running_ = true`, **unlocks**,
then assigns `thread_`. A concurrent `stop()` in that window sets `running_ = false`, finds
`thread_.joinable()` false, and returns. `start()` then assigns a thread that immediately exits and
is never joined; the destructor's `stop()` early-returns on `!running_`
(`parameter_refresher.cc:92-95`), and destroying a joinable `std::thread` calls `std::terminate`.
`MonitoringManager::start()` (`monitoring_manager.cc:163-172`) has the same window; its `stop()`
lacks the early return, but move-assigning over a still-joinable `thread_` terminates too.

Both classes advertise "All public methods are thread-safe" / "Idempotent". Today `main.cc` calls
each exactly once, so this is latent — but it is exactly the kind of thing the
library-grade-public-surface goal exists to prevent.

**Fixed.** Both assign `thread_` while holding the lock. The new thread's first act is to take that
same mutex, so it simply waits out the rest of `start()` — no deadlock, and the window is gone.

### F6 — `serializeDump` holds the shared lock for the whole dump

**Severity: low. Latency, not correctness.**

`serializeDump` (`device_manager.cc:505`) holds `deviceSetMutex_` shared while serialising the entire
recorder span — up to `recorderCapacity` (default 300 000) records, tens of megabytes. `readRecord`
takes the lock **per record** specifically to avoid becoming a long shared holder
(`:422-429`); this one does the opposite. While a dump streams, `scan`, `reset` and every re-map
block.

It is not wrong — the lock is what keeps the ring alive under it — but the two policies should agree,
or the divergence should say why. Chunking (re-take per record, tolerating a re-map ending the span
early, exactly as a lapped cursor already does) would align them.

### F7 — `initialised()` reads `driver_` unlocked

**Severity: low.**

`device_manager.h:240` reads the `unique_ptr` with no lock while `init`/`reset` write it under the
exclusive lock. Formally a data race; benignly a pointer read. The observable effect is a TOCTOU at
`http_server.cc:1370`, where two concurrent `POST /api/init` both see `false` — harmless, since
`init()` re-checks under the lock, but the second gets a 500 instead of the intended 409.

### F8 — stale documentation *(fixed in this commit)*

`docs/THREADS.md` and `docs/CLASS_DIAGRAM.md` both named a `DeviceManager::busMutex_` that no longer
exists (it is now `busOperationMutex_` + `deviceSetMutex_`), and THREADS.md's "five threads" count
predated the 32-worker HTTP pool and the procedure `jthread`s — which is what made F1–F3 invisible:
if the HTTP API really were one thread, every one of them would be benign. Both are corrected
alongside this file.

### Accepted, not defects

- **`SoemFieldbusDriver::exchangeProcessData` reads `ctx_` lock-free** (`:499`) while `closeContext`
  writes it under `controlPlaneMutex_` (`:528-531`). Correct only because `stopExchange()` orders it
  upstream. Load-bearing and worth naming, but the alternative (a lock on the RT path) is worse.
- **`GameLoop::health()` can divide a new-epoch `sumExecNs_` by an old-epoch `executedCycles_`**
  around a `setPeriod`. Diagnostics only, self-corrects within a cycle.
- **A batch output stage can straddle two RT cycles** (≤1 ms skew) — documented at
  `device_manager.h:621-624`; fixing it would require RT-side generation gating and break the
  lock-free output path.

## Checklist when adding a lock

1. Does the RT path reach it? If yes, stop — find a lock-free protocol instead.
2. Where does it sit in the [ordering](#lock-ordering)? If it is a new subsystem lock, it goes
   *above* the `DeviceManager` chain and must be released before any `DeviceManager` call.
3. Is it held across bus I/O? If yes, say so explicitly in its declaration comment and justify the
   duration.
4. Which threads take it? Name them — "the HTTP thread" is not an answer, because there are 32.
5. Does it hand out a pointer or reference to something the lock protects? If so, either return a
   copy or lend access under the lock (the `withDevice` pattern).
6. Add a row to the [inventory](#mutex-inventory) and a paragraph under
   [the mutexes in detail](#the-mutexes-in-detail).
