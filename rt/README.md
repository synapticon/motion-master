# Real-Time Linux Setup

Everything needed to turn a base Debian 14 (Forky) install into a real-time Motion Master
appliance: Ansible roles that do the work, and a throwaway QEMU VM to develop and prove them
against.

One playbook covers every target — the VM, an x86 board on your desk, and a Raspberry Pi 5.
Nothing here is machine-specific except the inventory.

## Targets

| | AAeon E3940 | Raspberry Pi 5 | QEMU dev VM |
| --- | --- | --- | --- |
| Distro | Debian 14 Forky | Debian 14 Forky (arm64) | Debian 14 Forky |
| Kernel | `linux-image-rt-amd64` 7.1.3 | `linux-image-rt-arm64` 7.1.3 | `linux-image-rt-amd64` 7.1.3 |
| Cores | 4 (Goldmont Atom, 1.6 GHz) | 4 (Cortex-A76, 2.4 GHz) | 4 vCPU |
| Bootloader | GRUB | raspi-firmware | GRUB |
| Provisioned | over SSH | image build, then SSH | over SSH |

Same distro, same kernel version, same PREEMPT_RT, same roles. The VM is sized to match the boards
so an isolated-core split means the same thing in all three.

**Why Forky rather than stable.** Stable cannot boot a Raspberry Pi 5 — RP1, the southbridge
carrying the Pi's Ethernet and USB, only became usable upstream around kernel 6.18, and stable is
still on 6.12. Debian's own position is that "RPi 5 only works with Debian 14 'Forky' (testing) or
Sid". Forky also carries `linux-image-rt-{amd64,arm64}` **7.1.3 in main**, the same version on both
architectures, so the two boards run an identical kernel with nothing to pin. Forky is testing
today; the boards ship in 2027, by which time it is the obvious target.

### Comparing the two boards

Shared OS configuration is worth having, and it is what this directory delivers. It does **not**
make the two boards' latency numbers interchangeable, and reading them that way will mislead you:

- **The CPUs are far apart.** A Cortex-A76 at 2.4 GHz is several times the per-core throughput of a
  1.6 GHz Goldmont Atom. A cycle-time budget measured on one says little about the other.
- **The network paths differ in kind.** An E3940 board carries an Intel i210/i211-class controller
  with full `ethtool -C` coalescing control — the single biggest jitter knob on commodity hardware.
  The Pi 5's Gigabit MAC lives inside the RP1 southbridge, sharing one PCIe 2.0 x4 link with both
  USB 3 controllers, MIPI and SDIO. For an EtherCAT master, where the cycle is a blocking frame
  round-trip, that is the difference most likely to show up in the numbers.

What the shared configuration *does* buy: when the two disagree, the software is not the variable.

## Quick Start

Everything here needs QEMU and Ansible; `./tools/install-deps.sh` installs both (and the rest of
the repository's dependencies) on Debian/Ubuntu or Fedora.

### Against the throwaway VM

```bash
cd rt/vm
./fetch-base.sh     # once: download + verify the Debian 14 cloud image (~400 MB)
./create.sh         # SSH keypair, cloud-init seed, copy-on-write overlay
./start.sh          # boots and waits for SSH — about 10 seconds
./provision.sh      # run the RT playbook
./ssh.sh            # shell in and look around
```

`./provision.sh` passes any extra arguments to `ansible-playbook`:

```bash
./provision.sh --check              # dry run
./provision.sh --tags rt-boot       # one role
./provision.sh --tags benchmark     # cyclictest (see the caveat below)
```

To start over from a pristine machine — the thing you will do most often:

```bash
./reset.sh          # discard the overlay and boot a clean Debian 14, ~15 seconds
```

The VM is loopback-only (SSH on `127.0.0.1:2222`), capped at 4 vCPU / 4 GB, and entirely
disposable: `.cache/` holds the base image, overlay, keypair and console log, and is gitignored.
Console output goes to `rt/vm/.cache/serial.log` — `tail -f` it when a boot goes wrong.

### Against real hardware

```bash
cd rt/provision/ansible/inventory
cp lab.yml.example lab.yml          # edit: address, user, cores, EtherCAT NIC, daemon
cd ..
../play-rt.sh inventory/lab.yml
```

`lab.yml` is gitignored. The target needs SSH key access and sudo; add `--ask-become-pass` if sudo
is not passwordless there. Set `motion_master_install: true` there to install the daemon as well as
the real-time OS — left off, you get a bare real-time host.

This is also the path for a Raspberry Pi **once it has been flashed** — a provisioned card is an
ordinary `rt_targets` host. Getting to that card is the one thing that differs; see
[Raspberry Pi 5](#raspberry-pi-5).

### Provisioning a development or CI machine

Unrelated to real-time — the C++ toolchain, Docker, and the GitHub Actions runner:

```bash
rt/provision/play-ci.sh
```

`bootstrap.sh` is the one-shot version of that for a fresh machine: it installs git and Ansible,
clones the repo, and runs `play-ci.sh`.

## Layout

```text
rt/
  vm/                        throwaway Debian 14 QEMU VM (plain qemu, no libvirt)
    common.sh                shared config — every value overridable from the environment
    fetch-base.sh            download + verify the Debian 14 cloud image
    create.sh                keypair, cloud-init seed ISO, qcow2 overlay
    start.sh stop.sh ssh.sh  lifecycle
    reset.sh                 back to a pristine machine in seconds
    provision.sh             run the RT playbook against it
  provision/
    bootstrap.sh             fresh CI machine: install ansible, clone, provision
    play-ci.sh               provision THIS machine as a dev/CI machine
    play-rt.sh               provision a real-time target (takes an inventory)
    ansible/
      inventory/
        ci.yml               localhost                        [ci_machines]
        qemu.yml             the VM in rt/vm                  [rt_targets]
        rpi-image.yml        the Pi image mid-build           [rt_targets]
        lab.yml.example      template for real hardware       [rt_targets]
      ci-machine.yml         dev-toolchain + actions-runner
      rt-target.yml          the real-time roles
      site.yml               both, selected by inventory
      roles/
        dev-toolchain/       C++ toolchain, Docker, Node — not real-time
        actions-runner/      GitHub Actions runner registration
        rt-kernel/           PREEMPT_RT kernel
        rt-boot/             kernel command line (GRUB or raspi-firmware)
        rt-limits/           rtprio / memlock privileges
        rt-tuning/           irqbalance, CPU governor, NIC offload
        rt-verify/           assertions, summary, cyclictest
        motion-master/       release .deb, config, systemd unit
  image/
    build-rpi-image.sh       flashable Raspberry Pi 5 image (arm64, slow)
```

Inventory groups are what keep the two concerns apart: a play whose group has no hosts in the
chosen inventory is simply skipped, so `site.yml` is safe to run against either.

## What PREEMPT_RT Actually Does

A stock kernel is fast *on average*; PREEMPT_RT is bounded in the *worst case*. That is the whole
trade, and it is why everything else in this directory exists.

On a normal kernel there are long stretches where the kernel cannot be preempted — a spinlock held,
an interrupt handler running, a softirq processing packets. When the real-time thread becomes
runnable it waits for those to finish, however high its priority. That wait is scheduling latency,
and its **maximum** is what decides whether a 1 ms deadline is met. Averages are irrelevant here:
one 3 ms stall a minute is a skipped cycle.

Four things change:

1. **Spinlocks become sleeping locks.** In mainline, holding a `spinlock_t` disables preemption.
   PREEMPT_RT converts most of them to rt-mutex-backed sleeping locks, so a task holding one can be
   preempted. Only `raw_spinlock_t` — scheduler core, low-level architecture code — stays truly
   non-preemptible.
2. **Interrupt handlers become threads.** Normally a hardware interrupt preempts everything at any
   priority. Under PREEMPT_RT most handlers run in kernel threads with real priorities (~50 by
   default), so a `SCHED_FIFO` 80 loop outranks the NIC. This is what makes
   `kRtThreadPriority = 80` meaningful at all — on a stock kernel the number cannot starve the NIC
   path, because hardirqs sit above every priority.
3. **Softirqs are threaded too** — NAPI network processing, timers, tasklets — so packet handling
   cannot silently preempt the cycle.
4. **Priority inheritance on kernel locks**, bounding priority inversion: a low-priority task
   holding a lock the RT thread needs temporarily inherits its priority, so it finishes and
   releases quickly.

Order of magnitude: worst-case wakeup latency drops from milliseconds to tens of microseconds. The
cost is throughput — more context switches, more lock overhead — and a slightly worse *average*.

### What it does not fix

Each of these is a separate latency source that PREEMPT_RT leaves untouched, which is exactly what
the rest of the roles are for:

| Problem | Addressed by |
| --- | --- |
| Page fault mid-cycle | `mlockall` (`rt-limits` grants `memlock`) |
| Deep C-state wakeup, tens of µs | `intel_idle.max_cstate=1` (`rt-boot`) |
| Other tasks stealing the core | `isolcpus` + `gameLoop.cpuAffinity` |
| Periodic tick on the real-time core | `nohz_full` (`rt-boot`) |
| Interrupts landing on the real-time core | `irqaffinity`, masking `irqbalance` (`rt-tuning`) |
| Frequency transitions | `performance` governor (`rt-tuning`) |
| SMIs — firmware interrupts invisible to the OS | nothing; a hardware/BIOS problem |

It also does nothing at all if the process never asks for real-time scheduling: a PREEMPT_RT kernel
running only `SCHED_OTHER` threads behaves like a slightly slower normal kernel.

**Why this matters for an EtherCAT master specifically.** The cycle is a blocking frame round-trip,
and in free-run distributed clocks the drives act on frame *arrival*. So jitter in when the loop
wakes becomes jitter in when the drive moves — there is no hardware pulse absorbing it. (Activating
DC SYNC0 would decouple the two; see NEXTGEN.md.)

One piece of history worth knowing, because it changes how you test for it: **since 6.12 PREEMPT_RT
is merged into mainline**, no longer an out-of-tree patch. That is why the RT kernel comes from the
ordinary archive, and why `/sys/kernel/realtime` — an artifact of the patch era — no longer exists.
`rt-verify` reads `PREEMPT_RT` out of `uname -v` instead.

## What the Playbook Does

Each role is one of the requirements for a deterministic cycle, and each is tagged with its own
name so it can be run alone.

### rt-kernel — the PREEMPT_RT kernel

Installs `linux-image-rt-<arch>` and the matching headers. Debian ships PREEMPT_RT in the archive,
so there is nothing to build or patch. The architecture comes from `dpkg --print-architecture`, so
the same role is correct on both boards.

The reboot is deferred to the end of the play, so a kernel install and a boot-parameter change cost
one restart between them rather than one each.

### rt-boot — the kernel command line

On GRUB machines, writes `/etc/default/grub.d/99-realtime.cfg`, a **drop-in** rather than an edit of
`/etc/default/grub`. `grub-mkconfig` sources `/etc/default/grub.d/*.cfg` after the base file, which
is also how the Debian cloud image ships its own settings — so the drop-in composes with them, is
idempotent by construction, and is undone by deleting one file.

On a Raspberry Pi there is no GRUB — but the principle is the same, and getting it wrong is easy.
The firmware reads `/boot/firmware/cmdline.txt` and `config.txt`, and the obvious move is to edit
them. **Don't.** `config.txt` opens with *"Do not modify this file! It is automatically generated
upon install or update of either the firmware or the Linux kernel"*, and `cmdline.txt` is
regenerated the same way — so an edit there survives exactly until the next kernel upgrade and then
disappears, taking the real-time configuration with it and leaving no obvious symptom beyond bad
latency.

`raspi-firmware` provides a source file for exactly this, and the role writes that instead:
`/etc/default/raspi-extra-cmdline`, holding the extra kernel parameters on one line with no
comments, followed by `update-initramfs -u -k all` to rebuild the generated files. Only our
parameters go in it — `root=`, `console=` and the rest are generated, so there is nothing to
preserve and no merge to get wrong.

That is the *only* file the role touches there. In particular it does not set `KERNEL=`, which looks
like the natural companion but produces an unbootable card — see below.

| Parameter | Purpose |
| --- | --- |
| `isolcpus=managed_irq,domain,3` | Takes core 3 out of the scheduler's load balancer; `managed_irq` also keeps multi-queue NIC interrupts off it |
| `rcu_nocbs=3` | Runs its RCU callbacks on the housekeeping cores instead |
| `nohz_full=3` | Stops the periodic scheduler tick on it |
| `irqaffinity=0,1,2` | Default affinity for every interrupt that is not explicitly pinned |
| `intel_idle.max_cstate=1` | Waking from a deep C-state costs tens of microseconds — more than a 1 ms cycle's jitter budget (x86 only) |
| `processor.max_cstate=1` | Same, for the ACPI idle driver (x86 only) |

### How many cores to isolate

**One, by default.** Motion Master runs exactly one real-time thread: `GameLoop::run()` blocks the
main thread and *is* the RT thread, and `apps/motion_master/game_loop.cc` is the only place
`setRealtimePriority()` is called. The HTTP server, the WebSocket server, the monitoring sampler
and the parameter refresher are all ordinary threads.

That matters on a 4-core board — the E3940 and the Raspberry Pi 5 both have four cores and no SMT.
Isolating two would leave the OS, the desktop, and Motion Master's own non-RT work (including
blocking SDO and FoE transfers) sharing two cores, in exchange for a second isolated core that
nothing runs on: interrupts are sent to the housekeeping cores by `irqaffinity`, so it would sit
genuinely empty.

`rt_isolated_cpu_count` (default `1`) takes that many cores from the top; `rt_isolated_cpus`
overrides it with an explicit list. The role refuses to leave the OS with no core at all.

The `irqaffinity` + `managed_irq` pair replaces the manual `echo 1 > /proc/irq/<n>/smp_affinity`
this document used to recommend. That form has to be repeated per interrupt, cannot express managed
interrupts at all, and does not survive a reboot.

The drop-in also sets `GRUB_TOP_LEVEL` to the installed RT kernel. Without it the boot default is
whichever kernel version sorts highest, so an ordinary non-RT kernel upgrade would quietly take the
machine off PREEMPT_RT. **Re-run the playbook after an RT kernel upgrade** so it points at the new
version; `rt-verify` fails loudly if the running kernel is ever not PREEMPT_RT.

**A Pi has no equivalent pin, and trying to add one breaks the card.** Setting `KERNEL=` in
`/etc/default/raspi-firmware` looks like the obvious analogue, but in `z50-raspi-firmware` the
variable holding the initrd filename is assigned *only* inside the `KERNEL="auto"` branch while
being used unconditionally to write `config.txt` — so an explicit `KERNEL=` emits an `initramfs`
line with no filename after it, and a modular arm64 kernel with no initrd panics on mounting root.
That branch is also what copies kernel, initrd and DTBs onto the firmware partition, so pinning
stops future kernel updates reaching it.

It is unnecessary anyway: `auto` picks `sort -V -r | head -1`, and at equal upstream versions
version-sort ranks `rt-arm64` above plain `arm64`, so the RT kernel and its matching initrd already
win. The residual risk — a *newer* non-RT kernel overtaking on version — is caught at build time,
where the image build asserts that the generated `config.txt` names an `-rt` kernel and a non-empty
initramfs.

### rt-limits — RT privileges

Creates a `realtime` group, adds `rt_user` to it, and writes
`/etc/security/limits.d/99-realtime.conf` granting `rtprio 99` and `memlock unlimited`. `GameLoop`
asks for `SCHED_FIFO` priority 80 (`kRtThreadPriority` in `libs/core/realtime.h`) and calls
`mlockall()`; without these limits both fail, and the loop runs — non-deterministically — anyway.

Group membership is read at login, so `ulimit -r` only reports 99 in a **new** session.

A systemd service ignores `limits.conf` entirely and needs `LimitRTPRIO=`/`LimitMEMLOCK=` in its
unit instead — which is why the `motion-master` unit below carries both.

### rt-tuning — everything else that adds jitter

- **Masks `irqbalance`**, which would otherwise redistribute interrupts across all cores within
  seconds and undo `irqaffinity=`.
- **Pins the CPU frequency governor** to `performance` via a small oneshot unit. Skipped on hosts
  with no cpufreq at all — virtual machines and fixed-frequency boards.
- **Disables segmentation and receive offload on the EtherCAT NIC**, via a systemd `.link` file
  rather than an `ethtool` invocation, so udev reapplies it whenever the device appears. Only when
  `rt_ethercat_interface` is set; guessing would reconfigure the management NIC.

SOEM takes exclusive raw-socket control of the EtherCAT NIC, so keep a separate NIC (or VLAN) for
management traffic and the Motion Master HTTP/WebSocket API.

### rt-verify — proof

Asserts the running kernel's build string (`uname -v`) contains `PREEMPT_RT`, which catches both a
failed install and a bootloader default that picked the wrong kernel. It deliberately does *not*
test `/sys/kernel/realtime`: that file came from the out-of-tree RT patch and no longer exists now
that PREEMPT_RT is merged upstream — on Debian's 7.1.3-rt it is absent while `CONFIG_PREEMPT_RT=y`,
so checking it would fail on a perfectly good real-time kernel.

Then prints the running kernel, the actual `/proc/cmdline`, the isolated and housekeeping cores,
the default IRQ mask, the governor, and `rt_user`'s effective `rtprio`/`memlock`:

```text
kernel            7.1.3+deb13-rt-amd64  (PREEMPT_RT confirmed)
cmdline           BOOT_IMAGE=/boot/vmlinuz-7.1.3+deb13-rt-amd64 ... isolcpus=managed_irq,domain,3 ...
isolated cores    3
housekeeping      0,1,2
default irq mask  7
governor          no cpufreq
rtprio            99  (debian)
memlock           unlimited  (debian)
```

`cyclictest` is tagged `benchmark` and does **not** run by default — it takes a minute, and see
*What the VM can and cannot tell you* for why it is meaningless in the VM.

### motion-master — the daemon

Off by default (`motion_master_install: true` to enable). Downloads the release `.deb` for
`motion_master_version` and the host's architecture — published for both `amd64` and `arm64`, so
one version string serves both boards — and installs it with apt, which lands everything in
`/opt/motion-master/` and runs the package's `setcap` for the raw-socket and real-time capabilities.

Then writes two files:

- `/opt/motion-master/motion-master.jsonc`. Auto-discovered because it sits beside the binary, so
  the service needs no `--config`. Sets `server.bindAddress` to `0.0.0.0` (an appliance is reached
  from another machine — **the API has no authentication**, so only on a network you trust; see
  [`docs/LAN_DEPLOYMENT.md`](../docs/LAN_DEPLOYMENT.md)), the fieldbus driver, and
  **`gameLoop.cpuAffinity` set to the core `rt-boot` isolated**.
- `/etc/systemd/system/motion-master.service`, running as root, with `LimitRTPRIO=99` and
  `LimitMEMLOCK=infinity`.

That `cpuAffinity` line is where the two halves meet, and it is the reason isolating a core is
worth anything: `isolcpus` takes the core out of the scheduler, so **nothing runs there until a
thread asks for it by name**. Without it the isolated core sits idle and every thread crowds onto
the rest.

The unit sets **no `CPUAffinity=`**, on purpose, and adding one would do active harm twice over. It
applies to the whole process, so it would drag the HTTP, WebSocket, monitoring and refresher threads
onto the isolated core to contend with the real-time one — and, by leaving more than one runnable
task there, stop `nohz_full` from taking effect. Worse, it *restricts the process's affinity mask*,
so a `gameLoop.cpuAffinity` naming a core outside it fails with `EINVAL` and the thread silently
stays unpinned. No capability helps with that; the same applies to a cgroup cpuset or an inherited
`taskset`.

Note that the pin itself needs no privilege — a thread may always set its own affinity, unlike
`SCHED_FIFO` and `mlockall`, which need `CAP_SYS_NICE` and `CAP_IPC_LOCK` (or the `rtprio`/`memlock`
limits `rt-limits` grants). The real-time thread pins *itself*. The result on a provisioned 4-core
host:

```text
$ ps -L -o comm,cls,rtprio,psr -p $(pgrep -x motion-master)
COMMAND         CLS RTPRIO PSR
motion-master    FF     80   3     ← RT thread, alone on the isolated core
motion-master    TS      -   2     ← HTTP
motion-master    TS      -   2     ← WebSocket
motion-master    TS      -   1     ← monitoring sampler
motion-master    TS      -   1     ← parameter refresher
```

`GET /api/game-loop` reports the same thing as `cpuAffinity` and `cpuPinned`, and the Console's
Game Loop page shows it — including a loud warning if a core was configured but the pin failed.

## What the VM Can and Cannot Tell You

**It can prove the automation is correct.** That the playbook converges from a genuinely clean
Debian 14, that a second run reports `changed=0`, that the reboot lands on the RT kernel, that the
limits and boot parameters are what you meant. This is the whole reason `reset.sh` is cheap: a
provisioning script is only trustworthy if it has been proven from a clean machine, and a clean
machine has to cost seconds or you stop checking.

**It cannot tell you anything about latency.** A guest inherits every scheduling delay of the host,
which here is an ordinary desktop Fedora kernel running a browser and a compiler. `cyclictest`
inside the VM measures that, not the guest's real-time behaviour, and a good number would be
misleading rather than reassuring. It also cannot tell you anything hardware-specific: C-states,
real NIC interrupts, EtherCAT wire timing.

Latency validation happens on the real target, and nowhere else.

## Verifying Latency on Hardware

```bash
rt/provision/play-rt.sh inventory/lab.yml --tags benchmark
```

or directly on the machine:

```bash
sudo cyclictest --mlockall --smp --priority=80 --interval=1000 --distance=0 --duration=60s
```

Target: max latency well under 200 µs for the 1 ms cycle. Spikes above 500 µs point at C-states or
interrupt affinity — check `/proc/cmdline` actually carries the parameters, and that `irqbalance`
is really masked.

`hil/jitter_bench` is the complementary measurement: `cyclictest` characterises the kernel, while
`jitter_bench` runs the same `CyclicTimer` loop `GameLoop` uses, at the same `SCHED_FIFO` priority.
Give it the same core to keep that equivalence honest — otherwise it measures an unpinned thread
while the server runs a pinned one:

```bash
sudo ./jitter_bench --cpu 3 --period 1000 --duration 60
```

When comparing the two boards, read *Comparing the two boards* above first — the shared
configuration makes the software constant, not the hardware.

## Raspberry Pi 5

```bash
./rt/image/build-rpi-image.sh          # as yourself, not sudo
sudo dd if=rt/image/.cache/motion-master-rpi5.img of=/dev/sdX bs=4M status=progress conv=fsync
```

The base image is `debian-14-raspi-arm64-daily.tar.xz` from **Debian Cloud Images** — an official
build (`vendor: raspi`, `arch: arm64`), containing a single 3 GB `disk.raw`.

**Do not use raspi.debian.net's own download pages.** It still serves "daily" and "tested" listings
that stop at Pi 4 and whose files now 404, and an FAQ still claiming Pi 5 is unsupported. Its front
page says the project "has been superseded by the images generated by the Debian Cloud Images" —
only that page was updated. The same front page is where the Pi-5-needs-Forky requirement comes
from, which is consistent with stable shipping a 6.12 kernel that predates usable RP1 support
(RP1 carries the Pi 5's Ethernet and USB).

The Pi still cannot be provisioned the way the x86 board is: it has no cloud-init seed and no
network until it has booted once, so the kernel and configuration have to be in place *before*
first boot. That is what the image build is for.

So the image is provisioned offline: boot its root filesystem under QEMU, run **the same
`rt-target.yml`** against it over SSH, shut down, write the result to a card. Same roles, same
inventory shape — only the delivery differs.

Two things are unlike the x86 loop:

- **QEMU has no Pi 5 machine type.** The script boots a generic arm64 `virt` guest using the kernel
  and initrd taken out of the image; the Pi's own firmware boots the card for real. The guest only
  has to run the userland long enough to provision it. Consequences the playbook handles via
  `inventory/rpi-image.yml`: no reboot (it would return on the borrowed kernel), the daemon is
  enabled but not started, `rt-verify` is skipped (it asserts on the *running* kernel), and the CPU
  governor unit is installed even though the emulated guest has no cpufreq. The script checks the
  finished image's contents instead — RT kernel present and newest, `isolcpus` on the command line,
  limits, daemon enabled, `cpuAffinity` configured.
- **No KVM for arm64 on an x86 host,** so it runs under TCG emulation and takes a while. Develop
  roles on the fast x86 VM; come here to produce an image.

**Run it as yourself, not under `sudo`.** Only the loop-mount and chroot need root, and the script
asks for your password once up front (then keeps the timestamp warm, so the slow emulated `apt` step
cannot trigger a second prompt mid-run). Running the whole thing as root works but leaves an 8 GB
image and a private key root-owned in your working tree — if that has already happened, the script
reclaims them on the next ordinary run.

**Prerequisites:** `qemu-system-aarch64`, `qemu-user-static` (its `binfmt_misc` handler is what lets
`apt` run inside the arm64 root filesystem on an x86 host), and sudo rights — nothing else in `rt/`
needs any of them. `./tools/install-deps.sh` installs all of it.

The image ships neither cloud-init nor `openssh-server`, so the build installs sshd in that chroot
before first boot; without it there would be no way for Ansible to get in.

### Unverified on hardware

The script itself has not been run end to end, and whether the 7.1.3-rt kernel drives BCM2712 and
RP1 on a real Pi 5 **cannot be established in QEMU** — QEMU is not a Pi. That is a card-in-the-slot
test. On first boot, confirm over serial or HDMI that the board comes up and that Ethernet appears.
Debian's position is that Forky supports the board, but "supported" and "boots with an RT kernel and
a 1 ms EtherCAT cycle" are different claims.

Also still open: mDNS/Avahi discovery so the Pi can be found without knowing its address. Design
context for the appliance — the LAN certificate scheme and what ships in the image — is in
`NEXTGEN.md` (sessions 2026-06-12, 2026-07-24 and 2026-07-31) and
[`docs/LAN_DEPLOYMENT.md`](../docs/LAN_DEPLOYMENT.md).
