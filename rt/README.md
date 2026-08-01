# Real-Time Linux Setup

This directory contains everything needed to run Motion Master on a real-time Linux system — documentation, configuration files, and Ansible playbooks for automated provisioning.

The target is a freshly installed Debian system going to a fully configured, real-time Motion Master host with no manual steps beyond running the playbook.

## Reference Hardware

The primary embedded target is an **AAeon board with an Intel E3940 (Apollo Lake)** CPU, 4 GB RAM, 20 GB storage. The recommended OS is **Debian 13 (Trixie)** with XFCE desktop (~5 GB installed).

## Overview

Getting Motion Master to run with hard real-time guarantees requires:

1. An RT-patched kernel (`PREEMPT_RT`)
2. Boot parameters that isolate CPU cores and disable C-states
3. A `realtime` group with elevated `rtprio` and `memlock` limits
4. CPU affinity pinning Motion Master to the isolated cores
5. NIC interrupt affinity moved off the isolated cores

Steps 1–3 are one-time system configuration. Steps 4–5 are part of the launch procedure (or a systemd service unit).

---

## 1. Install the RT Kernel

```bash
sudo apt install linux-image-rt-amd64 linux-headers-rt-amd64
sudo reboot
```

Verify after reboot:

```bash
uname -r              # should contain "-rt"
cat /sys/kernel/realtime   # prints 1
```

---

## 2. Boot Parameters

Add to `GRUB_CMDLINE_LINUX` in `/etc/default/grub`, then run `sudo update-grub`:

```text
isolcpus=2,3 rcu_nocbs=2,3 nohz_full=2,3 intel_idle.max_cstate=1 processor.max_cstate=1 quiet
```

| Parameter | Purpose |
| --- | --- |
| `isolcpus=2,3` | Removes cores 2–3 from the general scheduler; only explicitly pinned tasks run there |
| `rcu_nocbs=2,3` | Offloads RCU callbacks off isolated cores |
| `nohz_full=2,3` | Disables the periodic timer tick on isolated cores (tickless mode) |
| `intel_idle.max_cstate=1` | Prevents the CPU from entering deep sleep states that cause wake-up latency |
| `processor.max_cstate=1` | Redundant safeguard for the ACPI CPU idle driver |

The E3940 has 4 cores (0–3). Cores 0–1 handle the OS and GUI; cores 2–3 are reserved for the RT thread. Adjust if you need more OS headroom or have a different core count.

---

## 3. RT Privileges

Create a `realtime` group and add the Motion Master user:

```bash
sudo groupadd realtime
sudo usermod -aG realtime $USER
```

Create `/etc/security/limits.d/99-realtime.conf`:

```text
@realtime soft rtprio  99
@realtime hard rtprio  99
@realtime soft memlock unlimited
@realtime hard memlock unlimited
```

Log out and back in for group membership to take effect. Verify with:

```bash
ulimit -r    # should print 99
ulimit -l    # should print unlimited
```

---

## 4. Launch with CPU Affinity

Pin Motion Master to the isolated cores:

```bash
taskset -c 2 ./motion-master --cert /path/to/cert.pem --key /path/to/key.pem --driver soem
```

To also set the scheduling policy explicitly:

```bash
chrt -f 80 taskset -c 2 ./motion-master --cert /path/to/cert.pem --key /path/to/key.pem --driver soem
```

`GameLoop` calls `pthread_setschedparam(SCHED_FIFO, 80)` internally; `chrt` is a belt-and-suspenders fallback in case the process lacks `CAP_SYS_NICE`.

---

## 5. EtherCAT NIC Assignment

Identify your network interfaces:

```bash
ip link show
lspci | grep -i ethernet
```

SOEM takes exclusive raw socket control of the EtherCAT NIC. Keep a separate NIC or VLAN for management traffic and the Motion Master HTTP/WebSocket API.

Disable offloading features that interfere with EtherCAT frames:

```bash
sudo ethtool -K <iface> gso off gro off tso off
```

---

## 6. IRQ Affinity

Move NIC interrupts off the isolated cores so they do not preempt the RT thread:

```bash
# Find the IRQ number for your NIC
grep <iface> /proc/interrupts

# Pin it to core 0
echo 1 | sudo tee /proc/irq/<IRQ>/smp_affinity
```

Add this to a systemd service or `/etc/rc.local` for persistence.

---

## 7. Verify Latency

Install `cyclictest` and run a latency benchmark before deploying:

```bash
sudo apt install rt-tests
sudo cyclictest --mlockall --smp --priority=80 --interval=1000 --distance=0 --duration=60s
```

Target: max latency well under 200 µs with the 1 ms cycle. If you see spikes above 500 µs, revisit C-state and IRQ affinity settings.

---

## Ansible Provisioning

> Planned — Ansible playbooks will automate everything above, taking a freshly installed Debian system to a fully configured, real-time Motion Master host.
