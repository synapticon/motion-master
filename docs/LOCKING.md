# Locking and Synchronization

> **Hand-maintained.** Derived by reading the source. When you add, remove, or re-scope a
> synchronization primitive, update this file in the same commit.
>
> Companion documents: [THREADS.md](THREADS.md) (which threads exist and what each does),
> [CLASS_DIAGRAM.md](CLASS_DIAGRAM.md) (ownership), [RT_SCHEDULING.md](RT_SCHEDULING.md)
> (`SCHED_FIFO` / `mlockall`).

Motion Master has **nine mutexes, two condition variables, and four lock-free protocols**, plus a
handful of standalone atomics. The lock-free protocols are documented here alongside the mutexes on
purpose: they are not an optimisation applied to a mutex-protected design, they *are* the design on
the paths that matter — the cycle gate, the parameter cell, the recorder ring, the AL-state mirror.
Reasoning about a mutex without them is reasoning about half the machine.

A user cyclic task reads and writes device values from inside the RT loop without acquiring
anything, so the protocols below are what makes the Tier-3 surface safe. A change to `Device`,
`DeviceParameter` or `ProcessData` is a change to this page.

## Contents

- [The one-page summary](#the-one-page-summary)
- [Who runs concurrently](#who-runs-concurrently)
- [Mutex inventory](#mutex-inventory)
- [Lock ordering](#lock-ordering)
- [The mutexes in detail](#the-mutexes-in-detail)
- [Lock-free protocols](#lock-free-protocols) — [the cycle gate](#the-cycle-gate),
  [the parameter cell](#the-parameter-cell)
- [Invariants](#invariants)
- [How these rules are checked](#how-these-rules-are-checked)
- [Accepted trade-offs](#accepted-trade-offs)
- [Checklist when adding a lock](#checklist-when-adding-a-lock)

## The one-page summary

Six rules carry almost all of the correctness:

1. **On the control plane, no lock-protected state is reachable without the lock.** `DeviceManager`
   hands out no reference into `devices_` and `Device` none into `parameters_`: you borrow
   (`withDevice` / `withDevices`, which hold the lock for the callable's whole duration) or you get a
   copy (`parameter`, `parametersOrdered`, `value`). This is the rule the others rest on — it is what
   makes "which lock does this need?" a question with an answer at the call site.
2. **The RT path reaches a device without a lock, and nothing but convention keeps that safe.**
   `DeviceManager::findDevice` and `Device::findParameter` are public and take no lock, because a
   cyclic task must resolve its signals without ever blocking. The obligation that replaces the lock
   is [`DeviceManager::CycleLock`](#the-cycle-gate): a task holds one for its whole body, and inside
   it the raw lookups are safe. Off the RT thread, use the borrow instead — a raw `findDevice` there
   with no borrow is a use-after-free that compiles. This is the one place in the file where safety
   rests on a documented obligation rather than on a design where the mistake cannot be written; it
   is documented at both declarations and should stay confined to the RT surface.
3. **The RT thread takes no lock, ever.** `DeviceManager::exchangeProcessData` and `CycleLock` are
   gated by an atomic image pointer and an atomic depth counter, not a mutex. Everything the RT
   thread touches — the IOmap, the parameter cells, the recorder ring — is lock-free by construction.
   `CycleLock` is a lock in name only: one atomic increment and one atomic load, and it never waits.
4. **`FieldbusDriver::controlPlaneMutex_` is per *transaction*; `DeviceManager::busOperationMutex_`
   is per *operation*.** The first serialises one socket round-trip, the second serialises a whole
   multi-transaction activity (a scan, an AL transition, a re-map). They are not tiers of the same
   thing and a caller often holds only one.
5. **`deviceSetMutex_` guards lifetime, not data.** Shared means "the `Device` objects and the
   process-data runtime will not be freed while I hold this". It is legitimate to hold it shared for
   minutes, because its exclusive holders are rare and brief.
6. **No lock is held across bus I/O except `controlPlaneMutex_` itself**, and that one is held for
   exactly one transfer. `Device::parametersMutex_` in particular is taken, released for the
   transfer, and re-taken to commit.

## Who runs concurrently

| Thread | Count | Created in | Notes |
| --- | --- | --- | --- |
| RT game loop | 1 | the main thread becomes it, `main.cc` | takes no lock. **Every `CyclicTask` — including a user's — runs here**, so a Tier-3 task adds no thread and inherits the no-lock rule |
| HTTP event loop | 1 | `http_server.cc` | dispatch and response writes only; never runs a handler |
| **HTTP worker pool** | **32** | `BS::light_thread_pool pool_{32}` (`http_server.h`) | **every route handler runs here** (`libs/api/router.cc`, `pool->detach_task`) |
| WebSocket event loop | 1 | `ws_server.cc` | |
| Monitoring sampler | 1 | `monitoring_manager.cc` | |
| Parameter refresher | 1 | `parameter_refresher.cc` | |
| Procedure runs | 0..N | `std::jthread`, `procedure_manager.cc` | one per in-flight procedure |

**There is no such thing as "the HTTP thread".** Route handlers run on the worker pool, so two REST
requests genuinely run in parallel. Any invariant of the form "both callers are on the HTTP thread,
so they are serialised" is false.

## Mutex inventory

| # | Primitive | Type | Declared | Guards | Taken by | Held across bus I/O? |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | `DeviceManager::busOperationMutex_` | `std::mutex` | `libs/node/device_manager.h` | nothing — a token over an *activity* | `init`, `scan`, `reset`, `configureProcessData`, `transitionToState`, `writeDevicePdoMapping` | **yes**, for the whole operation (seconds) |
| 2 | `DeviceManager::deviceSetMutex_` | `std::shared_mutex` | `libs/node/device_manager.h` | `devices_`, `driver_`, `pd_->generations`, `pd_->ring` storage | exclusive: `init`/`scan`/`reset` + the re-map publish window. shared: every position-based read/write, `withDevice`, the recorder accessors, `serializeDump` | shared: **yes** (a whole procedure). exclusive: no |
| 3 | `Device::parametersMutex_` | `std::mutex` (via `unique_ptr`) | `libs/node/device.h` | `parameters_`' *structure* and an entry's non-atomic fields, `caSupport_` — **not** the atomic cell | every control-plane parameter read/write; **not** `value<T>()` / `setValue<T>()` | **no** — released for every transfer |
| 4 | `FieldbusDriver::controlPlaneMutex_` | `std::mutex` | `libs/comm/fieldbus_driver.h` | SOEM `ctx_` and the whole control plane (SDO, FoE, ESC registers, SII, AL state) | every driver method except `exchangeProcessData` and `slaveState` | **yes**, one transaction — except FoE, which holds it for the whole transfer |
| 5 | `MonitoringManager::mutex_` (+ `cv_`) | `std::mutex`, `condition_variable` | `libs/node/monitoring_manager.h` | `entries_`, `running_`, `nextEpoch_`, `publish_` | sampler thread + the `/api/monitorings` handlers, and `keepFresh` / `stopKeepingFresh` | **no** — dropped for the flush |
| 6 | `ParameterRefresher::mutex_` (+ `cv_`) | `std::mutex`, `condition_variable` | `libs/node/parameter_refresher.h` | `entries_`, `running_` | refresher thread + `acquire`/`release` from the sampler path | **no** — dropped for the poll |
| 7 | `ProcedureManager::mutex_` | `std::mutex` | `libs/node/procedure_manager.h` | `runs_` | `start`/`snapshot`/`cancel`/destructor | no |
| 8 | `ProcedureManager::Run::errorMutex_` | `std::mutex` | `libs/node/procedure_manager.h` | one `std::optional<std::string>` | the run's own thread (write), pollers (read) | no |
| 9 | `ProgressReporter::mutex_` | `std::mutex` | `libs/node/procedure.h` | the step array | the run's own thread (write), pollers (read) | no |
| — | `RingLogSink` (spdlog `base_sink::mutex_`) | `std::mutex` | `apps/motion_master/ring_log_sink.h` | the log ring buffer | every logging thread | no |
| — | single-instance lock | `flock` / named mutex | `libs/core/platform.cc` | the *process*, not a data structure | startup only | n/a |

Entries 8 and 9 exist for one reason worth stating: **a finishing procedure thread must never need
`ProcedureManager::mutex_`**, because the destructor collects the running threads under that lock and
joins them after releasing it. Anything the thread writes on its way out is therefore either an
atomic or has a per-run mutex of its own.

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

Declared at `libs/node/device_manager.h` and restated at `libs/node/device.h`.

**No RT-side lock appears in this chain, and that is the point.** `CycleLock` is not a mutex and is
not ordered against any of these; a control-plane operation waits *for* it (via
`ProcessData::pauseCycle`) rather than acquiring it, which is why holding a `CycleLock` across a
control-plane call would deadlock that call against its own drain. A cyclic task makes no such call.

The three subsystem mutexes (5, 6, 7) sit **above** this chain — `MonitoringManager::mutex_` is
released before any `DeviceManager` call, `ParameterRefresher::mutex_` likewise, and
`ProcedureManager` resolves the device *before* taking its own lock. None of them is ever held while
a `DeviceManager` lock is acquired, so they cannot participate in a cycle. Keeping that true is the
single most important rule for anyone adding to those classes.

`std::shared_mutex` has **no upgrade path**, and one consequence is load-bearing rather than
incidental: a `ProcedureBody` runs inside `withDevice` holding `deviceSetMutex_` shared, so it cannot
call `transitionToState` — which is precisely why `BusProcedureBody` exists as a second body shape
(`libs/node/procedure_manager.h`).

## The mutexes in detail

### 1. `DeviceManager::busOperationMutex_` — one control-plane operation at a time

*Type:* plain `std::mutex`. *Guards:* no member at all.

This is a mutual-exclusion token over an **activity**: "only one thing drives the bus through a
multi-step operation at a time". It is what distinguishes a whole `scan` from the individual socket
transactions inside it, which `controlPlaneMutex_` serialises separately.

**Why it is separate from `deviceSetMutex_`.** A single lock doing both jobs would be both
long-held-shared (a procedure) and contended-exclusive (an AL transition) at once — the exact state
in which `std::shared_mutex` contention behaviour decides the outcome. That behaviour is unspecified
by the standard and *opposite* across the supported platforms: glibc's `pthread_rwlock_t` is
reader-preferring and can starve a writer indefinitely, while Windows `SRWLOCK` is documented unfair
and can instead convoy readers behind a pending writer. Splitting removes the situation rather than
betting on a platform.

**The payoff is concrete:** `transitionToState` takes *only* this lock, so its multi-second AL wait
blocks no reader — the monitoring sampler keeps flushing and every `/api/monitorings` endpoint keeps
answering throughout. Likewise `configureProcessData` does its IOmap rebuild and per-device
PDO-mapping SDO reads under this lock alone, and takes `deviceSetMutex_` exclusively only for the
brief publish window in `remapProcessImage`.

**Because only a holder of this lock can rebuild `devices_`/`driver_`, holding it is by itself
sufficient to keep them stable.** That is why `remapProcessImage`, `updateExpectedWkc` and the
`transitionToState` body read `devices_` without `deviceSetMutex_`.

**The trade it accepts, and it is deliberate:** a borrow does **not** exclude an AL transition. A long
procedure and a user-driven state change interleave; the procedure's next bus transaction fails
against the changed state rather than the transition's HTTP request hanging for the procedure's whole
duration.

### 2. `DeviceManager::deviceSetMutex_` — lifetime, not data

*Type:* `std::shared_mutex`.

Shared means: **`devices_` and the process-data runtime are not being rebuilt or freed while I hold
this.** That is the whole contract. It says nothing about the *contents* of a `Device` (see
`parametersMutex_`) and nothing about what state the bus is in.

Exclusive holders are few and brief by design:

- `init` / `scan` / `reset` — these genuinely invalidate every `Device&` in flight;
- the publish window inside `remapProcessImage` — ring re-allocation, generation append, image
  publish.

Everything else takes it shared, including `withDevice` and `withDevices`, which are the **only**
ways to reach a `Device` from outside the class *on the control plane*. Holding it shared for a
multi-second procedure is *correct*, not merely tolerated: a rescan mid-operation is exactly what
would dangle the reference.

**`findDevice` is public and takes no lock**, because an RT cyclic task cannot acquire
`deviceSetMutex_` (shared or not — it may block), and a design where the only route to a `Device` is
a borrow would put device access out of reach of the very code the Tier-3 surface exists for. The
obligation the pointer carries — valid only until the next `scan()` / `reset()` — is carried by
[`CycleLock`](#the-cycle-gate) instead, which the task holds for its whole body and which a rebuild
waits out. The two routes are split by caller:

| Caller | Route | What holds the device still |
| --- | --- | --- |
| control plane (HTTP worker, procedure, sampler) | `withDevice` / `withDevices`, or a position-based method | `deviceSetMutex_` shared, for the call's whole duration |
| RT cyclic task | `findDevice` inside a `CycleLock` | the published image + the `inCycle` drain — no lock |

Anything else — a raw `findDevice` off the RT thread with no borrow — resolves a `Device*` that a
concurrent `POST /api/scan` can destroy before the caller has used it. It compiles, so the warning
lives on the declaration and this table is here.

**The recorder accessors take it for a reason that is easy to get wrong.** `recorderHead`,
`recorderOldestSeq` and `readRecord` take it shared even though `ProcessDataRing` is lock-free. The
adversary is *not* the RT producer — the ring's per-slot sequence re-check handles that. It is
`allocate`/`clear`, which **release the storage** and run from the control plane. Reading the ring
without this lock is a use-after-free, not a torn read — and the window is wide, since a sampler
flush walks thousands of records.

`readRecord` takes the lock **per record rather than per span** so a sampler flush walking thousands
of records never becomes a milliseconds-long shared holder that an exclusive `scan` queues behind. A
span is not atomic anyway — each record self-describes and a lapped one returns `false`.

### 3. `Device::parametersMutex_` — the parameter map, never across a transfer

*Type:* `std::mutex`, held by `unique_ptr` only because `Device` must stay move-constructible for
`std::vector<Device>`.

**The rule that matters: it is never held across bus I/O.** Every method that transfers
(`readParameter`, `writeParameter`, `readObjectComplete`, `readAllParameters`,
`initializeParameters`) takes it, decides what to transfer, **releases it**, transfers, and re-takes
it to commit.

That is not a micro-optimisation. A mailbox transfer queues behind `controlPlaneMutex_`, which an FoE
file transfer holds for its whole multi-second duration — so a lock held across "one mailbox
round-trip" is in fact held for as long as any unrelated bus traffic takes. With a background
parameter refresh and a user on the FoE page — the ordinary configuration, not a rare one — that
would stall every cached read of the device.

**The obligation this imposes on callers:** a `DeviceParameter*` must never be carried across the
release, because `initializeParameters` replaces the whole map. Re-find after the transfer and treat
a miss — or a changed `dataType` — as "re-enumerated mid-transfer" rather than assuming it cannot
happen.

**What it does *not* guard is the value.** A parameter's scalar value lives in `DeviceParameter::bits`
— a `uint64_t` reached through `std::atomic_ref` — and is read and written with no lock at all (see
[the parameter cell](#the-parameter-cell)). This mutex guards the map's *structure* and an entry's
*non-atomic* fields (`dataType`, `bitLength`, `syncState`, `rawValue`, `name`). The split is what
makes `Device::value<T>()` callable from the RT loop while `readParameter` stays a locked
control-plane call, and it is why re-enumeration is the hazard it is: rewriting `dataType` under a
concurrent RT decode would be a data race on a field the decode reads, which is why
`publishParameters` pauses the RT cycle across the swap rather than relying on this mutex alone.

**The map hands out copies, with one deliberate exception.** `parameter()` returns a copy of one
entry, `parametersOrdered()` a snapshot of all of them, `value(index, subindex)` a single field —
each taken under the lock, and each the right choice on the control plane. `findParameter()` is
**public** for the same reason `findDevice` is: a cyclic task resolves its signals that way once per
cycle, and no lock it could take would be RT-safe. Its contract splits by caller — a control-plane
caller must hold `parametersMutex_` for the lookup *and* for any access to a non-atomic field; a
cyclic task holds a `CycleLock` and touches only the cell.

### 4. `FieldbusDriver::controlPlaneMutex_` — one socket transaction

*Type:* `std::mutex`, `mutable` so `const` accessors can lock.

Serialises the entire control plane: SDO, FoE, ESC registers, SII, AL state. **The PDO path
deliberately does not take it** — `SoemFieldbusDriver::exchangeProcessData` runs lock-free, because
SOEM's port layer is internally thread-safe and PDO touches disjoint state (the IOmap) from the
control plane. That is the single decision that keeps a slow SDO from ever stalling the 1 ms cycle.

Held for one transaction, never across a sleep, a blocking wait, or a user callback. Two multi-second
operations demonstrate the discipline:

- `readObjectDictionary` takes it per SDO-Info transaction and releases it across `retrySdoInfo`'s
  back-off sleeps.
- `transitionToState` takes it around each discrete socket transaction, never across the poll sleep
  or the `tick()`/`shouldAbort()` callbacks.

**The accepted caveat, and it is uniform rather than specific to those two:** dropping the lock
between transactions means `ctx_` is not held stable for the operation's duration. A concurrent
`scan`/`reset`/`stop` landing in a gap frees `ctx_` and the next transaction dereferences a dangling
context. Within Motion Master this cannot happen — the callers exclude a rescan for the whole call
(`initializeDeviceParameters` holds `deviceSetMutex_` shared, `transitionToState` holds
`busOperationMutex_`, and `scan`/`reset` need both). **An embedder driving `SoemFieldbusDriver`
directly must provide the same guarantee.**

**FoE is the one long hold.** `readFile`/`writeFile` hold the lock for the entire transfer, which is
why the [AL-state mirror](#the-al-state-mirror) exists: without it, every reader asking "what state
is device 3 in?" would wait out a 12-second firmware write.

### 5–6. `MonitoringManager::mutex_` and `ParameterRefresher::mutex_`

Both follow the identical shape, for the identical reason: **snapshot under the lock, work with the
lock released, commit back under the lock.** `MonitoringManager` calls it `takeDue` → `flushDetached`
→ `commitFlush`; `ParameterRefresher::pollDue` does the same with a key list. Flushing under the lock
would let a control-plane operation holding the bus lock stall not just the sampler but every
`/api/monitorings` endpoint.

The subtle part is **`commitFlush`'s epoch check**. Releasing the lock lets a `remove` + re-create of
the same topic land mid-flush; writing the old flush's cursor onto the new registration would skip it
past cycles it never delivered. The epoch — stamped from `nextEpoch_` at create — is what makes
releasing safe. `ParameterRefresher` gets the same property more cheaply by re-finding the entry by
key after the poll.

Both `start()` implementations assign `thread_` **while holding the lock**, and the new thread's
first act is to take that same mutex, so it waits out the rest of `start()`. Assigning outside the
lock leaves a window in which a concurrent `stop()` sees a not-yet-joinable thread and returns,
leaving a joinable `std::thread` to be destroyed — which calls `std::terminate`.

### 7–9. Procedure locks

`ProcedureManager::mutex_` guards only `runs_`. The design constraint driving everything else is that
**a finishing run must not need it**: the destructor collects joinable threads under the lock and
joins them after releasing it, so a thread that needed the lock on its way out would deadlock against
its own collection. Hence `status`, `finishedAt` and `running` are atomics, and the one field that
cannot be — the error string — gets its own per-run, uncontended mutex. `libc++` implements neither
`std::atomic<std::shared_ptr<T>>` nor an atomic string, so this is a portability fact and not a
preference.

`discardIfRescanned` is subtle in one way worth flagging: it **never drops a still-running entry**,
and that is a correctness requirement. A running thread holds a `shared_ptr` to its own `Run`, which
owns the `std::jthread` it is executing on; releasing the last *other* reference would have the
thread destroy its own jthread as it exits and self-join. This is reachable because a
`BusProcedureBody` holds no bus lock between steps, so a `scan()` can land mid-run.

## Lock-free protocols

These carry the RT path. Each is a *protocol*, not just an atomic — the ordering is the mechanism.

### The cycle gate

`ProcessData::image` plus `ProcessData::inCycle` (`libs/node/process_data.h`) are the **only** thing
standing between the RT thread and a control-plane teardown. The handshake is one StoreLoad pair on
each side:

- **RT side:** raise `inCycle` **before** loading the image, then load it; back out if it is null.
  Done at two nesting levels — `exchangeProcessData` around the exchange itself and `CycleLock` one
  level up, around a whole cyclic task body — which is why `inCycle` is a **depth counter and not a
  flag**.
- **Control-plane side:** `ProcessData::pauseCycle` stores `nullptr` **then** waits for `inCycle` to
  reach zero.

Both sides use `seq_cst` on that pair specifically because it is a StoreLoad, which nothing weaker
prevents from reordering. The resulting total order guarantees that for any concurrent teardown,
*either* the RT thread observes the null image and backs out, *or* `pauseCycle` observes the depth and
drains it — never both missing each other. Unpublishing is what stops a *new* cycle starting; the wait
covers the one already running. `resumeCycle(previous)` republishes for a mutation that is not a
teardown — a device's parameter-map swap is the case that needs it; a re-map or rescan publishes a
freshly built image instead, or nothing at all.

**The pause protects far more than the IOmap.** Because a cyclic task inside a `CycleLock` walks
`devices_` and dereferences `DeviceParameter`s, the same drain is what makes `scan`/`reset`
(destroying devices) and `initializeParameters` (replacing a map) safe against the RT thread —
without either taking a lock the RT thread would have to take too.

The bound is **200 ms**, and expiry is logged rather than silent: `stopExchange` warns and names what
it is about to do anyway. Never hanging a control-plane call on a stalled RT loop is the accepted
trade, and the point of the log line is that taking it is not silent — memory corruption arriving
with nothing in the log to explain it is the failure mode that costs days.

Note what is **not** checked: `driver_`. Testing it on the RT thread would be an unsynchronised read
of a `unique_ptr` that `init`/`reset` write under a lock the RT thread never takes. A non-null image
already implies a live driver, established by ordering rather than by a check the RT thread has no
safe way to make.

Published images are **retained** in `generations` until `reset()`, so a lock-free reader can never
dereference a freed image after a re-map republishes.

### The parameter cell

`DeviceParameter::bits` (`libs/node/device_parameter.h`) is the value's single home: a plain
`uint64_t` of raw little-endian wire bytes, LSB-aligned, read and written **only** through
`loadBits()` / `storeBits()`, which wrap it in a `std::atomic_ref<uint64_t>` with **relaxed**
ordering. One cell per object, so it is simultaneously the setpoint a writer stores and the reading
the RT decode publishes.

Four properties make it work, and each is load-bearing:

- **Relaxed is correct, not a shortcut.** The cell is self-contained with no companion state to order
  against, and a reader is by definition unsynchronised with the writer that published it. What
  relaxed guarantees — no torn or invented value, and the last store eventually visible — is exactly
  what a control loop needs.
- **Single composer.** Any number of non-RT writers store into *different* objects' cells without
  contending (same object is last-writer-wins), and the RT loop is the sole thread that composes the
  output cells into the packed wire image each cycle. That is what makes bit-packed objects sharing a
  byte safe without a lock — NEXTGEN's "Design B".
- **`atomic_ref`, not `std::atomic<uint64_t>`.** An atomic member would make `DeviceParameter`
  neither copyable nor movable, and the struct must be both (`parameter()` and `parametersOrdered()`
  hand out copies; entries are moved into the map and into growing vectors). Hand-writing all five
  special members is the real hazard — a field added and forgotten in a copy constructor loses data
  silently. Putting the lock-freedom on the *access* keeps compiler-generated copies. Two
  `static_assert`s pin the contract: always-lock-free, and `atomic_ref`'s alignment requirement.
- **`mutable`, and `storeBits` is `const`.** A cell is written through a `const DeviceParameter*` —
  that is what lets `ProcessImageEntry::parameter` be `const` and `buildProcessImage` take devices by
  const reference. The value is a mutable atomic, like a lock.

**Non-scalars are not in the cell and not RT-readable.** Strings and byte arrays live in `rawValue` (a
`std::vector<uint8_t>`), guarded by `parametersMutex_` like any other non-atomic field; `scalar<T>()`
returns `nullopt` for them and `currentValue()` may allocate. A cycle reads numbers.

### The RT decode, and why the image carries a parameter pointer

`ProcessImageEntry::parameter` (`libs/node/process_image.h`) is a `const DeviceParameter*` resolved
once at publish time, and it is the one place a raw parameter pointer is deliberately held across a
long interval. It exists because the RT loop decodes **every** mapped input into its cell after each
exchange (`decodeInputsIntoCells`): a hash lookup per mapped object is nothing for one device and
**60–100 µs against a 1 ms grid at 50 devices × 40 objects**, so the decode walks contiguous entries
with no lookups at all.

That pointer's validity is a direct consequence of the cycle gate: it is invalidated only by
`initializeParameters` (which replaces the map) and `scan`/`reset` (which destroy the device), and
**all three pause the RT cycle across the change**; a re-map rebuilds every entry. `nullptr` is legal
and means the dictionary was not enumerated when the image was built — the object still exchanges, it
simply has nowhere to decode into.

The eager decode is a **read-path** decision: it makes `value<T>()` a lookup plus an atomic load
instead of loading the published image, locating the object in it and extracting bits from the newest
ring record — so the RT task, HTTP reads, monitoring and `readParameter`'s PDO branch all become a
cell load. It loses the sparse case (10 signals read off a 2000-object bus decodes 1990 values nobody
wants); the escape hatch, if that ever bites, is a "someone has bound this" flag per entry. Not built.

### The recorder ring

`ProcessDataRing` (`libs/node/process_data_ring.{h,cc}`) is single-writer / many-reader. The RT
`write()` is wait-free: invalidate the slot's sequence word, `memcpy` the payload, release-store the
real sequence, release-store the new head. A reader checks the sequence, copies, then **re-checks it
after an acquire fence** — which detects a producer that lapped it mid-copy. A lapped reader is a
normal, detected outcome, not a bug: it returns `false` and the caller resyncs to `oldestValidSeq()`.

**The guarantee is scoped to `write()` and that scoping is the whole point.** `allocate` and `clear`
are a third kind of writer — they release the storage, so no reader-side sequence check can make them
safe. The owner must exclude readers around them, which is what `deviceSetMutex_` does.

### The AL-state mirror

`SoemFieldbusDriver::slaveStates_` is a lock-free array of `std::atomic<uint16_t>`, published by
`publishSlaveStates()` at the end of every locked scope that could have changed a cached AL status. It
exists so `slaveState()` can honour the **"must not block"** contract on `FieldbusDriver::slaveState`:
the real value lives in SOEM's slavelist, reachable only under `controlPlaneMutex_`, which a firmware
transfer holds for its whole duration — and `slaveState` is asked once per device per monitoring
flush.

Relaxed ordering is correct rather than a shortcut: each entry is self-contained with no companion
state to order against, and a reader is by definition unsynchronised with the transition publishing
it. Allocated once at `EC_MAXSLAVE` and never reallocated, so a reader may index it for the driver's
whole lifetime — which is also why it is not a `vector<atomic>`, whose reallocation paths are
ill-formed on libc++ and MSVC.

### The remaining atomics

| Atomic | Where | Pattern |
| --- | --- | --- |
| `GameLoop::period_` | `game_loop.h` | many writers, single (RT) reader; reloaded at the top of each iteration |
| `GameLoop::running_` | `game_loop.h` | `stop()` is async-signal-safe — `static_assert`ed lock-free |
| `GameLoop` counters | `game_loop.h` | single RT writer, relaxed; diagnostics only, never synchronisation |
| `topologyGeneration_` / `processImageGeneration_` | `device_manager.h` | version counters an off-thread consumer compares to know its captured state is stale. **Position is not identity**: a task pinned to position 4 can find different hardware there after a rescan, and this is the only signal that says so |
| `lastWkc` / `expectedWkc` | `process_data.h` | RT writes the first, control plane the second |
| `HttpServer`/`WebSocketServer` `loop_`, `app_`, `running_` | `http_server.h`, `ws_server.h` | cross-thread `defer()` targets |
| `Router::stopping_` | `http_server.h` | set and read on the loop thread — serialised by the thread, not by the atomic. It closes the window where a request arriving *during* the pool drain would dispatch a fresh worker that then defers onto a dead loop |
| `Run::status`/`finishedAt`/`running` | `procedure_manager.h` | written last-to-first so a poller that sees "finished" also sees the outcome |

## Invariants

Things that must stay true. Each is currently relied upon somewhere.

1. **The RT thread acquires no mutex.** Verify by inspection of `exchangeProcessData`, `CycleLock`,
   and everything a registered `CyclicTask` calls. `Device::value<T>()` / `setValue<T>()` are the only
   value access permitted there.
2. **No subsystem mutex (monitoring, refresher, procedure) is held while a `DeviceManager` method is
   called.** This is what keeps the lock graph acyclic.
3. **`parametersMutex_` is not held across any bus transfer.**
4. **A `DeviceParameter*` never crosses a `parametersMutex_` release** on the control plane. The two
   sanctioned exceptions both substitute the cycle gate for the mutex: `ProcessImageEntry::parameter`
   (rebuilt on every re-map, and every invalidating change pauses the cycle) and a cyclic task's
   per-cycle lookup (held only within one `CycleLock`, never across cycles).
5. **`deviceSetMutex_` is not recursive**, and `withDevice`'s callable must not re-enter `scan`,
   `reset`, `init`, `configureProcessData` or `transitionToState`.
6. **Every reader of `pd_->ring` holds `deviceSetMutex_` shared.**
7. **`pauseCycle()` (via `stopExchange()`) precedes every mutation of the IOmap, the ring storage,
   `driver_`, `devices_`, or a device's `parameters_` map.** The last two are what a cyclic task
   walks, so the pause is not only about the IOmap.
8. **Published `ProcessImage` generations are retained until `reset()`.**
9. **A cyclic task holds a `CycleLock` for its whole body, and does nothing when it is falsy.** No
   `findDevice` / `findParameter` result and no cell access on the RT thread is valid outside one.
10. **A `CycleLock` is never held across a control-plane call** — that call's own drain would wait on
    it forever.
11. **An entry's non-atomic fields (`dataType`, `bitLength`, `rawValue`, `syncState`, `name`) are
    written only under `parametersMutex_` *and* with the RT cycle paused**, because the RT decode
    reads `dataType` / `bitLength` without a lock.
12. **Every `DeviceManager` read surface takes `deviceSetMutex_` shared** — `busConfig`,
    `initialised`, `deviceStates`, `deviceDiagnostics`, `dcSync`, `processDataWatchdog`. The
    lock-free exceptions are the RT ones, and they are gated by the cycle instead.

## How these rules are checked

**By review, and by nothing else. Know that before trusting a green test run.**

The test suite is ~95% single-threaded, so a locking mistake does not fail it. A race detector would
not close the gap either: it sees only what runs concurrently, which here is the `DeviceManager` read
surfaces and `ProcedureManager`'s run lifecycle, and leaves the parts that matter most untouched —
nothing anywhere exercises `exchangeProcessData` against a concurrent re-map, or `busOperationMutex_`
against anything, or the real driver's `controlPlaneMutex_`.

So the practical rules for a change on this page are:

- Read the [invariants](#invariants) above and say which one your change relies on.
- Prefer a design where the mistake cannot compile — borrow-or-copy over a raw accessor — since that
  is the only enforcement here that does not depend on someone remembering. Where the RT path forces
  the raw accessor (`findDevice`, `findParameter`), the obligation moves to an object the caller must
  construct (`CycleLock`) rather than to a sentence they may not read; that is the weakest form of
  enforcement in the file, and it should stay confined to the RT surface.
- Treat "the tests pass" as evidence about behaviour, not about synchronisation.

## Accepted trade-offs

Known, deliberate, and not defects.

- **`serializeDump` holds `deviceSetMutex_` shared for the whole dump** — up to `recorderCapacity`
  (default 300 000) records, tens of megabytes, during which `scan`, `reset` and every re-map block.
  `readRecord` takes the lock per record precisely to avoid this, so the two policies diverge.
  Nothing depends on aligning them: a dump is a deliberate, user-initiated act whose latency the user
  is already waiting on. Chunking (re-take per record, tolerating a re-map ending the span early,
  exactly as a lapped cursor already does) is the fix if that changes.
- **`SoemFieldbusDriver::exchangeProcessData` reads `ctx_` lock-free** while `closeContext` writes it
  under `controlPlaneMutex_`. Correct only because `stopExchange()` orders it upstream. Load-bearing
  and worth naming, but the alternative — a lock on the RT path — is worse.
- **A cyclic task can drive hardware that a rescan has renumbered.** `findDevice(4)` after an
  insertion resolves whatever now sits at position 4. `topologyGeneration()` is the mechanism for
  noticing, and it is documented on `findDevice`, but nothing enforces the check — a Tier-3 program
  knows the machine it was written for, and inserting a node is a commissioning act after which you
  rescan and restart.
- **Two concurrent `POST /api/init` can both observe `initialised() == false`** before either enters
  `init()`, which re-checks under the lock. The second gets a 500 rather than the intended 409.
- **`GameLoop::health()` can divide a new-epoch `sumExecNs_` by an old-epoch `executedCycles_`**
  around a `setPeriod`. Diagnostics only, self-corrects within a cycle.
- **A batch output stage can straddle two RT cycles** (≤1 ms skew) — `stageProcessDataOutputs` stores
  cells sequentially on a non-RT thread. Fixing it would require RT-side generation gating and break
  the lock-free output path.

## Checklist when adding a lock

1. Does the RT path reach it? If yes, stop — find a lock-free protocol instead. Remember that the RT
   path includes any user `CyclicTask`, so "does a cyclic task call this?" is part of the question.
2. Where does it sit in the [ordering](#lock-ordering)? If it is a new subsystem lock, it goes
   *above* the `DeviceManager` chain and must be released before any `DeviceManager` call.
3. Is it held across bus I/O? If yes, say so explicitly in its declaration comment and justify the
   duration.
4. Which threads take it? Name them — "the HTTP thread" is not an answer, because there are 32.
5. Does it hand out a pointer or reference to something the lock protects? If so, either return a
   copy or lend access under the lock (the `withDevice` pattern). The only sanctioned alternative is
   the RT one: a raw pointer whose lifetime is held by the cycle gate instead, documented at the
   declaration and valid only inside a `CycleLock`.
6. Add a row to the [inventory](#mutex-inventory) and a paragraph under
   [the mutexes in detail](#the-mutexes-in-detail).
