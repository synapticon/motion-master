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
      --cert TEXT:FILE          TLS certificate file
      --key TEXT:FILE           TLS private key file
  -d, --driver TEXT [soem]      Fieldbus driver: soem, spoe, igh
  -l, --log-level TEXT [info]   Log level: trace, debug, info, warn, error
```

## Local Development

Production releases bundle a CA-signed TLS certificate for `local.motion-master.synapticon.com`. For local development, use the run script — it generates a short-lived self-signed certificate and launches the binary:

```bash
./tools/run.sh
```

Test the HTTP API (accept the self-signed cert warning):

```bash
curl -k https://localhost:8443/api/version
curl -k https://localhost:8443/api/swagger.yml
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
| `./tools/run.sh` | Generate a tmp self-signed cert and run the binary |
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
| uwebsockets | HTTP and WebSocket server (TLS via OpenSSL) |
| GTest | Unit testing |

## Platform Support

| Platform | Status |
|---|---|
| Linux x86-64 | Primary target |
| Linux ARM | Planned |
| Windows x64 | Planned |

## Real-Time Linux

See [`rt/`](rt/) for the full setup guide and Ansible provisioning playbooks. Covers RT kernel installation, boot parameters, CPU isolation, IRQ affinity, and latency verification on the reference hardware (AAeon / Intel E3940).
