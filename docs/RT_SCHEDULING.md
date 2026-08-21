# Real-Time Scheduling Primer

> Background reference for the Linux real-time primitives that the 1 ms game loop depends on.
> This is general operating-system knowledge, scoped to how this project uses it. See
> [THREADS.md](THREADS.md) for where each primitive is applied.

Motion Master runs one real-time thread. The main thread becomes it, and it runs the game loop
(`apps/motion_master/game_loop.cc`). The `jitter_bench` tool in `hil/` calls the same setup
routine, so it measures what ships.

Three primitives are mandatory. Each one is necessary, and none is sufficient alone. A fourth is
optional, because it describes the deployment rather than the code.

1. **`SCHED_FIFO`** — the thread gets the processor at the moment it wants it.
2. **`mlockall`** — a page fault cannot stall the thread once it runs.
3. **`clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)`** — the thread wakes on an absolute
   schedule, and it yields the processor between cycles.
4. **`sched_setaffinity`** (optional) — the thread runs on one core, and the kernel can reserve
   that core for it.

`mm::core::setRealtimePriority()` (`libs/core/realtime.h`) applies primitives 1, 2 and 4.
`mm::core::CyclicTimer` (`libs/core/cyclic_timer.h`) applies primitive 3.

## `SCHED_FIFO` — real-time scheduling policy

Linux offers real-time scheduling policies that sit **above** the normal fair scheduler
(`SCHED_OTHER`, also called CFS). Any runnable real-time thread preempts every normal thread.

```c
struct sched_param param = { .sched_priority = 80 };
pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
```

`SCHED_FIFO` means "first in, first out". It has two properties:

- **No time slicing.** A running `SCHED_FIFO` thread keeps the processor until one of three
  things happens. It blocks by itself, through a sleep, an I/O call or a mutex wait. It yields.
  Or a real-time thread of a **higher** priority preempts it. A thread of an equal or a lower
  priority never preempts it.
- **Strict priority order.** The higher number wins. Threads of one priority run in first-in,
  first-out order. The thread that became runnable first runs to completion before the next one
  starts.

`SCHED_RR` means "round robin". It is identical, except that threads of one priority share a
time quantum.

### Priority range

| Policy | Priority axis | Range |
| --- | --- | --- |
| `SCHED_FIFO`, `SCHED_RR` | `sched_priority` | **1 to 99**. 1 is the lowest, 99 the highest |
| `SCHED_OTHER` (normal, CFS) | always real-time priority **0**. Tune it with **nice** | nice −20 to +19 |

Query the bounds. Do not hardcode them:

```c
int lo = sched_get_priority_min(SCHED_FIFO);  // 1
int hi = sched_get_priority_max(SCHED_FIFO);  // 99
```

Every real-time thread, at any priority from 1 to 99, outranks every `SCHED_OTHER` thread.
The kernel maps 1 to 99 onto an inverted internal scale, but user space always uses 1 to 99.

### Why priority 80

Motion Master requests **80** for the game loop and for `jitter_bench`
(`mm::core::kRtThreadPriority`). The number sits between two bands:

- The kernel's own migration and watchdog threads occupy the band above 90. A thread at 99 can
  starve them, and that destabilises the machine.
- `CONFIG_PREEMPT_RT` gives a threaded interrupt handler a priority near 50. Priority 80 is
  above that.

That second relation needs a check on each target. On a stock kernel a soft interrupt runs in
interrupt context, and it preempts a `SCHED_FIFO` thread at any priority. So the number cannot
starve the network interface. Under `PREEMPT_RT` this thread does outrank the interface's
interrupt thread. Confirm it against the target machine:

```bash
ps -eo pid,cls,rtprio,comm | grep irq
```

### A child process inherits the policy

A new process starts with the scheduling policy of the thread that spawned it. Motion Master
requests the policy with `SCHED_RESET_ON_FORK` where the platform defines that flag, which is
Linux only. A child then starts at `SCHED_OTHER` and nice 0, and the flag is cleared in the
child. The guarantee holds by construction.

One subsystem depends on this, and it does not depend on the flag alone. The auto-tuning
executable is a child process (`libs/auto_tuning/process.h`). **`main()` starts it before the
main thread becomes real-time.** The child spreads its work over one numerical worker thread per
core, and those threads synchronise by a spin-wait. At real-time priority each worker burns a
whole time slice in that spin-wait. Measured on an earlier integration, a seven-second
identification run took minutes, and it starved the 1 ms cycle for that whole time.

`mlockall(MCL_FUTURE)` is process-wide, and it survives a fork whatever the policy flag says. An
`exec` clears it.

### Hazards

- A runaway `SCHED_FIFO` thread with no blocking call **can hang the machine**. It preempts
  everything below it, and it never yields a time slice. Normal threads starve, and the shell is
  one of them. This is why the real-time loop must always sleep to the next cycle. The
  `clock_nanosleep` call is what yields the processor.
- Linux throttles real-time threads as a safety valve against exactly this. The default
  `sched_rt_runtime_us` allows 950 ms of real-time work in each second.

## `mlockall` — pin memory into RAM

```c
#include <sys/mman.h>
int mlockall(int flags);  // Motion Master passes MCL_CURRENT | MCL_FUTURE
```

The call locks the process's virtual memory pages into physical RAM. The kernel can then never
page them out to swap.

| Flag | Effect |
| --- | --- |
| `MCL_CURRENT` | Lock every page that is mapped now |
| `MCL_FUTURE` | Lock every page mapped later: heap growth, a new `mmap`, a growing stack |
| `MCL_ONFAULT` | Use it with one of the above. Lock each page as it faults in, rather than pre-fault everything |

`munlockall()` undoes the call. `mlock()` and `munlock()` lock one range.

### Why it matters

A page of the 1 ms loop can be paged out. Its code, its stack and its process-data buffers are
all candidates. The next access to a paged-out page triggers a **page fault**. A page fault can
cost milliseconds of disk I/O. That blows the cycle deadline and it injects jitter, which is
fatal for a hard real-time EtherCAT loop. `mlockall` keeps every page resident, so no page fault
ever stalls the real-time thread.

The recorder ring (`ProcessDataRing`) is a second reason. It is pre-allocated **and** locked at
`configureProcessData`, off the real-time path, so the loop never faults on it. The ring is
large. One drive costs roughly 128 bytes per cycle, so the default depth of 300 000 cycles is
about 38 MB. At a 1 ms period that holds about 5 minutes of history.

## `sched_setaffinity` — pin the thread to one core

```c
cpu_set_t cpus;
CPU_ZERO(&cpus);
CPU_SET(core, &cpus);
sched_setaffinity(0, sizeof(cpus), &cpus);  // 0 = the calling thread
```

Affinity restricts a thread to a set of cores. Motion Master pins the real-time thread to a
single core when the configuration names one:

```jsonc
"gameLoop": {
  "periodUs": 1000,
  "cpuAffinity": 3   // -1, the default, leaves the thread unpinned
}
```

The setting is fixed at startup. Unlike `periodUs`, it describes the deployment, so there is no
endpoint to retune it live. Linux only: macOS exposes no per-thread affinity call, and Windows
has no real-time path here.

### Why one core, and which one

Affinity pays for itself where the kernel booted with **`isolcpus`**. That parameter removes a
core from the scheduler's reach. Nothing runs on such a core unless a thread asks for it by
name. So an isolated core sits idle until something pins itself to it, and the isolation buys
nothing until then.

**Affinity is per thread, and that is the point.** Only the real-time thread moves. The HTTP,
WebSocket, monitoring and refresher threads stay on the housekeeping cores. Pinning the whole
process instead does the opposite: `taskset` and systemd's `CPUAffinity=` drag every non-real-time
thread onto the isolated core, where they contend with the loop. They also leave more than one
runnable task on that core, and that is what stops **`nohz_full`** from taking effect. A core
with one runnable task can stop its scheduling tick. A core with two cannot.

### What defeats it

The call needs **no privilege**. A thread may always set its own affinity. `CAP_SYS_NICE` is
required only to change the affinity of another process.

An outside restriction is what defeats it. A cgroup cpuset, a container, systemd's
`CPUAffinity=` or an inherited `taskset` all narrow the mask the process may use. A core outside
that mask then fails with `EINVAL`, whatever capabilities the process holds. A core that does
not exist fails the same way.

The step is best effort, like the other two. A failure is logged and the loop still runs:

```text
GameLoop: RT thread pinned to CPU 3
GameLoop: failed to pin the RT thread to CPU 3 — it will run on any non-isolated core
```

Nothing is logged when the configuration names no core. Unpinned is the default, not a fault.

## Capabilities and limits

On bare metal, `setcap` stamps the binary. Inside Docker, use `--cap-add`, because a container
ignores file capabilities. The full capability table is in [../CLAUDE.md](../CLAUDE.md).

| Need | Capability | Notes |
| --- | --- | --- |
| Set a real-time scheduling policy | `CAP_SYS_NICE` | For `SCHED_FIFO` |
| Lock process memory | `CAP_IPC_LOCK` | Add `--ulimit memlock=-1` in Docker to lift `RLIMIT_MEMLOCK` |
| Pin the thread to a core | none | A thread may always set its own affinity |

**The three steps are independent, and no failure is fatal.** A failed `SCHED_FIFO` skips
neither the `mlockall` nor the pinning. `setRealtimePriority` reports which steps took, and
`GameLoop::run()` warns about each step that did not. A process without `CAP_SYS_NICE` or
`CAP_IPC_LOCK` still runs. It runs without the timing guarantees.

`GET /api/game-loop` reports the outcome as `schedFifo`, `memLocked`, `cpuAffinity` and
`cpuPinned`, so a client can show what the host actually granted.

## See also

- [THREADS.md](THREADS.md) — every thread in the process. Thread 1 is the real-time loop that
  uses all of the above.
- [LOCKING.md](LOCKING.md) — why the real-time loop takes no lock.
- `libs/core/realtime.{h,cc}` — the routine that applies primitives 1, 2 and 4.
- `libs/core/cyclic_timer_linux.cc` — the absolute-deadline cycle timer.
- `hil/jitter_bench/` — measures cycle jitter under the same setup.
