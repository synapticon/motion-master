# Motion Master

Next-generation motion control software for SOMANET servo drives. Browser-based control interface, real-time process data exchange, and a secure HTTP API and WebSocket interface — control from any language, any tool, any AI agent.

## Prerequisites

- CMake 4.0+
- Ninja
- GCC / Clang with C++23 support (or MSVC on Windows)
- Git
- **Windows only:** [Npcap](https://npcap.com) in WinPcap-compatible mode — required at runtime for raw EtherCAT packet capture (install with "Install Npcap in WinPcap API-compatible Mode" checked)

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
  -c, --config TEXT:FILE        Path to JSONC config file (JSON with // and /* */ comments)
  -p, --port UINT [8443]        HTTP/WebSocket port
      --cert TEXT:FILE          TLS certificate file
      --key TEXT:FILE           TLS private key file
  -d, --driver TEXT             Fieldbus driver: soem (omit to defer initialisation to the HTTP API)
  -l, --log-level TEXT [info]   Log level: trace, debug, info, warn, error
  -a, --adapter TEXT            Network adapter for EtherCAT: interface name or MAC address
      --list-adapters           Print available network adapters and exit
```

On Linux, `motion-master` requires raw socket and RT scheduling capabilities. `./tools/build.sh` runs `sudo setcap cap_sys_nice,cap_net_admin,cap_net_raw=eip` on the binary after linking — you will be prompted for your password. Without these capabilities the binary still runs but EtherCAT initialisation will fail.

## Installation

Release packages are available on the [Releases](../../releases) page. Every release ships three artefacts:

| Artefact | Format | Install on |
|---|---|---|
| `motion-master-<version>-linux-x64.tar.gz` | Tarball | Any Linux x86-64 |
| `motion-master-<version>-amd64.deb` | Debian package | Ubuntu / Debian |
| `motion-master-<version>-x86_64.rpm` | RPM package | Fedora / RHEL / openSUSE |

All packages install to `/opt/motion-master/` with a `/usr/local/bin/motion-master` symlink.

### Debian / Ubuntu

```bash
sudo apt install ./motion-master-<version>-amd64.deb
```

The `postinst` script automatically sets the required capabilities (`cap_sys_nice`, `cap_net_admin`, `cap_net_raw`) on the binary.

### Fedora / RHEL / openSUSE

```bash
sudo dnf install ./motion-master-<version>-x86_64.rpm   # Fedora / RHEL
sudo zypper install ./motion-master-<version>-x86_64.rpm # openSUSE
```

### Tarball

```bash
tar -xzf motion-master-<version>-linux-x64.tar.gz
cd motion-master-<version>-linux-x64
sudo ./setup.sh    # sets capabilities once; re-run after any OS update that resets them
./motion-master --help
```

## Local Development

Production releases bundle a real Let's Encrypt TLS certificate for `local.motion-master.synapticon.com`, so the PWA at `https://motion-master.synapticon.com` connects without any browser warning.

For development, the run script picks up a cert automatically:

```bash
./tools/run.sh
```

It looks for a certificate in this order:
1. `cert.pem` / `key.pem` next to the binary (present in release builds)
2. `~/.acme.sh/local.motion-master.synapticon.com_ecc/` — if you have `acme.sh` installed locally with the Let's Encrypt cert (no browser warning)
3. Self-signed fallback — generated on the fly; requires accepting a browser security exception once per server restart

Test the API (add `-k` only when using the self-signed fallback):

```bash
curl -k https://localhost:8443/api/version
curl -k https://localhost:8443/api/swagger.yml
```

### Fieldbus lifecycle via API

`--driver` and `--adapter` are optional at startup. When omitted, the fieldbus is uninitialised and `GET /api/devices` returns an empty array. Use the lifecycle endpoints to initialise at runtime:

```bash
# 1. Discover available adapters
curl -k https://localhost:8443/api/adapters

# 2. Initialise the fieldbus driver
curl -k -X POST https://localhost:8443/api/init \
     -H 'Content-Type: application/json' \
     -d '{"driver":"soem","adapter":"eth0"}'

# 3. Scan for slaves and populate the device list
curl -k -X POST https://localhost:8443/api/scan

# 4. List discovered devices
curl -k https://localhost:8443/api/devices

# 5. Transition all devices to Op state (state values: 1=Init, 2=PreOp, 3=Boot, 4=SafeOp, 8=Op)
curl -k -X POST https://localhost:8443/api/state \
     -H 'Content-Type: application/json' \
     -d '{"state":8}'

# 6. Transition specific devices, with a custom timeout
curl -k -X POST https://localhost:8443/api/state \
     -H 'Content-Type: application/json' \
     -d '{"state":2,"positions":[1,2],"timeout":3000}'

# 7. Tear down (stops driver, clears device list; init + scan can be called again)
curl -k -X POST https://localhost:8443/api/reset
```

Connect a WebSocket client to `wss://localhost:8443/ws`. The server sends two message types:

```json
{"type": "monitoring", "topic": "pdos", "data": [1234567890, 39, 0, 12345]}
{"type": "notification", "data": {"event": "slaves_changed"}}
```

Fetch the monitoring schema to interpret the `data` array:

```bash
curl -k https://localhost:8443/api/monitoring/pdos
```

## Developer Scripts

All scripts default to the `x64-linux-debug` preset. Pass a preset name as the first argument to override (e.g. `./tools/build.sh x64-linux-release`).

| Script | Description |
|---|---|
| `./tools/configure.sh` | Run CMake configure |
| `./tools/build.sh` | Build all targets |
| `./tools/run.sh` | Run the binary with the best available TLS cert (real cert if acme.sh is set up, self-signed otherwise) |
| `./tools/test.sh` | Run tests |
| `./tools/format.sh` | Auto-format all sources with clang-format |
| `./tools/lint.sh` | Run cpplint (`pip install cpplint` if missing) |
| `./tools/cppcheck.sh` | Run cppcheck static analysis |
| `./tools/clean.sh` | Remove the build directory |
| `./tools/bump-version.sh <version>` | Bump the project semver everywhere (see [Versioning](#versioning)) |
| `./tools/package.sh [preset]` | Build `.deb` and `.rpm` packages (requires `cert.pem`/`key.pem` in the build dir) |

### Code Quality Tools

- **`format`** — runs `clang-format` over all `.cc`/`.h` sources and rewrites them in-place. Enforces Google style with a 100-column limit as defined in `.clang-format`. CI fails if any file is not already formatted.
- **`lint`** — runs `cpplint` to check for include order, deprecated constructs, and header guards. Configured via `CPPLINT.cfg`. Naming conventions are enforced in code review, not by this tool.
- **`cppcheck`** — static analysis that catches bugs the compiler doesn't warn about: null pointer dereferences, out-of-bounds access, uninitialized variables, resource leaks, etc. Runs with `warning,style,performance,portability` checks at `--std=c++23` and exits non-zero on any finding.

## Versioning

All components — C++ backend, React UI, OpenAPI spec, and npm packages — share a single semver. `VERSION` (repo root) is the canonical source; CMake reads it automatically to populate `libs/core/version.h`. Use the bump script to update every location in one shot:

```bash
./tools/bump-version.sh 6.1.0
./tools/bump-version.sh 6.1.0-alpha.0
```

After bumping, commit the changed files, then push a `v<version>` tag to trigger the release workflow:

```bash
git add -A
git commit -m "chore: bump version to 6.1.0"
git tag v6.1.0
git push && git push --tags
```

## CI

| Workflow | Trigger | Purpose |
|---|---|---|
| `build.yml` | push / PR to `main` | Build, test; vcpkg packages cached in `~/.cache/vcpkg/archives` |
| `lint.yml` | push / PR to `main` | clang-format + cpplint checks |
| `cert-renewal.yml` | 1st of every month | Renew Let's Encrypt cert via acme-dns; update `TLS_CERT` / `TLS_KEY` secrets |
| `release.yml` | `v*` tag push | Build release binary, bundle cert + key from secrets, publish GitHub Release with `.tar.gz`, `.deb`, and `.rpm` packages |

The vcpkg cache key is OS + `vcpkg.json` hash. The first run after a dependency change rebuilds from source; subsequent runs restore from cache.

## Dependencies

Managed via [vcpkg](https://vcpkg.io). No manual installation needed — vcpkg downloads and builds everything on first configure.

| Package | Purpose |
|---|---|
| CLI11 | Command-line argument parsing |
| spdlog | Structured logging |
| nlohmann-json | JSONC config file parsing (comments enabled) and HTTP response serialization |
| neargye-semver | Semantic versioning |
| uwebsockets | HTTP and WebSocket server (TLS via OpenSSL) |
| GTest | Unit testing |

## Platform Support

| Platform | Status |
|---|---|
| Linux x86-64 | Primary target |
| Linux ARM | Planned |
| Windows x64 | Planned |

## Real-Time Linux

Motion Master targets `CONFIG_PREEMPT_RT` kernels for hard real-time operation. The `GameLoop` sets `SCHED_FIFO` priority 80 and calls `mlockall` before entering the cycle loop. The cycle timer uses `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` so scheduling jitter in one cycle never accumulates into drift.

## Hardware-in-the-Loop Tests

The `hil/` directory contains standalone binaries for validating RT behaviour on a pre-configured Linux machine. They are built automatically with the rest of the project but require root (or `CAP_SYS_NICE` + `CAP_IPC_LOCK`) to produce valid results.

### jitter_bench

Measures the cycle-to-cycle scheduling jitter of the `GameLoop` timer loop — how much each actual cycle interval deviates from the target period.

```bash
# Build
./tools/build.sh

# Run 30 s at 1 ms period, write jitter.csv
sudo ./build/x64-linux-debug/hil/jitter_bench/jitter_bench

# Simulate 300 µs of task load per cycle
sudo ./build/x64-linux-debug/hil/jitter_bench/jitter_bench --workload 300

# Plot (requires matplotlib)
python3 hil/jitter_bench/plot_jitter.py jitter.csv
python3 hil/jitter_bench/plot_jitter.py jitter.csv -o report.png

# Full option list
./build/x64-linux-debug/hil/jitter_bench/jitter_bench --help
```

The plot shows a time-series with P99/P99.9 reference lines and a jitter histogram. The terminal output prints min/max/mean/stddev/P50/P95/P99/P99.9 and an overrun count. Compare a standard kernel against `PREEMPT_RT` by running with `--workload 300` (a realistic 1 ms cycle budget) on each.
