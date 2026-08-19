# Locking and Synchronisation

> **Hand-maintained.** The source code is the authority. When you add a synchronisation
> primitive, remove one, or change its scope, update this file in the same commit.
>
> Companion documents: [THREADS.md](THREADS.md) lists the threads and the work of each one.
> [CLASS_DIAGRAM.md](CLASS_DIAGRAM.md) shows ownership. [RT_SCHEDULING.md](RT_SCHEDULING.md)
> covers `SCHED_FIFO` and `mlockall`.

Motion Master holds nine mutexes, two condition variables, and four lock-free protocols. Some
standalone atomics sit beside them. This file lists all of them in one place.

The real-time loop takes no lock. So no mutex can protect anything that the loop touches. Four
lock-free protocols do that job instead:

- **The cycle gate** stops the control plane from freeing the process image, a device, or a
  parameter map while the real-time thread uses it.
- **The parameter cell** is the single home of a scalar value. Any thread reads it or writes it with
  one atomic access.
- **The recorder ring** carries one record per cycle from the real-time thread to the monitoring
  readers and to the dump.
- **The AL-state mirror** reports the state of a device without the driver lock, which a firmware
  transfer holds for several seconds.

A cyclic task reads and writes device values inside the real-time loop, and it acquires nothing.
Those four protocols are what makes that surface safe. A change to `Device`, `DeviceParameter`, or
`ProcessData` is a change to this file.

To find every primitive in the source, run:

```bash
grep -rn 'std::mutex\|shared_mutex\|condition_variable\|std::atomic' libs apps
```

## Contents

- [Terms](#terms)
- [The six rules](#the-six-rules)
- [Which threads run at the same time](#which-threads-run-at-the-same-time)
- [Mutex inventory](#mutex-inventory)
- [Lock order](#lock-order)
- [The mutexes in detail](#the-mutexes-in-detail)
- [Lock-free protocols](#lock-free-protocols) — [the cycle gate](#the-cycle-gate),
  [the parameter cell](#the-parameter-cell)
- [Invariants](#invariants)
- [How we check these rules](#how-we-check-these-rules)
- [Accepted trade-offs](#accepted-trade-offs)
- [Checklist when you add a lock](#checklist-when-you-add-a-lock)

## Terms

This file uses the vocabulary of the code. Read this table first if the repository is new to you.

| Term | Meaning |
| --- | --- |
| SOEM | Simple Open EtherCAT Master. The library under the fieldbus driver. |
| Fieldbus | The EtherCAT network. The fieldbus driver owns the socket. |
| Device | One EtherCAT node on the bus, at a fixed position in the chain. |
| Real-time loop | The cyclic thread that exchanges one fieldbus frame per cycle. The default period is 1 ms. |
| Cyclic task | A `CyclicTask` object. The real-time loop calls its `execute()` once per cycle. |
| Tier 3 | The extension surface for user control code. A user of Tier 3 adds a cyclic task. |
| Control plane | Every path that is not the real-time loop: HTTP handlers, procedures, and the two sampler threads. |
| PDO | Process Data Object. A cyclic frame carries it. Its bytes live in the IOmap. |
| IOmap | The flat byte buffer that SOEM exchanges with the bus in each cycle. |
| Process image | The description of which object sits at which offset in the IOmap. |
| Re-map | A rebuild of the process image after a change of the PDO mapping. |
| SDO | Service Data Object. One mailbox read or write of one dictionary entry. |
| CoE | CANopen over EtherCAT. The mailbox protocol that carries an SDO. |
| FoE | File over EtherCAT. The mailbox protocol for a file transfer, such as firmware. |
| AL state | Application Layer state of a device: INIT, PRE-OP, SAFE-OP, OP, or BOOT. |
| ESC | EtherCAT Slave Controller. The chip in a device. It holds registers that we read. |
| SII | Slave Information Interface. The EEPROM in a device. |
| WKC | Working counter. The number of devices that answered a frame. |
| Borrow | A call that lends a `Device&` to a callable and holds the lock for the whole call. |
| Procedure | Off-real-time command-and-wait work on one device, such as offset detection. |

## The six rules

Six rules carry almost all of the correctness.

1. **On the control plane, lock-protected state is unreachable without the lock.** `DeviceManager`
   hands out no reference into `devices_`. `Device` hands out no reference into `parameters_`. A
   caller borrows or takes a copy. `withDevice` and `withDevices` borrow, and they hold the lock for
   the whole duration of the callable. `parameter`, `parametersOrdered`, and `value` return a copy.
   The other rules rest on this one. It is the reason that the question "which lock does this need?"
   has an answer at the call site.
2. **The real-time path reaches a device without a lock. Only a convention keeps that safe.**
   `DeviceManager::findDevice` and `Device::findParameter` are public and take no lock, because a
   cyclic task must resolve its signals without a block. The obligation that replaces the lock is
   [`DeviceManager::CycleGuard`](#the-cycle-gate). A task holds one `CycleGuard` for its whole body,
   and inside it the raw lookups are safe. Off the real-time thread, borrow instead. A raw
   `findDevice` there, with no borrow, is a use-after-free that compiles. This is the one place in
   this file where safety rests on a documented obligation. Everywhere else the design makes the
   mistake impossible to write. Both declarations carry the warning. Keep the pattern inside the
   real-time surface.
3. **The real-time thread takes no lock, ever.** An atomic image pointer and an atomic depth counter
   gate `DeviceManager::exchangeProcessData` and `CycleGuard`. No mutex gates them. Everything the
   real-time thread touches is lock-free by construction: the IOmap, the parameter cells, and the
   recorder ring. `CycleGuard` is not a lock. It does one atomic increment and one atomic load, and
   it never waits.
4. **`FieldbusDriver::controlPlaneMutex_` covers one transaction.
   `DeviceManager::busOperationMutex_` covers one operation.** The first serialises a single socket
   round-trip. The second serialises a whole activity of many transactions, such as a scan, an AL
   transition, or a re-map. They are not two tiers of one thing, and a caller often holds only one
   of them.
5. **`deviceSetMutex_` guards lifetime, not data.** Shared means one thing: no other thread frees or
   rebuilds the `Device` objects and the process-data runtime while I hold this. A shared hold of
   several minutes is legitimate, because the exclusive holders are rare and brief.
6. **A lock is never held across bus input or output, except `controlPlaneMutex_`.** That one lock
   covers exactly one transfer. `Device::parametersMutex_` shows the pattern: take the lock, release
   it for the transfer, then take it again to commit.

## Which threads run at the same time

| Thread | Count | Created in | Notes |
| --- | --- | --- | --- |
| Real-time game loop | 1 | the main thread becomes it, in `main.cc` | takes no lock. **Every `CyclicTask` runs here, a user's task too.** A Tier-3 task adds no thread and inherits the no-lock rule |
| HTTP event loop | 1 | `http_server.cc` | dispatch and response writes only. It never runs a route handler |
| **HTTP worker pool** | **32** | `BS::light_thread_pool pool_{32}` in `http_server.h` | **every route handler runs here**, through `pool->detach_task` in `libs/api/router.cc` |
| WebSocket event loop | 1 | `ws_server.cc` | |
| Monitoring sampler | 1 | `monitoring_manager.cc` | |
| Parameter refresher | 1 | `parameter_refresher.cc` | |
| Procedure runs | 0 to N | `std::jthread` in `procedure_manager.cc` | one thread per procedure in flight |

**There is no such thing as "the HTTP thread".** Route handlers run on the worker pool, so two
REST requests truly run in parallel. An invariant of this form is false: "both callers run on the
HTTP thread, so the two calls are serialised."

## Mutex inventory

| # | Primitive | Type | Declared in | Guards | Taken by | Held across bus I/O? |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | `DeviceManager::busOperationMutex_` | `std::mutex` | `libs/node/device_manager.h` | nothing. It is a token over an *activity* | `init`, `scan`, `reset`, `configureProcessData`, `transitionToState`, `writeDevicePdoMapping` | **yes**, for the whole operation, which can take seconds |
| 2 | `DeviceManager::deviceSetMutex_` | `std::shared_mutex` | `libs/node/device_manager.h` | `devices_`, `driver_`, `pd_->generations`, and the `pd_->ring` storage | exclusive: `init`, `scan`, `reset`, and the re-map publish window. shared: every position-based read or write, `withDevice`, the recorder accessors, `serializeDump` | shared: **yes**, for a whole procedure. exclusive: no |
| 3 | `Device::parametersMutex_` | `std::mutex`, held through a `unique_ptr` | `libs/node/device.h` | the *structure* of `parameters_`, the non-atomic fields of an entry, and `caSupport_`. **Not** the atomic cell | every control-plane parameter read or write. **Not** `value<T>()` or `setValue<T>()` | **no**. It is released for every transfer |
| 4 | `FieldbusDriver::controlPlaneMutex_` | `std::mutex` | `libs/comm/fieldbus_driver.h` | the SOEM context `ctx_` and the whole control plane: SDO, FoE, ESC registers, SII, AL state | every driver method except `exchangeProcessData` and `slaveState` | **yes**, for one transaction. FoE is the exception and holds it for the whole transfer |
| 5 | `MonitoringManager::mutex_` and `cv_` | `std::mutex`, `std::condition_variable` | `libs/node/monitoring_manager.h` | `entries_`, `running_`, `nextEpoch_`, `publish_` | the sampler thread, the `/api/monitorings` handlers, `keepFresh`, `stopKeepingFresh` | **no**. It is dropped for the flush |
| 6 | `ParameterRefresher::mutex_` and `cv_` | `std::mutex`, `std::condition_variable` | `libs/node/parameter_refresher.h` | `entries_`, `running_` | the refresher thread, and `acquire` or `release` from the sampler path | **no**. It is dropped for the poll |
| 7 | `ProcedureManager::mutex_` | `std::mutex` | `libs/node/procedure_manager.h` | `runs_` | `start`, `snapshot`, `cancel`, and the destructor | no |
| 8 | `ProcedureManager::Run::errorMutex_` | `std::mutex` | `libs/node/procedure_manager.h` | one `std::optional<std::string>` | the thread of the run, which writes. Pollers read | no |
| 9 | `ProgressReporter::mutex_` | `std::mutex` | `libs/node/procedure.h` | the step array | the thread of the run, which writes. Pollers read | no |
| — | `RingLogSink`, through the spdlog `base_sink::mutex_` | `std::mutex` | `apps/motion_master/ring_log_sink.h` | the log ring buffer | every thread that logs | no |
| — | single-instance lock | `flock`, or a named mutex on Windows | `libs/core/platform.cc` | the *process*, not a data structure | startup only | not applicable |

Entries 8 and 9 exist for one reason. **A procedure thread that finishes must never need
`ProcedureManager::mutex_`.** The destructor collects the threads that still run under that lock,
then releases the lock and joins them. So everything such a thread writes on its way out is an
atomic, or it has a per-run mutex of its own.

## Lock order

```text
busOperationMutex_
      ↓
deviceSetMutex_
      ↓
Device::parametersMutex_
      ↓
FieldbusDriver::controlPlaneMutex_
```

`libs/node/device_manager.h` declares this order. `libs/node/device.h` restates it.

**No real-time primitive appears in the chain, by design.** `CycleGuard` is not a mutex, and no
order relates it to these four. A control-plane operation waits *for* it, through
`ProcessData::pauseCycle`. It never acquires it. For that reason a `CycleGuard` held across a
control-plane call deadlocks that call against its own drain. A cyclic task makes no such call.

The three subsystem mutexes (5, 6, and 7) sit **above** this chain. `MonitoringManager::mutex_` is
released before any `DeviceManager` call, and `ParameterRefresher::mutex_` likewise.
`ProcedureManager` resolves the device *before* it takes its own lock. No thread holds one of the
three while it acquires a `DeviceManager` lock, so none of them can join a cycle. To keep that
true is the most important rule for anyone who extends those classes.

`std::shared_mutex` has **no upgrade path**, and one consequence shapes the code. A `ProcedureBody`
runs inside `withDevice` and so holds `deviceSetMutex_` shared. It cannot call `transitionToState`,
which needs the same mutex exclusively. That is the reason that `BusProcedureBody` exists as a
second body shape (`libs/node/procedure_manager.h`).

## The mutexes in detail

### 1. `DeviceManager::busOperationMutex_` — one control-plane operation at a time

*Type:* plain `std::mutex`. *Guards:* no member at all.

This is a mutual-exclusion token over an **activity**: only one thing drives the bus through a
multi-step operation at a time. It separates a whole `scan` from the individual socket
transactions inside it, which `controlPlaneMutex_` serialises on its own.

**Why it is separate from `deviceSetMutex_`.** Consider one lock for both jobs. A procedure holds it
shared for a long time. At the same moment an AL transition wants it exclusively. In that state the
contention policy of `std::shared_mutex` decides the outcome. The standard does not specify that
policy, and the supported platforms choose the opposite of each other. The glibc `pthread_rwlock_t`
prefers readers and can starve a writer for an unbounded time. The Windows `SRWLOCK` is documented
as unfair and can instead queue readers behind a writer that waits. The split removes the situation.
It does not bet on one platform.

**The payoff is concrete.** `transitionToState` takes *only* this lock, so its AL wait of several
seconds blocks no reader. The monitoring sampler keeps its flush cadence, and every
`/api/monitorings` endpoint answers throughout. `configureProcessData` does the same: the IOmap
rebuild and the per-device PDO-mapping SDO reads run under this lock alone. It takes
`deviceSetMutex_` exclusively only for the brief publish window in `remapProcessImage`.

**Only a holder of this lock can rebuild `devices_` or `driver_`, so the lock alone keeps them
stable.** That is the reason that `remapProcessImage`, `updateExpectedWkc`, and the body of
`transitionToState` read `devices_` without `deviceSetMutex_`.

**The trade is deliberate: a borrow does not exclude an AL transition.** A long procedure and a
user-driven state change interleave. The next bus transaction of the procedure fails against the
changed state. The alternative is worse: the HTTP request for the transition would hang for the
whole duration of the procedure.

### 2. `DeviceManager::deviceSetMutex_` — lifetime, not data

*Type:* `std::shared_mutex`.

Shared means one thing. **No other thread rebuilds or frees `devices_` and the process-data
runtime while I hold this.** That is the whole contract. It says nothing about the *contents* of a
`Device`, which `parametersMutex_` guards. It says nothing about the state of the bus.

The exclusive holders are few and brief by design:

- `init`, `scan`, and `reset`, because each one invalidates every `Device&` in flight;
- the publish window inside `remapProcessImage`, which re-allocates the ring, appends a
  generation, and publishes the image.

Everything else takes the mutex shared. That includes `withDevice` and `withDevices`, the **only**
routes to a `Device` from outside the class on the control plane. A shared hold for a procedure of
several minutes is *correct*, not merely tolerated. A rescan in the middle of that procedure is
exactly what would dangle the reference.

**`findDevice` is public and takes no lock.** A cyclic task cannot acquire `deviceSetMutex_`, not
even shared, because the acquisition may block. Tier 3 exists for exactly that code. A design where
a borrow is the only route to a `Device` puts device access out of its reach. The pointer carries an
obligation instead: it is valid only until the next `scan()` or `reset()`.
[`CycleGuard`](#the-cycle-gate) carries that obligation. A task holds one for its whole body, and a
rebuild waits it out. The two routes split by caller:

| Caller | Route | What holds the device still |
| --- | --- | --- |
| control plane: HTTP worker, procedure, sampler | `withDevice`, `withDevices`, or a position-based method | `deviceSetMutex_` shared, for the whole duration of the call |
| real-time cyclic task | `findDevice` inside a `CycleGuard` | the published image and the `inCycle` drain. No lock |

Anything else is a defect. A raw `findDevice` off the real-time thread, with no borrow, resolves a
`Device*` that a concurrent `POST /api/scan` destroys before the caller uses it. That code
compiles, so the warning lives on the declaration, and this table repeats it.

**The recorder accessors take the mutex for a reason that is easy to get wrong.**
`recorderHead`, `recorderOldestSeq`, and `readRecord` take it shared, although `ProcessDataRing`
is lock-free. The adversary is *not* the real-time producer, because the per-slot sequence
re-check of the ring handles that case. The adversary is `allocate` or `clear`. Both **release the
storage**, and both run on the control plane. A read of the ring without this lock is a
use-after-free, not a torn read. The window is wide, because one sampler flush walks thousands of
records.

`readRecord` takes the lock **per record, not per span**. Otherwise a sampler flush that walks
thousands of records becomes a shared holder for milliseconds, and an exclusive `scan` queues
behind it. A span is not atomic in any case. Each record describes itself, and a lapped record
returns `false`.

### 3. `Device::parametersMutex_` — the parameter map, never across a transfer

*Type:* `std::mutex`. A `unique_ptr` holds it, and for one reason only: `Device` must stay
move-constructible for `std::vector<Device>`.

**The rule that matters: no caller holds it across bus input or output.** Every method that
transfers takes the mutex, decides what to transfer, **releases it**, transfers, then takes it
again to commit. The methods are `readParameter`, `writeParameter`, `readObjectComplete`,
`readAllParameters`, and `initializeParameters`.

That is not a micro-optimisation. A mailbox transfer queues behind `controlPlaneMutex_`, and an
FoE file transfer holds that lock for its whole duration of several seconds. So a lock held across
"one mailbox round-trip" is in fact held for as long as any unrelated bus traffic takes. Take a
background parameter refresh and a user on the FoE page, which is the ordinary configuration and
not a rare one. A lock across the transfer stalls every cached read of the device.

**The obligation on callers:** never carry a `DeviceParameter*` across the release, because
`initializeParameters` replaces the whole map. Find the entry again after the transfer. Treat a
miss, or a changed `dataType`, as re-enumeration in the middle of the transfer. Do not assume that
the case cannot happen.

**This mutex does not guard the value.** The scalar value of a parameter lives in
`DeviceParameter::bits`, a `uint64_t` reached through `std::atomic_ref`. Readers and writers touch
it with no lock at all, as [the parameter cell](#the-parameter-cell) describes. This mutex guards
the *structure* of the map and the *non-atomic* fields of an entry: `dataType`, `bitLength`,
`syncState`, `rawValue`, and `name`. That split is what makes `Device::value<T>()` callable from
the real-time loop while `readParameter` stays a locked control-plane call.

The split also explains the hazard of re-enumeration. A rewrite of `dataType` under a concurrent
real-time decode is a data race on a field that the decode reads. So `publishParameters` pauses
the real-time cycle across the swap. This mutex alone is not enough.

**The map hands out copies, with one deliberate exception.** `parameter()` returns a copy of one
entry. `parametersOrdered()` returns a snapshot of all entries. `value(index, subindex)` returns one
field. Each of the three takes the lock, and each is the right choice on the control plane.
`findParameter()` is the exception. It is **public**, for the same reason as `findDevice`. A cyclic
task resolves its signals that way once per cycle. No lock it could take is real-time safe. Its
contract splits by caller. A control-plane caller must hold `parametersMutex_` for the lookup *and*
for every access to a non-atomic field. A cyclic task holds a `CycleGuard` and touches only the
cell.

### 4. `FieldbusDriver::controlPlaneMutex_` — one socket transaction

*Type:* `std::mutex`, declared `mutable` so that a `const` accessor can lock it.

It serialises the entire control plane: SDO, FoE, ESC registers, SII, and AL state. **The PDO path
deliberately does not take it.** `SoemFieldbusDriver::exchangeProcessData` runs lock-free, for two
reasons. The port layer of SOEM is thread-safe on its own. PDO touches state that is disjoint from
the control plane: the IOmap. That single decision is the reason that a slow SDO never stalls the 1
ms cycle.

A caller holds it for one transaction, and never across a sleep, a wait that blocks, or a user
callback. Two operations of several seconds show the discipline:

- `readObjectDictionary` takes it per SDO-Info transaction. It releases the lock across the
  back-off sleeps of `retrySdoInfo`.
- `transitionToState` takes it around each discrete socket transaction. It never holds the lock
  across the poll sleep, or across the `tick()` and `shouldAbort()` callbacks.

**The accepted caveat is uniform, and not specific to those two methods.** A drop of the lock
between transactions means that `ctx_` is not stable for the duration of the operation. A concurrent
`scan`, `reset`, or `stop` that lands in a gap frees `ctx_`, and the next transaction dereferences a
freed context. Inside Motion Master this cannot happen, because the callers exclude a rescan for the
whole call. `initializeDeviceParameters` holds `deviceSetMutex_` shared, `transitionToState` holds
`busOperationMutex_`, and `scan` and `reset` need both. **An embedder that drives
`SoemFieldbusDriver` directly must give the same guarantee.**

**FoE is the one long hold.** `readFile` and `writeFile` hold the lock for the entire transfer.
That is the reason that the [AL-state mirror](#the-al-state-mirror) exists. Without the mirror,
every reader that asks for the state of device 3 waits out a firmware write of 12 seconds.

### 5 and 6. `MonitoringManager::mutex_` and `ParameterRefresher::mutex_`

Both have the identical shape, for the identical reason. **Snapshot under the lock, work with the
lock released, then commit back under the lock.** `MonitoringManager` names the three steps
`takeDue`, `flushDetached`, and `commitFlush`. `ParameterRefresher::pollDue` does the same with a
key list. A flush under the lock lets a control-plane operation that holds the bus lock stall the
sampler, and with it every `/api/monitorings` endpoint.

The subtle part is the **epoch check in `commitFlush`**. A release of the lock lets a `remove` and
a re-create of the same topic land in the middle of a flush. A write of the cursor of the old
flush onto the new registration skips the new one past cycles that it never delivered. The epoch
prevents that. `MonitoringManager` stamps it from `nextEpoch_` at create time, and the stamp is
what makes the release safe. `ParameterRefresher` gets the same property more cheaply: it finds
the entry again by key after the poll.

Both `start()` implementations assign `thread_` **under the lock**, and the first act of the new
thread is to take that same mutex. So the new thread waits out the rest of `start()`. An
assignment outside the lock leaves a window: a concurrent `stop()` sees a thread that is not yet
joinable and returns. That leaves a joinable `std::thread` for the destructor, which calls
`std::terminate`.

### 7 to 9. The procedure locks

`ProcedureManager::mutex_` guards `runs_` and nothing else. One design constraint drives
everything around it: **a run that finishes must not need that mutex.** The destructor collects
the joinable threads under the lock, then releases the lock and joins them. A thread that needs
the lock on its way out deadlocks against its own collection. So `status`, `finishedAt`, and
`running` are atomics. The one field that cannot be an atomic is the error string, and it gets a
per-run mutex of its own, which is never contended. `libc++` implements neither
`std::atomic<std::shared_ptr<T>>` nor an atomic string. This is a portability fact, not a
preference.

`discardIfRescanned` is subtle in one way. It **never drops an entry that still runs**, and that is
a correctness requirement. A thread that still runs holds a `shared_ptr` to its own `Run`, and that
`Run` owns the `std::jthread` that the thread executes on. A release of the last *other* reference
makes the thread destroy its own jthread as it exits, and join itself. The case is reachable,
because a `BusProcedureBody` holds no bus lock between steps. A `scan()` can land in the middle of
such a run.

## Lock-free protocols

These four protocols carry the real-time path. Each one is a *protocol*, not merely an atomic. The
memory ordering is the mechanism.

### The cycle gate

`ProcessData::image` and `ProcessData::inCycle` (`libs/node/process_data.h`) are the **only** thing
between the real-time thread and a control-plane teardown. The handshake is one store-load pair on
each side:

- **Real-time side.** Raise `inCycle` **before** the load of the image. Then load the image. Back
  out if it is null. The code does this at two levels: `exchangeProcessData` around the exchange
  itself, and `CycleGuard` one level up, around a whole cyclic-task body. That is the reason that
  `inCycle` is a **depth counter and not a flag**.
- **Control-plane side.** `ProcessData::pauseCycle` stores `nullptr`, **then** waits for `inCycle`
  to reach zero.

Both sides use `seq_cst` on that pair, and specifically because it is a store-load pair. Nothing
weaker prevents the reorder. That total order gives one guarantee for any concurrent teardown.
Either the real-time thread observes the null image and backs out, or `pauseCycle` observes the
depth and drains it. The two can never miss each other. An unpublished image prevents the start of a
*new* cycle. The wait covers the cycle that already runs. `resumeCycle(previous)` republishes the
image for a mutation that is not a teardown. The swap of the parameter map of a device is the case
that needs it. A re-map or a rescan publishes a freshly built image instead, or publishes nothing at
all.

**The pause protects much more than the IOmap.** A cyclic task inside a `CycleGuard` walks
`devices_` and dereferences `DeviceParameter` objects. So the same drain is what makes `scan` and
`reset` safe, which destroy devices, and `initializeParameters` safe, which replaces a map. Neither
needs a lock that the real-time thread would also have to take.

The bound on the drain is **200 ms** (`libs/node/device_manager.cc`). Expiry is not silent.
`stopExchange` logs a warning, and it names the action that it takes next. It takes that action in
any case. The accepted trade is simple: a stalled real-time loop must never hang a control-plane
call. The log line is what keeps the trade visible. Memory corruption with nothing in the log to
explain it is the failure mode that costs days.

Note what the gate does **not** check: `driver_`. A test of it on the real-time thread is an
unsynchronised read of a `unique_ptr`. `init` and `reset` write that pointer under a lock that the
real-time thread never takes. A non-null image already implies a live driver. The memory ordering
establishes that. A check is not an option, because the real-time thread has no safe way to make
one.

`generations` **retains** every published image until `reset()`. So a lock-free reader can never
dereference a freed image after a re-map republishes.

### The parameter cell

`DeviceParameter::bits` (`libs/node/device_parameter.h`) is the single home of the value. It is a
plain `uint64_t` of raw little-endian wire bytes, aligned to the least significant bit. Only
`loadBits()` and `storeBits()` touch it. Both wrap it in a `std::atomic_ref<uint64_t>` with
**relaxed** ordering. There is one cell per object. So one cell holds both the setpoint that a
writer stores, and the measured value that the real-time decode publishes.

Four properties make it work, and the cell needs all four:

- **Relaxed is correct, not a shortcut.** The cell is self-contained, with no companion state to
  order against, and a reader is by definition unsynchronised with the writer that published the
  value. Relaxed ordering guarantees two things. A reader never sees a torn or invented value. The
  last store becomes visible in the end. That is exactly what a control loop needs.
- **One composer.** Any number of non-real-time writers store into the cells of *different* objects
  with no contention. Two writes to one object resolve as last-writer-wins. The real-time loop is
  the only thread that composes the output cells into the packed wire image each cycle. That is what
  makes bit-packed objects that share a byte safe without a lock. `NEXTGEN.md` records this choice
  as "Design B", together with the alternatives.
- **`atomic_ref`, not `std::atomic<uint64_t>`.** An atomic member makes `DeviceParameter` neither
  copyable nor movable, and the struct must be both. `parameter()` and `parametersOrdered()` hand
  out copies, and the code moves entries into the map and into vectors that grow. To hand-write all
  five special members is the real hazard: a field that someone adds and forgets in the copy
  constructor loses data silently. Lock-freedom on the *access* keeps the compiler-generated copies.
  Two `static_assert` statements pin the contract: always lock-free, and the alignment requirement
  of `atomic_ref`.
- **`mutable`, and `storeBits` is `const`.** A writer writes the cell through a `const
  DeviceParameter*`. That is what lets `ProcessImageEntry::parameter` be `const`, and lets
  `buildProcessImage` take devices by const reference. The value is a mutable atomic, in the way
  that a lock is mutable.

**A non-scalar is not in the cell, and no real-time code may read it.** A string or a byte array
lives in `rawValue`, a `std::vector<uint8_t>`. `parametersMutex_` guards it like any other
non-atomic field. `scalar<T>()` returns `nullopt` for such a parameter, and `currentValue()` may
allocate. A cycle reads numbers.

### The parameter pointer inside the process image

`ProcessImageEntry::parameter` (`libs/node/process_image.h`) is a `const DeviceParameter*`. The
code resolves it once, at publish time. It is the one place where a raw parameter pointer lives
across a long interval. This is a consequence of the cycle gate, not a fifth protocol.

The pointer exists because the real-time loop decodes **every** mapped input into its cell after
each exchange, in `decodeInputsIntoCells`. One hash lookup per mapped object costs nothing for one
device. At 50 devices and 40 objects each it costs **60 µs to 100 µs against a 1 ms grid**. So the
decode walks contiguous entries and does no lookup at all.

The validity of that pointer follows directly from the cycle gate. Only two things invalidate it:
`initializeParameters`, which replaces the map, and `scan` or `reset`, which destroy the device.
**All three pause the real-time cycle across the change**, and a re-map rebuilds every entry. A
`nullptr` is legal. It means that the dictionary was not enumerated when the code built the image.
The object still exchanges. It simply has nowhere to decode into.

The eager decode is a **read-path** decision. It makes `value<T>()` one lookup plus one atomic load.
The alternative is more work. It loads the published image, finds the object inside it, then
extracts the bits from the newest ring record. So the real-time task, the HTTP reads, the
monitoring, and the PDO branch of `readParameter` all become a load of a cell. The eager decode
loses the sparse case. Read 10 signals off a bus of 2000 objects, and the decode still writes 1990
values that nobody wants. The escape hatch for that case is a flag per entry that says "a reader has
bound this". It is not built.

### The recorder ring

`ProcessDataRing` (`libs/node/process_data_ring.{h,cc}`) has one writer and many readers. The
real-time `write()` is wait-free. It invalidates the sequence word of the slot, copies the payload
with `memcpy`, release-stores the real sequence, then release-stores the new head. A reader checks
the sequence, copies the record, then checks the sequence **again after an acquire fence**. The
second check detects a producer that lapped the reader in the middle of the copy. A lapped reader
is a normal and detected outcome, not a defect. `readRecord` returns `false`, and the caller
resyncs to `oldestValidSeq()`.

**The guarantee covers `write()` only, and that scope is deliberate.** `allocate` and `clear` are a
third kind of writer. They release the storage, so no sequence check on the reader side can make
them safe. The owner must exclude readers around them, and `deviceSetMutex_` is what does that.

### The AL-state mirror

`SoemFieldbusDriver::slaveStates_` is a lock-free array of `std::atomic<uint16_t>`.
`publishSlaveStates()` publishes into it at the end of every locked scope that can change a cached
AL status.

The mirror exists so that `slaveState()` can honour the **must-not-block** contract on
`FieldbusDriver::slaveState`. The real value lives in the slavelist of SOEM, reachable only under
`controlPlaneMutex_`. A firmware transfer holds that lock for its whole duration. Every monitoring
flush asks for the state of every device once.

Relaxed ordering is correct here, and not a shortcut. Each entry is self-contained, with no
companion state to order against, and a reader is by definition unsynchronised with the transition
that publishes the entry. The array is allocated once, at `EC_MAXSLAVE` entries, and never
re-allocated. So a reader may index it for the whole lifetime of the driver. That is also the
reason that it is not a `std::vector<std::atomic<uint16_t>>`, whose re-allocation paths are
ill-formed on libc++ and on MSVC.

### The remaining atomics

| Atomic | Where | Pattern |
| --- | --- | --- |
| `GameLoop::period_` | `game_loop.h` | many writers, one reader on the real-time thread. It reloads the value at the top of each iteration |
| `GameLoop::running_` | `game_loop.h` | `stop()` is async-signal-safe. A `static_assert` pins the lock-free property |
| `GameLoop` counters | `game_loop.h` | one real-time writer, relaxed. Diagnostics only. Never a synchronisation mechanism |
| `topologyGeneration_` and `processImageGeneration_` | `device_manager.h` | version counters. An off-thread consumer compares one to learn that its captured state is stale. **Position is not identity**: after a rescan, a task pinned to position 4 can find different hardware there, and this counter is the only signal that says so |
| `lastWkc` and `expectedWkc` | `process_data.h` | the real-time thread writes the first. The control plane writes the second |
| `loop_`, `app_`, and `running_` of the two servers | `http_server.h`, `ws_server.h` | targets of a cross-thread `defer()` |
| `Router::stopping_` | `http_server.h` | set and read on the loop thread. The thread serialises it, not the atomic. It closes the window where a request that arrives *during* the pool drain dispatches a fresh worker, which then defers onto a dead loop |
| `Run::status`, `Run::finishedAt`, `Run::running` | `procedure_manager.h` | written last to first, so a poller that sees "finished" also sees the outcome |

## Invariants

Each item below must stay true. Something in the code relies on each one today.

1. **The real-time thread acquires no mutex.** Verify it by inspection of `exchangeProcessData`, of
   `CycleGuard`, and of everything a registered `CyclicTask` calls. `Device::value<T>()` and
   `Device::setValue<T>()` are the only value access permitted there.
2. **No subsystem mutex is held during a call to a `DeviceManager` method.** This covers the
   monitoring, refresher, and procedure mutexes. It is what keeps the lock graph acyclic.
3. **No caller holds `parametersMutex_` across a bus transfer.**
4. **A `DeviceParameter*` never crosses a release of `parametersMutex_`** on the control plane.
   There are two sanctioned exceptions, and both substitute the cycle gate for the mutex.
   `ProcessImageEntry::parameter` is one: every re-map rebuilds it, and every change that
   invalidates it pauses the cycle. The per-cycle lookup of a cyclic task is the other: it holds
   inside one `CycleGuard`, never across cycles.
5. **`deviceSetMutex_` is not recursive.** The callable of `withDevice` must not re-enter `scan`,
   `reset`, `init`, `configureProcessData`, or `transitionToState`.
6. **Every reader of `pd_->ring` holds `deviceSetMutex_` shared.**
7. **`pauseCycle()`, through `stopExchange()`, precedes every mutation of these: the IOmap, the ring
   storage, `driver_`, `devices_`, and the `parameters_` map of a device.** A cyclic task walks the
   last two, so the pause is not only about the IOmap.
8. **`generations` retains every published `ProcessImage` until `reset()`.**
9. **A cyclic task holds a `CycleGuard` for its whole body, and does nothing when the guard is
   falsy.** On the real-time thread, no `findDevice` result, no `findParameter` result, and no cell
   access is valid outside one.
10. **No code holds a `CycleGuard` across a control-plane call.** The drain of that call would wait
    on the guard forever.
11. **A writer changes the non-atomic fields of an entry only under `parametersMutex_` *and* with
    the real-time cycle paused.** The fields are `dataType`, `bitLength`, `rawValue`, `syncState`,
    and `name`. The real-time decode reads `dataType` and `bitLength` without a lock.
12. **Every read surface of `DeviceManager` takes `deviceSetMutex_` shared.** That includes
    `busConfig`, `initialised`, `deviceStates`, `deviceDiagnostics`, `dcSync`, and
    `processDataWatchdog`. The lock-free exceptions are the real-time ones, and the cycle gate
    guards those instead.

## How we check these rules

**By review, and by nothing else. Know that before you trust a green test run.**

About 95% of the test suite is single-threaded, so a locking mistake does not fail it. A race
detector does not close the gap either. It sees only what truly runs in parallel, which here is the
read surfaces of `DeviceManager` and the run lifecycle of `ProcedureManager`. It leaves the parts
that matter most untouched. Nothing anywhere exercises `exchangeProcessData` against a concurrent
re-map, or `busOperationMutex_` against anything, or `controlPlaneMutex_` of the real driver.

So three practical rules apply to a change on this page:

- Read the [invariants](#invariants) above. Name the one that your change relies on.
- Prefer a design where the mistake does not compile. Borrow-or-copy beats a raw accessor, because
  it is the only enforcement here that does not depend on someone's memory. Where the real-time
  path forces a raw accessor, as `findDevice` and `findParameter` do, move the obligation to an
  object that the caller must construct. `CycleGuard` is that object. A sentence that the caller may
  never read is not enough. This is the weakest enforcement in the file, so keep it inside the
  real-time surface.
- Treat "the tests pass" as evidence about behaviour, not as evidence about synchronisation.

## Accepted trade-offs

Each item below is known and deliberate. None is a defect.

- **`serializeDump` holds `deviceSetMutex_` shared for the whole dump.** The dump covers up to
  `recorderCapacity` records, 300 000 by default, and tens of megabytes. `scan`, `reset`, and every
  re-map block for that whole time. `readRecord` takes the lock per record to avoid exactly this, so
  the two policies diverge on purpose. Nothing depends on an alignment of the two. A dump is a
  deliberate user action, and the user already waits for its latency. The fix, if that changes, is
  to chunk the dump. Take the lock per record. Tolerate a re-map that ends the span early, exactly
  as a lapped cursor already does.
- **`SoemFieldbusDriver::exchangeProcessData` reads `ctx_` lock-free**, while `closeContext` writes
  it under `controlPlaneMutex_`. This is correct only because `stopExchange()` orders the two
  upstream. The dependency is easy to miss, so this list names it. The alternative is a lock on the
  real-time path, which is worse.
- **A cyclic task can drive hardware that a rescan renumbered.** After an insertion, `findDevice(4)`
  resolves whatever now sits at position 4. `topologyGeneration()` is the mechanism to notice it,
  and the declaration of `findDevice` documents it. Nothing enforces the check. A Tier-3 program
  knows the machine that someone wrote it for, and to insert a node is a commissioning act. After
  it, rescan and restart.
- **Two concurrent `POST /api/init` requests can both observe `initialised() == false`** before
  either one enters `init()`, which re-checks under the lock. The second request gets a 500 instead
  of the intended 409.
- **`GameLoop::health()` can divide a `sumExecNs_` from a new epoch by an `executedCycles_` from the
  old epoch**, around a call to `setPeriod`. These are diagnostics only, and the value corrects
  itself within one cycle.
- **A batch output stage can straddle two real-time cycles**, with a skew of up to 1 ms.
  `stageProcessDataOutputs` stores the cells one after another, on a non-real-time thread. A fix
  needs a generation gate on the real-time side, which breaks the lock-free output path.

## Checklist when you add a lock

1. Does the real-time path reach it? If yes, stop. Find a lock-free protocol instead. The real-time
   path includes every user `CyclicTask`, so "does a cyclic task call this?" is part of the
   question.
2. Where does it sit in the [order](#lock-order)? A new subsystem lock goes *above* the
   `DeviceManager` chain. Release it before every `DeviceManager` call.
3. Does a caller hold it across bus input or output? If yes, say so in the comment on its
   declaration. Justify the duration there.
4. Which threads take it? Name them. "The HTTP thread" is not an answer, because there are 32 worker
   threads.
5. Does it hand out a pointer or a reference to something that it protects? If yes, return a copy,
   or lend access under the lock with the `withDevice` pattern. There is one sanctioned alternative,
   the real-time one. Use a raw pointer whose lifetime the cycle gate holds. Document it at the
   declaration. It is valid only inside a `CycleGuard`.
6. Add a row to the [inventory](#mutex-inventory). Add a paragraph under
   [the mutexes in detail](#the-mutexes-in-detail).
