# Real-Time Scheduling Primer

> Background reference for the Linux RT primitives Motion Master relies on to run its
> 1 ms game loop. This is general OS knowledge scoped to how *this* project uses it — see
> [THREADS.md](THREADS.md) for where these primitives are actually applied (thread 1, the
> RT loop).

The hard-RT recipe used by the game loop (`game_loop.cc`) and the `jitter_bench` HIL tool
is three things together:

1. **`SCHED_FIFO`** — guarantees the RT thread gets the CPU *when* it wants it.
2. **`mlockall`** — guarantees that once it's running, a page fault won't stall it.
3. **`clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)`** — wakes it on an absolute,
   drift-free schedule and yields the CPU between cycles.

Each is necessary; none is sufficient alone.

## `SCHED_FIFO` — real-time scheduling policy

Linux exposes real-time scheduling policies that sit **above** the normal fair scheduler
(`SCHED_OTHER` / CFS). Any runnable RT thread preempts every normal thread.

```c
struct sched_param param = { .sched_priority = 80 };
sched_setscheduler(0, SCHED_FIFO, &param);   // or pthread_setschedparam()
```

`SCHED_FIFO` ("first in, first out"):

- **No time slicing.** A running `SCHED_FIFO` thread runs until it *voluntarily* blocks
  (sleep, I/O, mutex wait), yields, or is preempted by a **higher-priority** RT thread.
  It is never preempted by an equal- or lower-priority thread.
- **Strict priority order.** Higher number wins. Same-priority threads run in FIFO order —
  the one that became runnable first runs to completion before the next.
- Contrast `SCHED_RR` ("round robin"): identical *except* same-priority threads share a
  time quantum.

### Priority range

| Policy | Priority axis | Range |
|---|---|---|
| `SCHED_FIFO`, `SCHED_RR` | `sched_priority` | **1 – 99** (1 lowest, 99 highest) |
| `SCHED_OTHER` (normal/CFS) | always RT priority **0**; tune with **nice** | nice −20 … +19 |

Query the bounds instead of hardcoding:

```c
int lo = sched_get_priority_min(SCHED_FIFO);  // → 1
int hi = sched_get_priority_max(SCHED_FIFO);  // → 99
```

Any RT thread (1–99) outranks every `SCHED_OTHER` thread (priority 0). Internally the
kernel maps 1–99 onto its own inverted scale, but userspace always uses 1–99.

### Why 80, not 99

Critical kernel threads (migration, watchdog, some IRQ threads under `PREEMPT_RT`) run at
high RT priorities. Sitting at 99 can starve them and destabilize the system. Motion Master
uses **80** for the game loop and `jitter_bench`: high enough to preempt all normal work,
low enough to stay under the essential kernel threads.

### Hazards

- A runaway `SCHED_FIFO` thread with no blocking call **can hang the machine** — it
  preempts everything below it and never time-slices, starving normal threads (including
  your shell). This is why the RT loop must always sleep to the next cycle; the
  `clock_nanosleep` is what yields the CPU.
- Linux's `sched_rt_runtime_us` throttle (default 950 ms per 1 s) is a safety valve against
  exactly this.

## `mlockall` — pin memory into RAM

```c
#include <sys/mman.h>
int mlockall(int flags);        // typically MCL_CURRENT | MCL_FUTURE
```

Locks the process's virtual memory pages into physical RAM so the kernel can never page
(swap) them out.

| Flag | Effect |
|---|---|
| `MCL_CURRENT` | lock all pages currently mapped |
| `MCL_FUTURE` | lock all pages mapped in the future (heap growth, new mmaps, growing stack) |
| `MCL_ONFAULT` | (with one of the above) lock lazily as pages fault in, rather than pre-faulting everything |

Undo with `munlockall()`; lock a single range with `mlock()` / `munlock()`.

### Why it matters for RT

If a page belonging to the 1 ms loop (code, stack, process-data buffers) has been swapped
out, the next access triggers a **page fault** — potentially milliseconds of disk I/O,
which blows the cycle deadline and injects jitter. Fatal for a hard-RT EtherCAT loop.
`mlockall` guarantees every page stays resident, so no page fault ever stalls the RT thread.

This is also why the recorder ring (~120 MB, `ProcessDataRing`) is pre-allocated **and**
`mlock`'d at `configureProcessData`, off the RT path — so the RT loop never faults on it.

## Capabilities & limits

On bare metal `setcap` stamps the binary; inside Docker use `--cap-add` (file caps are
ignored in containers). See the capability table in [../CLAUDE.md](../CLAUDE.md).

| Need | Capability | Notes |
|---|---|---|
| Set an RT scheduling policy | `CAP_SYS_NICE` | for `SCHED_FIFO` |
| Lock process memory | `CAP_IPC_LOCK` | plus `--ulimit memlock=-1` in Docker to lift `RLIMIT_MEMLOCK` |

If the RT capabilities are missing, Motion Master **warns and runs non-RT** rather than
failing — the loop still starts, just without scheduling/memory guarantees.

## See also

- [THREADS.md](THREADS.md) — the five threads; thread 1 is the RT loop that uses all of the above.
- `hil/jitter_bench/` — measures cycle jitter under `SCHED_FIFO` prio 80 + `mlockall`.
- `libs/core/cyclic_timer_linux.cc` — the `clock_nanosleep` absolute-time cycle timer.
