# Development

This document covers working on Motion Master from a source checkout: the toolchain, the build, running the server against a development Console, adding your own HTTP routes, and the tests that need real hardware or a real server. To install a release package instead, read [Installing Motion Master](INSTALLATION.md). To cut a release, read [Releasing](RELEASING.md).

## Prerequisites

Build toolchain for the C++ server:

- CMake 4.0+
- Ninja
- GCC / Clang with C++23 support (or MSVC on Windows)
- Git

Only needed for the parts you actually touch:

| For | Needs |
| --- | --- |
| The web apps, the TS SDK, and the API integration tests | Node.js 22+ and pnpm 10+ (plus Docker for the [API tests](#api-integration-tests)) |
| The [Python reference client](../README.md#clients) and the `jitter_bench` plot script | Python 3.11+ (`matplotlib` for plotting) |
| Static analysis and formatting | `pip install cpplint cmakelang`, plus `clang-format` and `cppcheck` from your package manager |

## Building from source

```bash
git clone git@github.com:synapticon/motion-master.git
cd motion-master
git submodule update --init --recursive

./tools/install-deps.sh          # Debian/Ubuntu or Fedora — see below
./tools/configure.sh
./tools/build.sh
```

`./tools/install-deps.sh` installs everything the repository needs from the distro: the C++23 toolchain, the tools vcpkg uses to build the dependencies from source, the lint and format set (`clang-format`, `clang-tidy`, `cppcheck`, `shellcheck`, plus `cpplint` and `cmake-format` via pipx), `dpkg-dev`/`rpm` for packaging, Node for the `web/` workspace, Ansible and QEMU for `rt/`, and Docker for the `hil/api` tests. It supports **Debian/Ubuntu (apt)** and **Fedora (dnf)**, and takes `--dry-run` to list the packages without installing, plus `--no-qemu` / `--no-docker` to skip the heavier groups. Everything else is built by the build itself — the script deliberately installs no vcpkg dependencies, only the tools that compile them.

Every script defaults to the **`x64-linux-debug`** preset (`CMAKE_BUILD_TYPE=Debug`), so the binary lands in `build/x64-linux-debug/apps/motion_master/`. Pass a preset name as the first argument to build optimised instead:

```bash
./tools/configure.sh x64-linux-release
./tools/build.sh x64-linux-release   # → build/x64-linux-release/apps/motion_master/
```

Use the release preset for anything timing-sensitive — a debug build is unoptimised, so its per-cycle task times are not representative of what the real-time loop achieves.

For the inner development loop, `./tools/build-dev.sh` builds and stamps capabilities as it goes (`--no-setcap` skips the `sudo`), and `./tools/run-dev.sh` starts the result with CORS opened to the Vite dev server and debug-level logging — see [Running locally](#running-locally). Run `./tools/test.sh` separately when you want the test suite.

On Linux, `./tools/build.sh --setcap` runs `sudo setcap cap_sys_nice,cap_net_admin,cap_net_raw,cap_ipc_lock=eip` on the binary after linking — you will be prompted for your password. This is the same set the release packages apply, and it has to be re-run after every relink because the capabilities are attached to the file; see [Linux capabilities](INSTALLATION.md#linux-capabilities) for what each one grants.

## Running locally

Production releases bundle a real Let's Encrypt TLS certificate for `local.motion-master.synapticon.com`, so the PWA at `https://motion-master.synapticon.com` connects without any browser warning.

For development, the run script picks up a cert automatically:

```bash
./tools/run.sh
```

It looks for a certificate in this order:

1. `cert.pem` / `key.pem` next to the binary (present in release builds)
2. `~/.acme.sh/local.motion-master.synapticon.com_ecc/` — if you have `acme.sh` installed locally with the Let's Encrypt cert (no browser warning)
3. Self-signed fallback — generated on the fly; requires accepting a browser security exception once per server restart

If the cert is missing, expired, or expiring soon at startup, the binary fetches a fresh one from the rolling release and installs it before serving (disable by setting `tls.autoUpdate` to `false` in the config). You can also refresh on demand — `motion-master --update-cert` (terminal) or the **Refresh certificate** button on the PWA's Connection page (`POST /api/cert/refresh`). The cert is rotated monthly and published at a stable URL, decoupled from app releases:

```text
https://github.com/synapticon/motion-master/releases/download/tls-cert/{cert,key}.pem
```

Test the API:

```bash
# Real cert (bundled or acme.sh) — verification passes on the name the cert is issued for
curl https://local.motion-master.synapticon.com:61447/api/version

# Self-signed fallback, or any request to https://localhost, needs -k
curl -k https://localhost:61447/api/version
```

### CORS

The server sends `Access-Control-Allow-Origin: https://motion-master.synapticon.com` by default so the production PWA can reach a locally running backend. The origin is the `server.corsOrigin` config setting (there is no CLI flag). When developing the UI against a different origin (e.g. Vite dev server on `http://localhost:5173`), the easiest path is the `CORS_ORIGIN` env var, which `tools/run.sh` bakes into the config it generates:

```bash
# Vite dev server
CORS_ORIGIN=http://localhost:5173 ./tools/run.sh

# Allow any origin (development only — do not use in production)
CORS_ORIGIN='*' ./tools/run.sh
```

`./tools/run-dev.sh` is a shortcut for the first of those — it calls `run.sh` with `CORS_ORIGIN=http://localhost:5173` and `LOG_LEVEL=debug` already set.

Calling the binary directly, set it in your config file instead:

```jsonc
{ "server": { "corsOrigin": "http://localhost:5173" } }
```

## Web UI development

All browser and Node TypeScript lives under [`web/`](../web) — the console PWA, a copy-me starter app, the published SDK, and the shared design system. They are members of one pnpm workspace **rooted at the repository root** (together with `hil/api`), so pnpm commands run from the root, not from `web/`:

```bash
pnpm install            # once, from the repo root
pnpm dev                # console dev server on http://localhost:5173
pnpm build              # regenerate the API client, then build the console
pnpm generate-api       # regenerate the client from swagger.yml on its own
```

Run the server alongside it with `./tools/run-dev.sh`, which sets the CORS origin to the Vite dev server for you. See [`web/README.md`](../web/README.md) for the package layout and the GitHub Pages deployment scheme.

The generated HTTP client (`web/packages/motion-master-client/src/generated/`) is **committed**, so regenerate and commit it whenever `swagger.yml` changes — the `api-client-drift` CI job fails on any diff.

Note that the hosted apps are pinned to the latest `v*` tag rather than to `main`, so the deployed console always matches a released binary. A web-only fix therefore still needs a version bump and a tag to go live; see [Versioning](RELEASING.md#versioning).

## Docker image

Build the runtime image from a source checkout:

```bash
git submodule update --init --recursive
docker build -t motion-master .
```

The build fetches the current `cert.pem`/`key.pem` from the rolling `tls-cert` release and bakes them in — the same single source the running binary's self-heal uses, so no secrets are involved. An offline build bakes empty placeholders instead; the container then self-signs and the startup self-heal fetches a real cert on first run. See [Installation → Docker](INSTALLATION.md#docker) for running the image, the cert discovery order, and the `--cap-add` flags.

### Publishing to Docker Hub

`tools/docker-build.sh` builds the image from the current `VERSION` and tags it with the **bare** version (Docker convention — no `v` prefix) plus a rolling tag: `latest` for a stable release, or `next` for a prerelease (a `-` in the version, e.g. `6.0.0-alpha.34`). It defaults to the `markosankovic/motion-master` repository; override with an argument or the `IMAGE` env var.

```bash
# 1. Authenticate — use a Docker Hub access token, not your password.
#    Create one at Docker Hub → Account Settings → Personal access tokens.
echo "$DOCKERHUB_TOKEN" | docker login -u markosankovic --password-stdin
#    (or interactively, pasting the token at the password prompt)
docker login -u markosankovic

# 2. Build + tag from the current VERSION (version tag + latest/next).
./tools/docker-build.sh
#    …or publish under a different repository:
IMAGE=myuser/motion-master ./tools/docker-build.sh

# 3. Push both tags — the script prints these exact commands for the built version.
#    The repo is auto-created (public) on first push.
docker push markosankovic/motion-master:6.0.0-alpha.34
docker push markosankovic/motion-master:next
```

Prereleases move `next`, not `latest`, so a plain `docker pull markosankovic/motion-master` (which resolves `latest`) always lands on the newest **stable** image.

## Extending the API (C++ routes)

You can add your own HTTP endpoints in C++ without touching the server core. `libs/example` is a copy-me starter (the server-side counterpart of the `web/apps/example` PWA) that registers `GET /api/example/devices`:

```bash
curl -k https://localhost:61447/api/example/devices
```

To add your own:

1. **Copy `libs/example`**, then rename the directory, the `mm::example` namespace, and the `/api/example/...` route prefix.
2. **Put real logic in `*_logic.{h,cc}`** — plain, HTTP-agnostic functions that take a `DeviceManager&` (testable with no server). Format responses in `*_routes.cc` using the `mm::api::sendJson` / `sendError` / `sendStatus` helpers so the content type and CORS headers match the built-in routes.
3. **Add the subdirectory** to the root `CMakeLists.txt` and link your lib into `apps/motion_master`.
4. **Wire it in `main.cc`** before the server starts:

   ```cpp
   httpServer.addRoutes(mm::yourapp::registerRoutes);  // before httpServer.start()
   ```

Your `registerRoutes(uWS::SSLApp&, const mm::api::RouteContext&)` runs once on the HTTP event-loop thread, after the built-in routes and before the catch-all 404. Register only your own paths (e.g. `/api/yourapp/...`) — never the `/api/*` or `/*` wildcards. The transport glue lives in `libs/api` (`mm::api`); the domain layer (`mm::node`) stays free of any HTTP/uWebSockets dependency. These routes are intentionally **not** part of `swagger.yml` — that spec documents the stable built-in API only.

## Developer Scripts

All scripts default to the `x64-linux-debug` preset. Pass a preset name as the first argument to override (e.g. `./tools/build.sh x64-linux-release`).

| Script | Description |
| --- | --- |
| `./tools/configure.sh` | Run CMake configure |
| `./tools/build.sh` | Build all targets (`--setcap` also stamps Linux capabilities, needs `sudo`) |
| `./tools/build-dev.sh` | Build — the inner development loop; stamps capabilities by default (`--no-setcap` to skip the `sudo`) |
| `./tools/run.sh` | Run the binary with the best available TLS cert (real cert if acme.sh is set up, self-signed otherwise) |
| `./tools/run-dev.sh` | Run for UI development — CORS opened to the Vite dev server (`http://localhost:5173`) and debug-level logging |
| `./tools/test.sh` | Run tests |
| `./tools/format.sh` | Auto-format all sources with clang-format |
| `./tools/lint.sh` | Run cpplint (`pip install cpplint` if missing) |
| `./tools/cppcheck.sh` | Run cppcheck static analysis |
| `./tools/format-cmake.sh` | Auto-format all CMake files with cmake-format (`--check` reports without editing; `pip install cmakelang`) |
| `./tools/lint-cmake.sh` | Run cmake-lint over all CMake files (`pip install cmakelang`) |
| `./tools/check.sh` | Run format, cppcheck, lint, and cmake-lint in sequence |
| `./tools/clean.sh` | Remove the build directory |
| `./tools/docs.sh` | Build the Doxygen documentation (`docs` target) |
| `./tools/code-stats.sh` | Report per-file / per-directory C++ line counts (code, comment, blank) |
| `./tools/release-stats.sh` | Write `STATS.md`: the download count of every release asset, read from the GitHub API (needs `gh` and `jq`) |
| `./tools/bump-version.sh <version>` | Bump the project semver everywhere (see [Versioning](RELEASING.md#versioning)) |
| `./tools/package.sh [preset]` | Build `.deb` and `.rpm` packages (requires `cert.pem`/`key.pem` in the build dir) |
| `./tools/docker-build.sh [image]` | Build + tag the Docker image from the current `VERSION` (version tag + `latest`/`next`) |

### Code Quality Tools

- **`format`** — runs `clang-format` over all `.cc`/`.h` sources and rewrites them in-place. Enforces Google style with a 100-column limit as defined in `.clang-format`. CI fails if any file is not already formatted.
- **`lint`** — runs `cpplint` to check for include order, deprecated constructs, and header guards. Configured via `CPPLINT.cfg`. Naming conventions are enforced in code review, not by this tool.
- **`cppcheck`** — static analysis that catches bugs the compiler doesn't warn about: null pointer dereferences, out-of-bounds access, uninitialized variables, resource leaks, etc. Runs with `warning,style,performance,portability` checks at `--std=c++23` and exits non-zero on any finding.

## Dependencies

Managed via [vcpkg](https://vcpkg.io). No manual installation needed — vcpkg downloads and builds everything on first configure.

| Package | Purpose |
| --- | --- |
| SOEM | EtherCAT master stack — raw frames, mailbox/CoE, FoE, ESC registers, process-data mapping |
| CLI11 | Command-line argument parsing |
| spdlog | Structured logging |
| nlohmann-json | JSONC config file parsing (comments enabled) and HTTP response serialization |
| neargye-semver | Semantic versioning |
| uwebsockets | HTTP and WebSocket server (TLS via OpenSSL) |
| OpenSSL | TLS termination for both servers, and certificate/key validation in the self-heal path |
| curl | Fetching a fresh TLS certificate from the rolling `tls-cert` release |
| GTest | Unit testing |

On Windows, SOEM's NIC driver additionally builds against the [Npcap SDK](https://npcap.com/#sdk), vendored in-repo under `extern/` so it survives vcpkg binary-cache restores. The Npcap *driver* is a runtime dependency — see [Prerequisites](../README.md#prerequisites).

## Real-Time Linux

Motion Master targets `CONFIG_PREEMPT_RT` kernels for hard real-time operation. The `GameLoop` sets `SCHED_FIFO` priority 80 and calls `mlockall` before entering the cycle loop. The cycle timer uses `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` so scheduling jitter in one cycle never accumulates into drift.

## Hardware-in-the-Loop Tests

The `hil/` directory holds tests that need something a unit test cannot provide — real OS scheduling, or a real running server. They are not part of the CTest suite.

`jitter_bench` is built with the rest of the project on Linux and macOS (it is skipped on Windows) and needs root, or `CAP_SYS_NICE` + `CAP_IPC_LOCK`, to produce valid results.

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

The plot shows a time-series with P99/P99.9 reference lines and a jitter histogram. The terminal output prints min/max/mean/stddev/P50/P95/P99/P99.9 and an overrun count. Compare a standard kernel against `PREEMPT_RT` by running with `--workload 300` (a realistic 1 ms cycle budget) on each. The `Jitter Bench` workflow runs the same benchmark on demand on a `PREEMPT_RT` CI machine, with duration, period, and workload as dispatch inputs.

### api integration tests

[`hil/api`](../hil/api) drives the published TypeScript SDK against a real server with Vitest, so one run exercises the binary, the HTTP/WebSocket contract, and the client library together. The Docker lifecycle is fully managed by the test setup — no manual server startup:

```bash
pnpm install                # from the repo root — first time only
pnpm test:api               # build image → start container → run tests → stop & remove
```

The image runs with `--network host` because the server binds to `127.0.0.1`. Set `MM_SKIP_DOCKER=1` to skip Docker entirely and test against an already-running instance, e.g. one started by `./tools/run.sh`.
