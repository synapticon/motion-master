# Locking and Synchronisation

> **Hand-maintained.** The source code is the authority. When you add a synchronisation
> primitive, remove one, or change its scope, update this file in the same commit.
>
> Companion documents: [THREADS.md](THREADS.md) lists the threads and the work of each one.
> [CLASS_DIAGRAM.md](CLASS_DIAGRAM.md) shows ownership. [RT_SCHEDULING.md](RT_SCHEDULING.md)
> covers `SCHED_FIFO` and `mlockall`.

Motion Master holds ten mutexes, two condition variables, and four lock-free protocols. The page
below is the whole model. The sections after it are the detail behind it: every primitive, what each
one guards, and the invariants they hold up.

## The model on one page

**Who owns what.**

```text
DeviceManager
 ├── shared_ptr<DeviceSet>          currentSet_       the live generation, and its only owner
 ├── atomic<DeviceSet*>             publishedSet_     the live generation, for the RT thread
 └── unique_ptr<ProcessData>        pd_               created once, never replaced
      ├── atomic<const ProcessImage*>          image        the live layout
      ├── vector<shared_ptr<const ProcessImage>> generations every layout since the last scan
      ├── ProcessDataRing                      ring         one record per cycle
      └── atomic<int>                          inCycle      how deep the RT thread is in a cycle

DeviceSet — immutable once published
 ├── shared_ptr<FieldbusDriver>  driver
 └── vector<Device>              devices
      └── Device
           ├── deque<DeviceParameter>             cells_       the values. Never erased
           └── map<key, DeviceParameter*>         parameters_  replaced by each enumeration
```

**Four rules.**

1. **Published, then immutable.** A device set, a process image and a parameter cell are published
   once and never modified afterwards. A change publishes a new one. While you still hold the old
   one it is *valid but no longer fed*: reads serve the last values, writes reach no wire.
2. **Off the real-time thread, hold a refcount, never a lock.** `deviceAt()` returns a
   `DeviceHandle`; `deviceSet()` returns the set. Either keeps its devices alive for as long as you
   hold it. Nothing waits for you, and no rescan can invalidate you.
3. **The real-time thread holds neither.** `GameLoop` enters the cycle before it calls any task, and
   every operation that frees something drains the cycle first. That is the whole real-time
   contract.
4. **Freeing waits for two things, and nothing longer.** A retired set goes away when the last
   `DeviceHandle` releases it *and* the cycle has been drained. Memory does not grow with rescans,
   and the price is rule 2's other half: a cyclic task resolves its devices each cycle and caches
   nothing across cycles.

**Who frees what.**

| Operation | Frees | Safe because |
| --- | --- | --- |
| a re-enumeration | nothing. A cell is reused or added, never erased | — |
| a re-map | the ring storage, re-allocated for the new layout | `stopExchange()` drains the cycle first |
| `scan()` | the retired device set and its cells once the last handle releases them, the retained images, the ring storage | the same drain, so no task is inside a cycle holding a pointer |
| `reset()` | the same, and it stops the driver | the same drain |
| `~DeviceManager` | everything | the loop has stopped |

**The locks, one line each.**

| Lock | Covers | Longest hold |
| --- | --- | --- |
| `busOperationMutex_` | one control-plane operation drives the bus at a time | a whole scan or AL transition — seconds |
| `Device::parametersMutex_` | the parameter map, and a cell's non-atomic fields | never across bus I/O |
| `FieldbusDriver::controlPlaneMutex_` | one socket transaction | one FoE transfer — seconds |
| `currentSetMutex_` | one `shared_ptr` copy | nanoseconds |
| `processDataMutex_` | the ring storage and the retained images | one `.mmpd` dump |
| monitoring, refresher, procedure | each subsystem's own registry | never across a `DeviceManager` call |

Lock order is `busOperationMutex_` → `Device::parametersMutex_` →
`FieldbusDriver::controlPlaneMutex_`. The bottom three rows are leaves: nothing is ever acquired
while one of them is held. The [inventory](#mutex-inventory) below lists all ten, including the
per-run procedure locks and the log sink that this summary folds together.

To find every primitive in the source, run:

```bash
grep -rn 'std::mutex\|shared_mutex\|condition_variable\|std::atomic' libs apps
```

## Contents

- [Terms](#terms)
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
| `DeviceSet` | One published generation of the bus: the driver, the devices, and the topology generation. Immutable once published. |
| `DeviceHandle` | A device plus a `shared_ptr` to the set that holds it alive. What `deviceAt` returns. |
| Procedure | Off-real-time command-and-wait work on one device, such as offset detection. |

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
| 2 | `DeviceManager::currentSetMutex_` | `std::mutex` | `libs/node/device_manager.h` | the `shared_ptr<DeviceSet>` itself, and nothing it points at | `deviceAt`, `deviceSet`, and `publishDeviceSet` | **no**. One pointer copy |
| 3 | `DeviceManager::processDataMutex_` | `std::shared_mutex` | `libs/node/device_manager.h` | `pd_->ring`'s storage and `pd_->generations` | exclusive: `allocate` and `clear`, in the re-map publish window and in `scan`/`reset`. shared: the recorder accessors, `processImageInfo`, `serializeDump` | **no** |
| 4 | `Device::parametersMutex_` | `std::mutex`, held through a `unique_ptr` | `libs/node/device.h` | the *structure* of `parameters_`, the non-atomic fields of an entry, and `caSupport_`. **Not** the atomic cell | every control-plane parameter read or write. **Not** `value<T>()` or `setValue<T>()` | **no**. It is released for every transfer |
| 5 | `FieldbusDriver::controlPlaneMutex_` | `std::mutex` | `libs/comm/fieldbus_driver.h` | the SOEM context `ctx_` and the whole control plane: SDO, FoE, ESC registers, SII, AL state | every driver method except `exchangeProcessData` and `slaveState` | **yes**, for one transaction. FoE is the exception and holds it for the whole transfer |
| 6 | `MonitoringManager::mutex_` and `cv_` | `std::mutex`, `std::condition_variable` | `libs/node/monitoring_manager.h` | `entries_`, `running_`, `nextEpoch_`, `publish_` | the sampler thread, the `/api/monitorings` handlers, `keepFresh`, `stopKeepingFresh` | **no**. It is dropped for the flush |
| 7 | `ParameterRefresher::mutex_` and `cv_` | `std::mutex`, `std::condition_variable` | `libs/node/parameter_refresher.h` | `entries_`, `running_` | the refresher thread, and `acquire` or `release` from the sampler path | **no**. It is dropped for the poll |
| 8 | `ProcedureManager::mutex_` | `std::mutex` | `libs/node/procedure_manager.h` | `runs_` | `start`, `snapshot`, `cancel`, and the destructor | no |
| 9 | `ProcedureManager::Run::errorMutex_` | `std::mutex` | `libs/node/procedure_manager.h` | one `std::optional<std::string>` | the thread of the run, which writes. Pollers read | no |
| 10 | `ProgressReporter::mutex_` | `std::mutex` | `libs/node/procedure.h` | the step array | the thread of the run, which writes. Pollers read | no |
| — | `RingLogSink`, through the spdlog `base_sink::mutex_` | `std::mutex` | `apps/motion_master/ring_log_sink.h` | the log ring buffer | every thread that logs | no |
| — | single-instance lock | `flock`, or a named mutex on Windows | `libs/core/platform.cc` | the *process*, not a data structure | startup only | not applicable |

Entries 9 and 10 exist for one reason. **A procedure thread that finishes must never need
`ProcedureManager::mutex_`.** The destructor collects the threads that still run under that lock,
then releases the lock and joins them. So everything such a thread writes on its way out is an
atomic, or it has a per-run mutex of its own.

## Lock order

```text
busOperationMutex_
      ↓
Device::parametersMutex_
      ↓
FieldbusDriver::controlPlaneMutex_
```

`libs/node/device_manager.h` declares this order. `libs/node/device.h` restates it.

**Two mutexes are outside the chain, because they are leaves.** `currentSetMutex_` and
`processDataMutex_` are never held while anything else is acquired. Nothing orders against them.

**No real-time primitive appears in the chain, by design.** `CycleGuard` is not a mutex, and no
order relates it to these three. A control-plane operation waits *for* it, through
`ProcessData::pauseCycle`. It never acquires it. For that reason a `CycleGuard` held across a
control-plane call deadlocks that call against its own drain. A cyclic task makes no such call.

The three subsystem mutexes (6, 7, and 8) sit **above** this chain. `MonitoringManager::mutex_` is
released before any `DeviceManager` call, and `ParameterRefresher::mutex_` likewise.
`ProcedureManager` resolves the device *before* it takes its own lock. No thread holds one of the
three while it acquires a `DeviceManager` lock, so none of them can join a cycle. To keep that
true is the most important rule for anyone who extends those classes.

**A procedure body holds no lock, so it may call anything.** It holds a `DeviceHandle`, which is a
refcount. `transitionToState` from inside a body is therefore ordinary, which is why there is one
body shape rather than two (`libs/node/procedure_manager.h`).

## The mutexes in detail

### 1. `DeviceManager::busOperationMutex_` — one control-plane operation at a time

*Type:* plain `std::mutex`. *Guards:* no member at all.

This is a mutual-exclusion token over an **activity**: only one thing drives the bus through a
multi-step operation at a time. It separates a whole `scan` from the individual socket
transactions inside it, which `controlPlaneMutex_` serialises on its own.

**Holding it is what keeps the published set still.** Only a holder can publish a new `DeviceSet`,
so an operation that holds it can read `devices` and `driver` from the current set with no further
synchronisation. `remapProcessImage`, `updateExpectedWkc`, and the body of `transitionToState` all
rely on that.

**It blocks no reader.** `transitionToState` takes *only* this lock, so its AL wait of several
seconds delays nothing else. The monitoring sampler keeps its flush cadence, and every
`/api/monitorings` endpoint answers throughout. `configureProcessData` is the same: the IOmap
rebuild and the per-device PDO-mapping SDO reads run under this lock alone.

**The trade is deliberate: holding a device does not exclude an AL transition or a rescan.** A long
procedure and a user-driven state change interleave. The next bus transaction of the procedure fails
against the changed state. The alternative is worse: the HTTP request for the transition would wait
for the whole duration of the procedure.

### 2. The device set — lifetime without a lock

*Type:* `std::shared_ptr<DeviceSet>`, plus a `std::mutex` (`currentSetMutex_`) that guards the
pointer, and a `std::atomic<DeviceSet*>` (`publishedSet_`) that the real-time thread reads.

A `DeviceSet` holds the driver, the devices, and the topology generation it was published under.
`init` and `scan` build a new one and publish it. **Nothing modifies a set after publication.**

Off the real-time thread there are two ways in, and both are refcounts:

- `deviceAt(position)` returns a `DeviceHandle`: the device, plus a `shared_ptr` to its set.
- `deviceSet()` returns the `shared_ptr` itself, for work that spans several devices.

Each copies the pointer under `currentSetMutex_`, which is held for that copy alone. After that the
caller holds no lock, for a millisecond or for ten minutes. `std::atomic<std::shared_ptr<T>>` would
remove even that mutex, but it is not lock-free, and libc++ does not implement it.

**A rescan neither waits for a holder nor invalidates one.** `scan` publishes a new set; the old one
is freed when its last holder releases it. While a holder still has it, that device is *valid but no
longer fed*: reads serve its last values, `exchangesProcessData()` is false, and a write reaches no
wire. A procedure that was running keeps
working against its own device. Because the retired set also owns the driver, that device's next
transfer reaches a live driver object on a bus that has moved on, and fails as an error rather than
a crash.

**The real-time thread reads `publishedSet_`, not the `shared_ptr`.** The control plane replaces
that raw pointer only with the cycle drained (`ProcessData::pauseCycle`), so a cyclic task inside a
[`CycleGuard`](#the-cycle-gate) can dereference it for the whole body. The set it names needs no
reference of its own: `currentSet_` owns the object it names, and `publishDeviceSet` writes both
with the cycle drained. The two routes split by caller:

| Caller | Route | What holds the device still |
| --- | --- | --- |
| control plane: HTTP worker, procedure, sampler | `deviceAt`, `deviceSet`, or a position-based method | a `shared_ptr` to the set. No lock |
| real-time cyclic task | `findDevice`, inside the cycle `GameLoop` has entered | the published set pointer and the `inCycle` drain. No lock |

Anything else is a defect. A raw `findDevice` off the real-time thread resolves a `Device*` with
nothing holding its set alive. That code compiles, so the warning lives on the declaration, and this
table repeats it.

### 3. `DeviceManager::processDataMutex_` — the recorder storage and the retained images

*Type:* `std::shared_mutex`.

`recorderHead`, `recorderOldestSeq`, and `readRecord` take it shared, although `ProcessDataRing`
is lock-free. The adversary is *not* the real-time producer, because the per-slot sequence
re-check of the ring handles that case. The adversary is `allocate` or `clear`. Both **release the
storage**, and both run on the control plane. A read of the ring without this lock is a
use-after-free, not a torn read. The window is wide, because one sampler flush walks thousands of
records.

`pd_->generations` has the same shape: a re-map appends to it, and `scan` and `reset` clear it, so
`processImageInfo` and `serializeDump` read it under the same lock.

`readRecord` takes the lock **per record, not per span**. Otherwise a sampler flush that walks
thousands of records becomes a shared holder for milliseconds, and the exclusive `clear` inside a
`scan` queues behind it. A span is not atomic in any case. Each record describes itself, and a
lapped record returns `false`.

### 4. `Device::parametersMutex_` — the parameter map, never across a transfer

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
for every access to a non-atomic field. A cyclic task runs inside the cycle the loop entered and
touches only the cell.

### 5. `FieldbusDriver::controlPlaneMutex_` — one socket transaction

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
between transactions means that `ctx_` is not stable for the duration of the operation. A `stop`
that lands in a gap frees `ctx_`, and the next transaction dereferences a freed context. Inside
Motion Master this cannot happen: a caller of a multi-transaction operation holds a `DeviceHandle`,
whose set owns the driver, so the driver object outlives the operation even across a concurrent
`scan` or `reset`. A `reset` stops the bus, so the next transaction fails — which is an error, not a
crash. **An embedder that drives `SoemFieldbusDriver` directly must keep the driver alive the same
way.**

**FoE is the one long hold.** `readFile` and `writeFile` hold the lock for the entire transfer.
That is the reason that the [AL-state mirror](#the-al-state-mirror) exists. Without the mirror,
every reader that asks for the state of device 3 waits out a firmware write of 12 seconds.

### 6 and 7. `MonitoringManager::mutex_` and `ParameterRefresher::mutex_`

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

### 8 to 10. The procedure locks

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
makes the thread destroy its own jthread as it exits, and join itself. The case is reachable for
every run, because no run holds a lock. A `scan()` can land in the middle of one.

## Lock-free protocols

The real-time loop takes no lock, so no mutex can protect what it touches. Four protocols do that
job:

- **The cycle gate** stops the control plane from freeing the process image, a device, or a
  parameter map while the real-time thread uses it.
- **The parameter cell** is the single home of a scalar value. Any thread reads it or writes it with
  one atomic access.
- **The recorder ring** carries one record per cycle from the real-time thread to the monitoring
  readers and to the dump.
- **The AL-state mirror** reports the state of a device without the driver lock, which a firmware
  transfer holds for several seconds.

Each one is a *protocol*, not merely an atomic. The memory ordering is the mechanism.

### The cycle gate

`ProcessData::image` and `ProcessData::inCycle` (`libs/node/process_data.h`) are the **only** thing
between the real-time thread and a control-plane teardown. The handshake is one store-load pair on
each side:

- **Real-time side.** Raise `inCycle` **before** the load of the image. Then load the image. Back
  out if it is null. The code does this at two levels: `exchangeProcessData` around the exchange
  itself, and `CycleGuard` one level up, taken by `GameLoop` around the whole task list. That is the
  reason that
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

**The pause protects more than the IOmap.** A cyclic task, inside the cycle the loop entered,
resolves devices through `publishedSet_` and dereferences `DeviceParameter` objects. So the same
drain is what makes two other changes safe: the swap of `publishedSet_` in `scan` and `reset`, and
the replacement of a parameter map in `initializeParameters`. Neither needs a lock that the
real-time thread would also have to take.

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
them safe. The owner must exclude readers around them, and `processDataMutex_` is what does that.

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
   `CycleGuard`, of `GameLoop::run`, and of everything a registered `CyclicTask` calls.
   `Device::value<T>()` and `Device::setValue<T>()` are the only value access permitted there.
2. **No subsystem mutex is held during a call to a `DeviceManager` method.** This covers the
   monitoring, refresher, and procedure mutexes. It is what keeps the lock graph acyclic.
3. **No caller holds `parametersMutex_` across a bus transfer.**
4. **A `DeviceParameter*` never crosses a release of `parametersMutex_`** on the control plane.
   There are two sanctioned exceptions, and both substitute the cycle gate for the mutex.
   `ProcessImageEntry::parameter` is one: every re-map rebuilds it, and every change that
   invalidates it pauses the cycle. The per-cycle lookup of a cyclic task is the other: it holds
   inside one cycle, never across cycles.
5. **A published `DeviceSet` is never modified**, and it lives exactly as long as its holders.
   `init` and `scan` build a new one and publish it. A caller holding a handle on a retired set may
   read it and may drive its device; the transfer fails on the bus rather than in memory. On the
   real-time thread the drain takes the place of the handle, so a task must not cache a pointer
   across cycles.
6. **Every reader of `pd_->ring` or `pd_->generations` holds `processDataMutex_` shared.**
7. **`pauseCycle()`, through `stopExchange()`, precedes every mutation of these: the IOmap, the ring
   storage, `publishedSet_`, and the `parameters_` map of a device.** A cyclic task reads the last
   two, so the pause is not only about the IOmap.
8. **`generations` retains every published `ProcessImage` until `reset()`.**
9. **`GameLoop` holds a `CycleGuard` around every call to `CyclicTask::execute`, and calls no task
   when the guard is falsy.** On the real-time thread, no `findDevice` result, no `findParameter`
   result, and no cell access is valid outside one. A task therefore takes none itself.
10. **No code holds a `CycleGuard` across a control-plane call.** The drain of that call would wait
    on the guard forever.
11. **A writer changes the non-atomic fields of an entry only under `parametersMutex_` *and* with
    the real-time cycle paused.** The fields are `dataType`, `bitLength`, `rawValue`, `syncState`,
    and `name`. The real-time decode reads `dataType` and `bitLength` without a lock.
12. **Every read surface of `DeviceManager` works from a snapshot.** That includes `busConfig`,
    `initialised`, `deviceStates`, `deviceDiagnostics`, `dcSync`, and `processDataWatchdog`: each
    takes a `shared_ptr` to the current set and reads that, so none of them can observe a set being
    built. The real-time exceptions read `publishedSet_`, guarded by the cycle gate instead.
13. **`publishedSet_` is replaced only with the real-time cycle drained**, by `publishDeviceSet`,
    whose callers hold `busOperationMutex_` and have called `stopExchange`.

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
  object that the caller must construct — or, better still, to a caller that cannot forget.
  `CycleGuard` is that object, and `GameLoop` is that caller: it enters the cycle around every task,
  so a Tier-3 author cannot omit it. A sentence that the caller may never read is not enough.
- Treat "the tests pass" as evidence about behaviour, not as evidence about synchronisation.

## Accepted trade-offs

Each item below is known and deliberate. None is a defect.

- **`serializeDump` holds `processDataMutex_` shared for the whole dump.** The dump covers up to
  `recorderCapacity` records, 300 000 by default, and tens of megabytes. Every re-map, `scan`, and
  `reset` blocks for that whole time, because each one clears or re-allocates the ring. `readRecord`
  takes the lock per record to avoid exactly this, so the two policies diverge on purpose. Nothing
  depends on an alignment of the two. A dump is a deliberate user action, and the user already waits
  for its latency. The fix, if that changes, is to chunk the dump. Take the lock per record.
  Tolerate a re-map that ends the span early, exactly as a lapped cursor already does.
- **A retired `DeviceSet` lives until its last holder drops it.** A long procedure therefore keeps a
  full device set — devices, parameter maps, and the driver — alive after a rescan has replaced it.
  The memory is transient rather than retained, and the alternative is a rescan that waits minutes
  for a procedure to finish.
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
5. Does it hand out a pointer or a reference to something that it protects? If yes, prefer a
   refcount to a lock: hand back a handle that owns what it points at, as `DeviceHandle` does.
   Otherwise return a copy. There is one sanctioned alternative, the real-time one: a raw pointer
   whose lifetime the cycle gate holds, documented at the declaration and valid only inside a
   `CycleGuard`.
6. Add a row to the [inventory](#mutex-inventory). Add a paragraph under
   [the mutexes in detail](#the-mutexes-in-detail).
