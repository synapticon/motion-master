# Motion Master

Next-generation motion control software featuring a simplified design, streamlined HTTP API, and true real-time support. See [NEXTGEN.md](NEXTGEN.md) for the full design specification.

## Prerequisites

- CMake 4.0+
- Ninja
- GCC / Clang with C++23 support (or MSVC on Windows)
- Git

## Getting Started

```bash
git clone <repo-url>
cd motion-master
git submodule update --init --recursive

./tools/configure.sh
./tools/build.sh
```

The `motion-master` binary lands in `build/x64-linux-debug/apps/motion_master/`.

## Usage

```
motion-master [OPTIONS]

  -h, --help                    Print this help message and exit
      --version                 Display program version and exit
  -c, --config TEXT:FILE        Path to JSON config file
  -p, --port UINT [8443]        HTTP/WebSocket port
  -d, --driver TEXT [soem]      Fieldbus driver: soem, spoe, igh
  -l, --log-level TEXT [info]   Log level: trace, debug, info, warn, error
```

## Developer Scripts

All scripts default to the `x64-linux-debug` preset. Pass a preset name as the first argument to override (e.g. `./tools/build.sh x64-linux-release`).

| Script | Description |
|---|---|
| `./tools/configure.sh` | Run CMake configure |
| `./tools/build.sh` | Build all targets |
| `./tools/test.sh` | Run tests |
| `./tools/format.sh` | Auto-format all sources with clang-format |
| `./tools/lint.sh` | Run cpplint (`pip install cpplint` if missing) |
| `./tools/cppcheck.sh` | Run cppcheck static analysis |
| `./tools/clean.sh` | Remove the build directory |

### Code Quality Tools

- **`format`** — runs `clang-format` over all `.cc`/`.h` sources and rewrites them in-place. Enforces Google style with a 100-column limit as defined in `.clang-format`. CI fails if any file is not already formatted.
- **`lint`** — runs `cpplint` to check for Google-style C++ convention violations: include order, naming, deprecated constructs, and header guards. Configured via `CPPLINT.cfg`.
- **`cppcheck`** — static analysis that catches bugs the compiler doesn't warn about: null pointer dereferences, out-of-bounds access, uninitialized variables, resource leaks, etc. Runs with `warning,style,performance,portability` checks at `--std=c++23` and exits non-zero on any finding.

## CI

GitHub Actions (`.github/workflows/build.yml`) caches vcpkg pre-built packages in `~/.cache/vcpkg/archives` via `actions/cache@v5`, keyed on OS and `vcpkg.json` hash. The first run after a dependency change rebuilds from source; subsequent runs restore from cache.

## Dependencies

Managed via [vcpkg](https://vcpkg.io). No manual installation needed — vcpkg downloads and builds everything on first configure.

| Package | Purpose |
|---|---|
| CLI11 | Command-line argument parsing |
| spdlog | Structured logging |
| nlohmann-json | JSON config file parsing |
| neargye-semver | Semantic versioning |
| GTest | Unit testing |

## Platform Support

| Platform | Status |
|---|---|
| Linux x86-64 | Primary target |
| Linux ARM | Planned |
| Windows x64 | Planned |

## Real-Time Deployment on Linux (Intel E3940 / AAeon)

The reference embedded target is an AAeon board with an Intel E3940 (Apollo Lake) CPU, 4 GB RAM, and 20 GB storage. The recommended OS is **Debian 13 (Trixie)** with XFCE desktop (~5 GB installed, leaves headroom for build artifacts).

### 1. Install the RT Kernel

```bash
sudo apt install linux-image-rt-amd64 linux-headers-rt-amd64
sudo reboot
```

Verify after reboot:

```bash
uname -r          # should contain -rt
cat /sys/kernel/realtime   # prints 1
```

### 2. Boot Parameters

Add to `GRUB_CMDLINE_LINUX` in `/etc/default/grub`, then run `sudo update-grub`:

```
isolcpus=2,3 rcu_nocbs=2,3 nohz_full=2,3 intel_idle.max_cstate=1 processor.max_cstate=1 quiet
```

`isolcpus=2,3` reserves cores 2 and 3 for the RT thread. The E3940 has 4 cores (0–3); cores 0–1 handle the OS and GUI. Adjust if you need more OS headroom.

### 3. RT Privileges for the motion-master User

```bash
sudo groupadd realtime
sudo usermod -aG realtime $USER
```

Create `/etc/security/limits.d/99-realtime.conf`:

```
@realtime soft rtprio  99
@realtime hard rtprio  99
@realtime soft memlock unlimited
@realtime hard memlock unlimited
```

Log out and back in for group membership to take effect.

### 4. Set CPU Affinity at Launch

Pin motion-master to the isolated cores so the OS scheduler never migrates it:

```bash
taskset -c 2 ./motion-master --driver soem
```

Or use `chrt` to also set the scheduling policy explicitly:

```bash
chrt -f 80 taskset -c 2 ./motion-master --driver soem
```

The `GameLoop` already calls `pthread_setschedparam(SCHED_FIFO, 80)` internally; `chrt` here is a belt-and-suspenders fallback in case the process lacks `CAP_SYS_NICE`.

### 5. EtherCAT NIC Assignment

The E3940 board typically has an Intel I211 or I219 onboard NIC. Identify your interfaces:

```bash
ip link show
lspci | grep -i ethernet
```

Pass the EtherCAT interface name to motion-master via config or `--ifname` (once that flag is wired up). Keep a separate NIC or VLAN for management traffic — SOEM takes exclusive control of the EtherCAT NIC.

Verify the NIC supports raw socket access (no offloading that interferes with EtherCAT frames):

```bash
sudo ethtool -K <iface> gso off gro off tso off
```

### 6. IRQ Affinity

Move NIC interrupts off the isolated cores so they do not interrupt the RT thread:

```bash
# Find the IRQ number for your NIC
grep <iface> /proc/interrupts

# Pin it to core 0
echo 1 | sudo tee /proc/irq/<IRQ>/smp_affinity
```

Consider adding this to a systemd service or `/etc/rc.local` for persistence.

### 7. Verify Latency

Install `cyclictest` and run a latency benchmark before deploying:

```bash
sudo apt install rt-tests
sudo cyclictest --mlockall --smp --priority=80 --interval=1000 --distance=0 --duration=60s
```

Target: max latency well under 200 µs with the 1 ms cycle. If you see spikes above 500 µs, revisit C-state and IRQ affinity settings.
