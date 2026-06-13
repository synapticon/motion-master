# Motion Master

Next-generation motion control software for SOMANET servo drives. Browser-based control interface, real-time process data exchange, and a secure HTTP API and WebSocket interface — control from any language, any tool, any AI agent.

## Architecture

Design documents with Mermaid diagrams (rendered natively on GitHub):

- [Threading model](https://github.com/synapticon/motion-master/blob/main/docs/THREADS.md) — the five threads, RT cycle, and control-plane vs PDO-path locking.
- [Class diagram](https://github.com/synapticon/motion-master/blob/main/docs/CLASS_DIAGRAM.md) — class structure, ownership, and inheritance.

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
  -p, --port UINT [61447]       HTTP API port
      --ws-port UINT [62281]    Realtime WebSocket port (separate loop from the HTTP API)
      --cert TEXT               TLS certificate file
      --key TEXT                TLS private key file
      --update-cert             Download a fresh TLS cert/key, install them, and exit
      --no-cert-update          Do not auto-fetch a fresh cert on startup when it is missing/expired
      --cert-url TEXT           Source URL for the TLS certificate (default: rolling release)
      --key-url TEXT            Source URL for the TLS private key (default: rolling release)
      --cors-origin TEXT [https://motion-master.synapticon.com]
                                Value sent in Access-Control-Allow-Origin (use '*' to allow any)
  -d, --driver TEXT             Fieldbus driver: soem (omit to defer initialisation to the HTTP API)
  -l, --log-level TEXT [info]   Log level: trace, debug, info, warn, error
  -a, --adapter TEXT            Network adapter for EtherCAT: interface name or MAC address
      --list-adapters           Print available network adapters and exit
      --open                    Open https://motion-master.synapticon.com/app/ in the default browser after startup
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
sudo apt install ./motion-master-<version>-amd64.deb    # install or upgrade
sudo apt remove motion-master                            # remove (leaves cert.pem / key.pem)
sudo apt purge motion-master                             # full removal including certs
```

The `postinst` script automatically sets the required capabilities (`cap_sys_nice`, `cap_net_admin`, `cap_net_raw`) on the binary. On upgrade the capabilities are re-applied to the new binary automatically.

> **Note:** `apt remove` leaves `cert.pem` and `key.pem` behind as conffiles. Use `apt purge` for a complete uninstall.

### Fedora / RHEL / openSUSE

```bash
sudo dnf install ./motion-master-<version>-x86_64.rpm   # Fedora / RHEL (install or upgrade)
sudo zypper install ./motion-master-<version>-x86_64.rpm # openSUSE (install or upgrade)
sudo dnf remove motion-master                            # full removal
```

On uninstall, unmodified `cert.pem` and `key.pem` are removed automatically. If you replaced them with your own, they are saved as `cert.pem.rpmsave` / `key.pem.rpmsave`.

### Tarball

```bash
tar -xzf motion-master-<version>-linux-x64.tar.gz
cd motion-master-<version>-linux-x64
sudo ./setup.sh    # sets capabilities once; re-run after any OS update that resets them
./motion-master --help
```

### Docker

```bash
# Build
git submodule update --init --recursive
docker build -t motion-master .
```

`--network host` is required on all `docker run` commands — the server binds to `127.0.0.1` and Docker's port forwarding never reaches the loopback interface.

**TLS certificates**

Release images have `cert.pem`/`key.pem` baked in (CI places them at the repo root before `docker build`). Developer images built from source fall back to the acme.sh cert or a self-signed cert. The discovery order is the same as `tools/run.sh`:

```bash
# Release image — bundled cert used automatically
docker run --rm --network host motion-master

# Developer image — mount acme.sh cert from host (no browser warning)
docker run --rm --network host \
  -v "$HOME/.acme.sh/local.motion-master.synapticon.com_ecc:/root/.acme.sh/local.motion-master.synapticon.com_ecc:ro" \
  motion-master

# Developer image — self-signed fallback (browser security exception required)
docker run --rm --network host motion-master
```

**Updating an expired cert on an older image**

The bundled cert is renewed monthly, but an older image keeps its original cert. By default the container self-heals: if the baked-in cert is expired at startup it fetches a fresh one from the rolling release before serving (pass `--no-cert-update` to disable, e.g. for air-gapped hosts). To pin your own cert instead, override it at runtime — the volume mount shadows the baked-in file:

```bash
docker run --rm --network host \
  -v /path/to/cert.pem:/opt/motion-master/cert.pem:ro \
  -v /path/to/key.pem:/opt/motion-master/key.pem:ro \
  motion-master
```

Or point to an arbitrary path with env vars:

```bash
docker run --rm --network host \
  -e CERT=/certs/cert.pem -e KEY=/certs/key.pem \
  -v /path/to/cert.pem:/certs/cert.pem:ro \
  -v /path/to/key.pem:/certs/key.pem:ro \
  motion-master
```

**Capabilities**

Docker drops most Linux capabilities by default. On a bare-metal install `postinst`/`setup.sh` stamps the binary with `setcap` so any user can run it and it receives the required capabilities automatically. Inside a container, file capabilities are ignored — you grant the equivalent capabilities to the container process with `--cap-add` at `docker run` time instead.

| Capability | What it unlocks | Required for |
|---|---|---|
| `CAP_NET_RAW` | Open raw/packet sockets | SOEM sending/receiving raw EtherCAT frames |
| `CAP_NET_ADMIN` | Configure network interfaces | SOEM putting the NIC into promiscuous mode |
| `CAP_SYS_NICE` | Set `SCHED_FIFO` scheduling policy and RT priority | Real-time game loop |
| `CAP_IPC_LOCK` | Call `mlockall()` to pin process memory | Preventing page faults during RT cycles |

`--ulimit memlock=-1` is also required alongside `CAP_IPC_LOCK` — without it the kernel rejects `mlockall()` even when the capability is present.

The binary degrades gracefully: missing RT caps produce a warning and the loop runs without RT guarantees; missing EtherCAT caps cause `POST /api/init` to fail when a SOEM driver is requested.

```bash
# EtherCAT only (no RT requirement on the host kernel)
docker run --rm --network host \
  --cap-add NET_ADMIN --cap-add NET_RAW \
  motion-master --driver soem --adapter eth0

# RT scheduling only (PREEMPT_RT host kernel required)
docker run --rm --network host \
  --cap-add SYS_NICE --cap-add IPC_LOCK --ulimit memlock=-1 \
  motion-master

# Full EtherCAT + RT
docker run --rm --network host \
  --cap-add NET_ADMIN --cap-add NET_RAW \
  --cap-add SYS_NICE --cap-add IPC_LOCK --ulimit memlock=-1 \
  motion-master --driver soem --adapter eth0
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

If the cert is missing or already expired at startup, the binary fetches a fresh one from the rolling release and installs it before serving (disable with `--no-cert-update`). You can also refresh on demand — `motion-master --update-cert` (terminal) or the **Refresh certificate** button on the PWA's Connection page (`POST /api/cert/refresh`). The cert is rotated monthly and published at a stable URL, decoupled from app releases:

```
https://github.com/synapticon/motion-master/releases/download/tls-cert/{cert,key}.pem
```

Test the API (add `-k` only when using the self-signed fallback):

```bash
curl -k https://localhost:61447/api/version
```

### CORS

The server sends `Access-Control-Allow-Origin: https://motion-master.synapticon.com` by default so the production PWA can reach a locally running backend. When developing the UI against a different origin (e.g. Vite dev server on `http://localhost:5173`), override it via the `CORS_ORIGIN` env var picked up by `tools/run.sh`, or by passing `--cors-origin` to the binary directly:

```bash
# Vite dev server
CORS_ORIGIN=http://localhost:5173 ./tools/run.sh

# Allow any origin (development only — do not use in production)
CORS_ORIGIN='*' ./tools/run.sh

# Equivalent, calling the binary directly
./build/x64-linux-debug/apps/motion_master/motion-master --cors-origin http://localhost:5173
```

### Fieldbus lifecycle via API

`--driver` and `--adapter` are optional at startup. When omitted, the fieldbus is uninitialised and `GET /api/devices` returns an empty array. Use the lifecycle endpoints to initialise at runtime:

```bash
# 1. Discover available adapters
curl -k https://localhost:61447/api/adapters

# 2. Initialise the fieldbus driver
curl -k -X POST https://localhost:61447/api/init \
     -H 'Content-Type: application/json' \
     -d '{"driver":"soem","adapter":"eth0"}'

# 3. Scan for slaves and populate the device list. The first scan of a given drive enumerates its
#    full CoE object dictionary (hundreds of mailbox round-trips, a few seconds); the definitions
#    are cached on disk per device identity, so later scans of the same hardware skip it. Enabled
#    for Synapticon drives by default — see the parameterCache config block. Inspect or clear the
#    cache via GET/DELETE /api/parameter-caches (or the Parameter Caches page in the UI).
curl -k -X POST https://localhost:61447/api/scan

# 4. List discovered devices
curl -k https://localhost:61447/api/devices

# 5. Climb to Op state (state values: 1=Init, 2=PreOp, 3=Boot, 4=SafeOp, 8=Op).
#    EtherCAT only allows single-step climbs, so go up one level at a time. After a
#    scan devices sit in Init; jumping straight to a higher state is rejected.
curl -k -X POST https://localhost:61447/api/devices/state -H 'Content-Type: application/json' -d '{"state":2}'  # PreOp
curl -k -X POST https://localhost:61447/api/devices/state -H 'Content-Type: application/json' -d '{"state":4}'  # SafeOp
curl -k -X POST https://localhost:61447/api/devices/state -H 'Content-Type: application/json' -d '{"state":8}'  # Op

# 6. Transition specific devices, with a custom timeout
curl -k -X POST https://localhost:61447/api/devices/state \
     -H 'Content-Type: application/json' \
     -d '{"state":2,"positions":[1,2],"timeout":3000}'

# 7. Tear down (stops driver, clears device list; init + scan can be called again)
curl -k -X POST https://localhost:61447/api/reset
```

Connect a WebSocket client to `wss://localhost:62281` (the realtime channel runs on its own port and event loop, separate from the HTTP API on 61447, so a slow HTTP request never stalls the stream; the whole port is the WebSocket, so the URL needs no path). The server sends two message types:

```json
{"type": "monitoring", "topic": "left-leg", "data": [[1735821000123456, 39, 0, 12345], ...]}
{"type": "notification", "data": {"event": "slaves_changed"}}
```

`data` is an array of cycle rows — the stream is **lossless**, one row per recorded process-data cycle since the last flush. Each row is `[timestampUs, v0, v1, ...]`: epoch microseconds followed by one value per parameter in the monitoring's order (a value is `null` while its device is not exchanging). `interval` is the flush cadence (5–2000 ms), not a sample rate. Fetch the parameter order (and how each value is sourced) to interpret the array:

```bash
curl -k https://localhost:61447/api/monitorings/left-leg
```

### Bus inspection

Read-only endpoints expose the configured bus state and live health — none of them reconfigure the bus:

```bash
# Per-slave ESC configuration captured at scan (Sync Managers, FMMUs, mailbox, distributed clock)
curl -k https://localhost:61447/api/bus-config

# Process-image layout: every PDO-mapped object resolved to a bit offset, plus working-counter health
curl -k https://localhost:61447/api/process-image

# Dump the lossless recorder to a binary .mmpd file (full raw inputs+outputs for every cycle in the
# ring, with the process image embedded as a header so it decodes offline). Captures the ring from
# oldest to newest at the moment of the call — works while exchanging too. The file is written on the
# server's machine under recorder.dumpDir (default: <temp>/motion-master); the response is its path.
curl -k -X POST https://localhost:61447/api/process-data/dump

# Current AL state of every device (1=Init, 2=PreOp, 3=Boot, 4=SafeOp, 8=Op)
curl -k https://localhost:61447/api/devices/state

# Live link diagnostics from each slave's EtherCAT Slave Controller: per-port error counters
# (invalid frame, RX / forwarded errors, lost link), link state, and watchdog expirations. The
# counters are cumulative — watch for a rising delta to localise a degrading cable or connector.
curl -k https://localhost:61447/api/devices/diagnostics

# The device queries accept a positions filter
curl -k 'https://localhost:61447/api/devices/diagnostics?positions=1,2'
```

In the web UI these are the **Fieldbus** group's pages — Control (init / scan / state), Configuration, Process Image, and Diagnostics.

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
| `./tools/check.sh` | Run format, cppcheck, and lint in sequence |
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
| `build-linux-x64.yml` | push / PR to `main` | Build & test (Linux x64); vcpkg packages cached |
| `build-linux-arm64.yml` | push / PR to `main` | Build & test (Linux ARM64) |
| `build-macos-arm64.yml` | push / PR to `main` | Build & test (macOS Apple Silicon) |
| `build-windows-x64.yml` | push / PR to `main` | Build & test (Windows x64) |
| `lint.yml` | push / PR to `main` | clang-format + cpplint checks |
| `cert-renewal.yml` | 1st of every month | Renew Let's Encrypt cert via acme-dns; update `TLS_CERT` / `TLS_KEY` secrets |
| `release.yml` | `v*` tag push | Build all platforms, bundle cert + key from secrets, publish GitHub Release with `.tar.gz`, `.deb`, `.rpm` (Linux), `.zip` (Windows), and `.tar.gz` (macOS arm64) |

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
