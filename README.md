# Motion Master

Next-generation motion control software for SOMANET servo drives. Browser-based control interface, real-time process data exchange, and a secure HTTP API and WebSocket interface — control from any language, any tool, any AI agent.

See [FEATURES.md](FEATURES.md) for a full catalog of capabilities.

## Architecture

Design documents (the Mermaid diagrams render natively on GitHub):

- [Class diagram](https://github.com/synapticon/motion-master/blob/main/docs/CLASS_DIAGRAM.md) — class structure, ownership, and inheritance.
- [RT scheduling primer](https://github.com/synapticon/motion-master/blob/main/docs/RT_SCHEDULING.md) — `SCHED_FIFO`, `mlockall`, and absolute-deadline sleeping: the three primitives the cycle depends on.
- [Threading model](https://github.com/synapticon/motion-master/blob/main/docs/THREADS.md) — the built-in threads, the RT cycle, and why the RT loop never takes a lock.
- [Locking and synchronization](https://github.com/synapticon/motion-master/blob/main/docs/LOCKING.md) — every mutex, the lock ordering, and the lock-free protocols that carry the RT path.
- [LAN deployment](https://github.com/synapticon/motion-master/blob/main/docs/LAN_DEPLOYMENT.md) — running the server on a separate machine (Raspberry Pi, industrial PC) and reaching it from the Console over the network.

## Installation

Release packages are available on the [Releases](../../releases) page. Every release ships binaries for Linux (x86-64 and aarch64), Windows, and macOS:

| Artefact | Format | Install on |
| --- | --- | --- |
| `motion-master-<version>-linux-x64.tar.gz` | Tarball | Any Linux x86-64 |
| `motion-master-<version>-amd64.deb` | Debian package | Ubuntu / Debian (x86-64) |
| `motion-master-<version>-x86_64.rpm` | RPM package | Fedora / RHEL / openSUSE (x86-64) |
| `motion-master-<version>-linux-arm64.tar.gz` | Tarball | Any Linux aarch64 |
| `motion-master-<version>-arm64.deb` | Debian package | Debian 13 / Raspberry Pi OS trixie (aarch64) |
| `motion-master-<version>-aarch64.rpm` | RPM package | Fedora / openSUSE (aarch64) |
| `motion-master-<version>-windows-x64.zip` | Zip archive | Windows x64 |
| `motion-master-<version>-macos-arm64.tar.gz` | Tarball | macOS (Apple Silicon) |

The Linux `.deb`/`.rpm` packages install to `/opt/motion-master/` with a `/usr/local/bin/motion-master` symlink.

> **aarch64 note:** the arm64 artefacts are built on **Debian 13 (trixie)** and need glibc 2.38 or newer, so they run on Debian 13 and Raspberry Pi OS trixie. Debian 12 (bookworm, glibc 2.36) is too old — [build from source](#building-from-source) there.

### Debian / Ubuntu

```bash
sudo apt install ./motion-master-<version>-amd64.deb  # install or upgrade (aarch64: -arm64.deb)
sudo apt remove motion-master                         # remove (leaves cert.pem / key.pem)
sudo apt purge motion-master                          # full removal including certs
```

The `postinst` script automatically sets the four required capabilities (`cap_sys_nice`, `cap_net_admin`, `cap_net_raw`, `cap_ipc_lock`) on the binary — see [Linux capabilities](#linux-capabilities) for what each one does. On upgrade the capabilities are re-applied to the new binary automatically.

> **Note:** `apt remove` leaves `cert.pem` and `key.pem` behind as conffiles. Use `apt purge` for a complete uninstall.

### Fedora / RHEL / openSUSE

```bash
sudo dnf install ./motion-master-<version>-x86_64.rpm     # Fedora / RHEL (install or upgrade)
sudo zypper install ./motion-master-<version>-x86_64.rpm  # openSUSE (install or upgrade)
sudo dnf remove motion-master                             # full removal
```

On aarch64 use `motion-master-<version>-aarch64.rpm` instead.

On uninstall, unmodified `cert.pem` and `key.pem` are removed automatically. If you replaced them with your own, they are saved as `cert.pem.rpmsave` / `key.pem.rpmsave`.

### Tarball

```bash
tar -xzf motion-master-<version>-linux-x64.tar.gz  # aarch64: -linux-arm64.tar.gz
cd motion-master-<version>-linux-x64
sudo ./setup.sh  # sets capabilities once; re-run after any OS update that resets them
./motion-master --help
```

Alongside the binary the tarball carries `setup.sh`, a `SETUP.md` with the same first-run notes, the bundled `cert.pem`/`key.pem`, and the annotated `motion-master.example.jsonc`.

### Linux capabilities

On Linux the binary needs four capabilities. All three Linux install paths apply the same set — the deb `postinst`, the rpm `%post` scriptlet, and the tarball's `setup.sh` all run:

```bash
setcap cap_sys_nice,cap_net_admin,cap_net_raw,cap_ipc_lock=eip <install-dir>/motion-master
```

| Capability | What the binary does with it | Missing it |
| --- | --- | --- |
| `cap_net_raw` | Opens an `AF_PACKET` raw socket. EtherCAT is a bare Ethernet protocol (EtherType `0x88A4`) with no IP layer, so SOEM writes and reads frames directly on the wire instead of going through a normal socket. | `POST /api/init` fails when a SOEM driver is requested — no fieldbus |
| `cap_net_admin` | Puts the NIC into promiscuous mode. EtherCAT frames come back addressed to the slaves, not to the host MAC, so without promiscuous mode the kernel drops every reply. | Slave discovery/scan finds nothing, or `init` fails |
| `cap_sys_nice` | Raises the game-loop thread to `SCHED_FIFO` priority 80, so the real-time cycle is not preempted by ordinary `SCHED_OTHER` work. | Warning at startup; the loop runs at normal priority with no timing guarantees |
| `cap_ipc_lock` | Calls `mlockall(MCL_CURRENT \| MCL_FUTURE)` to pin all process pages in RAM, so a mid-cycle page fault cannot inject an unbounded latency spike. | Warning at startup; shows as `mlockall: no` on the console's Game Loop page |

The first two are the EtherCAT pair, the last two the real-time pair. The real-time pair is best-effort — the binary logs a warning and keeps running without either — so a plain `./motion-master` on a machine with no capabilities set still serves the HTTP API and the WebSocket; it just cannot talk to a fieldbus or hold a deterministic cycle.

The `=eip` suffix puts each capability in the file's **e**ffective, **i**nheritable and **p**ermitted sets, which is what lets an unprivileged user run the binary and still get them — no `sudo` and no root-owned process. Two consequences worth knowing:

- Capabilities live in an extended attribute on the *file*, so they are lost whenever the file is replaced — a rebuild, a `cp`, or a manual binary swap. Re-run `setup.sh` (or `./tools/build.sh --setcap`) afterwards. Package upgrades handle this themselves.
- They require a filesystem with extended-attribute support, mounted without `nosuid`. On a `nosuid` mount, or over NFS without xattrs, `setcap` succeeds but the kernel ignores the capabilities at exec time — run the binary from local storage instead.

Inside a container file capabilities are ignored entirely; see [Docker → Capabilities](#capabilities) for the `--cap-add` equivalents.

### Windows

Unzip `motion-master-<version>-windows-x64.zip` — it contains `motion-master.exe`, the bundled `cert.pem`/`key.pem`, an auto-loaded `motion-master.jsonc` (preset to a 4 ms real-time cycle, robust on stock Windows timers), the annotated `motion-master.example.jsonc`, and the required vcpkg runtime DLLs. Install the two runtime dependencies listed under [Usage → Prerequisites](#prerequisites) (Visual C++ Redistributable and Npcap), then run `motion-master.exe` from the extracted directory — it picks up the neighbouring `motion-master.jsonc` automatically (edit it to change the cycle period or any other setting).

`motion-master.exe` is **Authenticode code-signed** (with an RFC 3161 timestamp), so Windows shows Synapticon as the verified publisher instead of an "unknown publisher" SmartScreen block. Signing happens on a self-hosted runner holding the certificate token, as a final step of the release workflow — it replaces the zip asset in place, so the published archive is always the signed one.

### macOS (Apple Silicon)

```bash
tar -xzf motion-master-<version>-macos-arm64.tar.gz
cd motion-master-<version>-macos-arm64
xattr -dr com.apple.quarantine motion-master  # required once — see below
sudo ./motion-master
```

Two macOS specifics, both covered by the bundled `SETUP.md`:

- **The build is not notarized**, so Gatekeeper quarantines it on download and refuses to launch it. Clear the quarantine attribute as shown above (or right-click → **Open** once in Finder and confirm the dialog, which whitelists that exact binary).
- **`sudo` is needed** for the same two reasons capabilities are needed on Linux: the fieldbus opens the NIC through the root-only BPF devices (`/dev/bpf*`), so `POST /api/init` with `driver: soem` fails without it, and raising the game-loop thread to `SCHED_FIFO` is privileged. Without `sudo` the server still runs and serves the API — it just cannot reach a fieldbus and logs a warning as the RT loop drops to normal priority. `SETUP.md` also shows how to grant BPF access to your user instead, if you would rather not use `sudo`.

### Docker

Motion Master also runs as a container image. Building the image and publishing it to a registry is covered under [Development → Docker image](#docker-image); this section covers running an image you built locally or pulled from a registry.

`--network host` is required on all `docker run` commands — the server binds to `127.0.0.1` and Docker's port forwarding never reaches the loopback interface.

#### TLS certificates

Every image bakes `cert.pem`/`key.pem` in at build time by fetching them from the rolling `tls-cert` release (see [Development → Docker image](#docker-image)); an image built offline ships with empty placeholders and self-heals on start instead. The runtime discovery order is the same as `tools/run.sh`:

```bash
# Bundled cert used automatically
docker run --rm --network host motion-master

# Mount an acme.sh cert from the host instead (e.g. for a locally-issued cert)
docker run --rm --network host \
  -v "$HOME/.acme.sh/local.motion-master.synapticon.com_ecc:/root/.acme.sh/local.motion-master.synapticon.com_ecc:ro" \
  motion-master
```

#### Updating an expired cert on an older image

The bundled cert is renewed monthly, but an older image keeps its original cert. By default the container self-heals: if the baked-in cert is missing, expired, or expiring soon at startup it fetches a fresh one from the rolling release before serving (set `"tls": { "autoUpdate": false }` in a mounted config to disable, e.g. for air-gapped hosts). To pin your own cert instead, override it at runtime — the volume mount shadows the baked-in file:

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

#### Capabilities

Docker drops most Linux capabilities by default. On a bare-metal install `postinst`/`setup.sh` stamps the binary with `setcap` so any user can run it and it receives the required capabilities automatically. Inside a container, file capabilities are ignored — you grant the equivalent capabilities to the container process with `--cap-add` at `docker run` time instead.

| Capability | `docker run` flag | Required for |
| --- | --- | --- |
| `CAP_NET_RAW` | `--cap-add NET_RAW` | SOEM sending/receiving raw EtherCAT frames |
| `CAP_NET_ADMIN` | `--cap-add NET_ADMIN` | SOEM putting the NIC into promiscuous mode |
| `CAP_SYS_NICE` | `--cap-add SYS_NICE` | `SCHED_FIFO` priority on the real-time game loop |
| `CAP_IPC_LOCK` | `--cap-add IPC_LOCK` | `mlockall()` pinning process memory for RT |

See [Linux capabilities](#linux-capabilities) for what each one actually does and how the binary behaves without it: missing RT caps produce a warning and the loop runs without RT guarantees; missing EtherCAT caps cause `POST /api/init` to fail when a SOEM driver is requested.

`--ulimit memlock=-1` is also required alongside `CAP_IPC_LOCK` — without it the kernel rejects `mlockall()` even when the capability is present.

To auto-initialise the fieldbus in a container, mount a JSONC config with a `fieldbus` block (`{ "driver": "soem", "adapter": "eth0" }`) and pass `--config` — there are no `--driver`/`--adapter` flags. Omit it to defer initialisation to `POST /api/init` at runtime.

```bash
# EtherCAT only (no RT requirement on the host kernel)
docker run --rm --network host \
  --cap-add NET_ADMIN --cap-add NET_RAW \
  -v "$(pwd)/motion-master.jsonc:/config.jsonc:ro" \
  motion-master --config /config.jsonc

# RT scheduling only (PREEMPT_RT host kernel required)
docker run --rm --network host \
  --cap-add SYS_NICE --cap-add IPC_LOCK --ulimit memlock=-1 \
  motion-master

# Full EtherCAT + RT
docker run --rm --network host \
  --cap-add NET_ADMIN --cap-add NET_RAW \
  --cap-add SYS_NICE --cap-add IPC_LOCK --ulimit memlock=-1 \
  -v "$(pwd)/motion-master.jsonc:/config.jsonc:ro" \
  motion-master --config /config.jsonc
```

#### Run from a registry

Pull and run a published image, mounting `cert.pem`/`key.pem` from the current directory. Source paths **must be absolute** — a relative `-v cert.pem:...` is interpreted as a *named volume*, not your local file, and the container silently falls back to a self-signed cert. Use `$(pwd)/` to force an absolute path:

```bash
docker run --rm --network host \
  --cap-add NET_RAW --cap-add NET_ADMIN \
  --cap-add SYS_NICE --cap-add IPC_LOCK --ulimit memlock=-1 \
  -v "$(pwd)/cert.pem:/opt/motion-master/cert.pem:ro" \
  -v "$(pwd)/key.pem:/opt/motion-master/key.pem:ro" \
  markosankovic/motion-master:latest
```

`docker run` pulls the image automatically if it isn't present locally. Drop the two `-v` flags to let the container self-heal its cert instead (fetches a fresh Let's Encrypt cert at startup; see **Updating an expired cert** above).

## Usage

### First run

Start the server (on Linux from a package install, `motion-master` is already on `PATH`):

```bash
motion-master           # macOS: sudo ./motion-master   Windows: motion-master.exe
```

It serves two ports, both bound to `127.0.0.1` and each on its own event loop and thread, so a slow HTTP request can never stall the live data stream:

| Port | Purpose | Config key |
| --- | --- | --- |
| `61447` | HTTPS API — everything request/response (init, scan, SDO, FoE, state, parameters) | `server.httpPort` |
| `62281` | Secure WebSocket (`wss://`) — monitoring batches and notifications out, topic subscriptions in | `server.wsPort` |

Then open the console and point it at your machine:

```bash
motion-master --open    # opens https://motion-master.synapticon.com/apps/console/
```

The console is a PWA hosted by Synapticon; it talks to the server running on your machine over `https://local.motion-master.synapticon.com:61447` (a DNS name that resolves to `127.0.0.1`, which is why the bundled certificate is valid for it). Nothing is uploaded — the page is the only thing that comes from the network.

The fieldbus does **not** come up on its own unless you configure it to. Either bring it up from the console, or do it over the API:

```bash
motion-master --list-adapters   # find your NIC

base=https://local.motion-master.synapticon.com:61447
curl "$base/api/version"
curl -X POST "$base/api/init" \
  -H 'Content-Type: application/json' \
  -d '{"driver":"soem","adapter":"eth0"}'   # then POST /api/scan to discover drives
```

Use the `local.motion-master.synapticon.com` name rather than `localhost` — it resolves to `127.0.0.1` all the same, but it is the name the bundled certificate is issued for, so TLS verification passes. Against `https://localhost` the same request fails on a hostname mismatch unless you add `-k`.

To skip that step on every start, put a `fieldbus` block in a [config file](#configuration) and the driver initialises at startup instead.

The server also serves its own OpenAPI spec at `GET /api/swagger.yml` — the contract for everything above, and what the [reference clients](#clients) read at startup so they never hold a hardcoded URL. The same spec is rendered as browsable API Docs inside the console.

### Prerequisites

Requirements for running a release binary (building from source has its own — see [Development](#development)):

- **Linux (both architectures):** glibc **2.38** or newer and libstdc++ from GCC 13.2 or newer (`GLIBCXX_3.4.32`) — the only dynamic dependencies are `libc`, `libm`, `libstdc++` and `libgcc_s`. The floor comes from the symbols the binary references, not from the distro it was built on: the x64 artefacts are built on Ubuntu 24.04 and the arm64 ones on Debian 13 (trixie), and both land on the same 2.38 requirement. So Ubuntu 24.04+, Debian 13+ and Raspberry Pi OS trixie all work, while Ubuntu 22.04 (glibc 2.35) and Debian 12 (bookworm, 2.36) fail at load with a `version 'GLIBC_2.3x' not found` error — [build from source](#building-from-source) there.
  Verify a binary's own requirement with `readelf -V ./motion-master | grep GLIBC_` (highest version wins).
- **Windows:** two runtime dependencies for the packaged binary:
  - [Microsoft Visual C++ Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist?view=msvc-170) (x64) — the MSVC runtime the binary is linked against
  - [Npcap](https://npcap.com) — raw EtherCAT packet capture. **⚠️ You must tick "Install Npcap in WinPcap API-compatible Mode" in the installer — this is not optional.** Motion Master links `wpcap.dll` at load time, and only compatibility mode places it where the loader looks (`C:\Windows\System32`); a default install hides it under `System32\Npcap\`, so `motion-master.exe` refuses to start with *"The code execution cannot proceed because wpcap.dll was not found"*. If you already installed Npcap without the box checked, simply re-run the installer and tick it.

### Platform Support

| Platform | Status |
| --- | --- |
| Linux x86-64 | Primary target — `.tar.gz`, `.deb`, `.rpm` |
| Linux aarch64 | Supported — `.tar.gz`, `.deb`, `.rpm` (built on Debian 13) |
| Windows x64 | Supported — `.zip` |
| macOS (Apple Silicon) | Supported — `.tar.gz` |

### Command-line options

The command line carries only *actions* and the cert-fetch source URLs. Every tunable **setting** — ports, fieldbus driver/adapter, log level, CORS origin, TLS cert/key paths, and cert auto-update — lives in a JSONC config file (see [`motion-master.example.jsonc`](apps/motion_master/motion-master.example.jsonc)). Motion Master loads that file from one of two places: the path you pass with `-c`/`--config`, or, absent that, a `motion-master.jsonc` sitting next to the executable (auto-discovered — `--config` wins over it). With neither, all built-in defaults apply. The Windows release ships such a `motion-master.jsonc` preconfigured to a 4 ms real-time cycle (robust on stock Windows timers); Linux and macOS keep the 1 ms default and ship only the annotated example.

```text
motion-master [OPTIONS]

OPTIONS:
  -h, --help              Print this help message and exit
      --version           Display program version information and exit
      --list-adapters     Print network adapters (MAC -> interface) and exit
  -c, --config TEXT:FILE  Path to a JSONC config file — ports, fieldbus, log level,
                          and TLS live only in such a file (see motion-master.example.jsonc).
                          Overrides the motion-master.jsonc auto-discovered next to the executable
      --open              Open https://motion-master.synapticon.com/apps/console/ in the
                          default browser
      --update-cert       Download a fresh TLS cert/key, install them at the configured
                          (or default) path, and exit
      --cert-url TEXT     Source URL for the TLS certificate
                          [default: .../releases/download/tls-cert/cert.pem]
      --key-url TEXT      Source URL for the TLS private key
                          [default: .../releases/download/tls-cert/key.pem]
```

On Linux, `motion-master` requires raw socket and RT scheduling capabilities. Release packages set all four automatically (deb `postinst`, rpm `%post`, tarball `setup.sh`) — see [Linux capabilities](#linux-capabilities). Without them the binary still runs and serves the API, but EtherCAT initialisation fails and the game loop cannot go real-time.

### Configuration

Every setting lives in a JSONC config file — comments are allowed, all keys are optional, and each one falls back to a built-in default. [`motion-master.example.jsonc`](apps/motion_master/motion-master.example.jsonc) documents every key inline with its default; copy it to `motion-master.jsonc` next to the binary (auto-discovered) or pass it with `--config`. The top-level blocks:

| Block | What it covers |
| --- | --- |
| `server` | `httpPort` (61447), `wsPort` (62281), `corsOrigin` |
| `fieldbus` | `driver` + `adapter` to initialise at startup — omit the block to wait for `POST /api/init` instead. Also `mailboxStatusFmmu`, which must stay `false` on TI PRU-ICSS ESCs |
| `logLevel` | `trace` … `critical` \| `off` (default `info`) |
| `gameLoop` | `periodUs` — the real-time cycle period (1000 = 1 ms; the Windows release ships 4000) |
| `recorder` | `capacity` of the lossless process-data ring in cycles (default 300000 ≈ 5 min at 1 ms for one drive, ≈ 38 MB) and `dumpDir` for `.mmpd` dumps |
| `parameterCache` | On-disk cache of CoE object-dictionary *definitions* (never values) keyed by vendor/product/revision — `enabled`, `cacheAllVendors`, `directory` |
| `parameters` | `readObjectDictionaryOnPreop` (enumerate on the first PRE-OP) and `useCompleteAccess` (one CoE Complete Access upload per ARRAY/RECORD instead of per-subindex reads) |
| `tls` | `certPath`, `keyPath` (empty = auto-discover), and `autoUpdate` — set `false` for air-gapped installs |

If a raised `skippedCycles` count shows up right after start (visible on the console's Game Loop page), the configured `gameLoop.periodUs` is too aggressive for that machine's timer resolution — raise it. A 1 ms cycle is realistic on a `PREEMPT_RT` Linux host; stock Windows often cannot sustain better than ~1.5 ms, which is why the Windows release defaults to 4 ms.

### Clients

The API is plain HTTPS + JSON, so any language works. Two reference clients ship in the repo:

- **[`clients/python`](clients/python)** — a standalone reference client (`requests` + `websockets`). Each example runs on its own, and the numbered set reads in order as a walkthrough of the whole fieldbus lifecycle: bring up a driver, scan, climb the AL states, read SDOs and files, dump parameters, stream live process data, and tear back down. It resolves every call by `operationId` against the spec it fetches from `GET /api/swagger.yml` at startup, so it holds no hardcoded URLs and adapts to whichever server version it is pointed at. Copy the two client files anywhere — no checkout needed.

  ```bash
  cd clients/python
  source setup.sh                      # venv + pip install -r requirements.txt
  cp config.example.toml config.toml   # point it at your server
  python examples/01_init.py
  ```

- **[`@synapticon/motion-master-client`](web/packages/motion-master-client)** — the published, isomorphic TypeScript SDK (browser and Node): the generated HTTP client plus the WebSocket connection, and a parser for `.mmpd` recorder dumps. It ships on npm at the same version as the server binary, so `client@X` is known-good against `binary@X`. This is what the console and the integration tests use.

  ```bash
  npm install @synapticon/motion-master-client
  ```

## Development

### Prerequisites

Build toolchain for the C++ server:

- CMake 4.0+
- Ninja
- GCC / Clang with C++23 support (or MSVC on Windows)
- Git

Only needed for the parts you actually touch:

| For | Needs |
| --- | --- |
| The web apps, the TS SDK, and the API integration tests | Node.js 22+ and pnpm 10+ (plus Docker for the [API tests](#api-integration-tests)) |
| The [Python reference client](#clients) and the `jitter_bench` plot script | Python 3.11+ (`matplotlib` for plotting) |
| Static analysis and formatting | `pip install cpplint cmakelang`, plus `clang-format` and `cppcheck` from your package manager |

### Building from source

```bash
git clone git@github.com:synapticon/motion-master.git
cd motion-master
git submodule update --init --recursive

./tools/install-deps.sh          # Debian/Ubuntu or Fedora — see below
./tools/configure.sh
./tools/build.sh
```

`./tools/install-deps.sh` installs everything the repository needs from the distro: the C++23
toolchain, the tools vcpkg uses to build the dependencies from source, the lint and format set
(`clang-format`, `clang-tidy`, `cppcheck`, `shellcheck`, plus `cpplint` and `cmake-format` via
pipx), `dpkg-dev`/`rpm` for packaging, Node for the `web/` workspace, Ansible and QEMU for `rt/`,
and Docker for the `hil/api` tests. It supports **Debian/Ubuntu (apt)** and **Fedora (dnf)**, and
takes `--dry-run` to list the packages without installing, plus `--no-qemu` / `--no-docker` to skip
the heavier groups. Everything else is built by the build itself — the script deliberately installs
no vcpkg dependencies, only the tools that compile them.

Every script defaults to the **`x64-linux-debug`** preset (`CMAKE_BUILD_TYPE=Debug`), so the binary lands in `build/x64-linux-debug/apps/motion_master/`. Pass a preset name as the first argument to build optimised instead:

```bash
./tools/configure.sh x64-linux-release
./tools/build.sh x64-linux-release   # → build/x64-linux-release/apps/motion_master/
```

Use the release preset for anything timing-sensitive — a debug build is unoptimised, so its per-cycle task times are not representative of what the real-time loop achieves.

For the inner development loop, `./tools/build-dev.sh` builds and stamps capabilities as it goes (`--no-setcap` skips the `sudo`), and `./tools/run-dev.sh` starts the result with CORS opened to the Vite dev server and debug-level logging — see [Running locally](#running-locally). Run `./tools/test.sh` separately when you want the test suite.

On Linux, `./tools/build.sh --setcap` runs `sudo setcap cap_sys_nice,cap_net_admin,cap_net_raw,cap_ipc_lock=eip` on the binary after linking — you will be prompted for your password. This is the same set the release packages apply, and it has to be re-run after every relink because the capabilities are attached to the file; see [Linux capabilities](#linux-capabilities) for what each one grants.

### Running locally

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

#### CORS

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

### Web UI development

All browser and Node TypeScript lives under [`web/`](web) — the console PWA, a copy-me starter app, the published SDK, and the shared design system. They are members of one pnpm workspace **rooted at the repository root** (together with `hil/api`), so pnpm commands run from the root, not from `web/`:

```bash
pnpm install            # once, from the repo root
pnpm dev                # console dev server on http://localhost:5173
pnpm build              # regenerate the API client, then build the console
pnpm generate-api       # regenerate the client from swagger.yml on its own
```

Run the server alongside it with `./tools/run-dev.sh`, which sets the CORS origin to the Vite dev server for you. See [`web/README.md`](web/README.md) for the package layout and the GitHub Pages deployment scheme.

The generated HTTP client (`web/packages/motion-master-client/src/generated/`) is **committed**, so regenerate and commit it whenever `swagger.yml` changes — the `api-client-drift` CI job fails on any diff.

Note that the hosted apps are pinned to the latest `v*` tag rather than to `main`, so the deployed console always matches a released binary. A web-only fix therefore still needs a version bump and a tag to go live; see [Versioning](#versioning).

### Docker image

Build the runtime image from a source checkout:

```bash
git submodule update --init --recursive
docker build -t motion-master .
```

The build fetches the current `cert.pem`/`key.pem` from the rolling `tls-cert` release and bakes them in — the same single source the running binary's self-heal uses, so no secrets are involved. An offline build bakes empty placeholders instead; the container then self-signs and the startup self-heal fetches a real cert on first run. See [Installation → Docker](#docker) for running the image, the cert discovery order, and the `--cap-add` flags.

#### Publishing to Docker Hub

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

### Extending the API (C++ routes)

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

### Developer Scripts

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
| `./tools/bump-version.sh <version>` | Bump the project semver everywhere (see [Versioning](#versioning)) |
| `./tools/package.sh [preset]` | Build `.deb` and `.rpm` packages (requires `cert.pem`/`key.pem` in the build dir) |
| `./tools/docker-build.sh [image]` | Build + tag the Docker image from the current `VERSION` (version tag + `latest`/`next`) |

#### Code Quality Tools

- **`format`** — runs `clang-format` over all `.cc`/`.h` sources and rewrites them in-place. Enforces Google style with a 100-column limit as defined in `.clang-format`. CI fails if any file is not already formatted.
- **`lint`** — runs `cpplint` to check for include order, deprecated constructs, and header guards. Configured via `CPPLINT.cfg`. Naming conventions are enforced in code review, not by this tool.
- **`cppcheck`** — static analysis that catches bugs the compiler doesn't warn about: null pointer dereferences, out-of-bounds access, uninitialized variables, resource leaks, etc. Runs with `warning,style,performance,portability` checks at `--std=c++23` and exits non-zero on any finding.

### Dependencies

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

On Windows, SOEM's NIC driver additionally builds against the [Npcap SDK](https://npcap.com/#sdk), vendored in-repo under `extern/` so it survives vcpkg binary-cache restores. The Npcap *driver* is a runtime dependency — see [Prerequisites](#prerequisites).

### Real-Time Linux

Motion Master targets `CONFIG_PREEMPT_RT` kernels for hard real-time operation. The `GameLoop` sets `SCHED_FIFO` priority 80 and calls `mlockall` before entering the cycle loop. The cycle timer uses `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` so scheduling jitter in one cycle never accumulates into drift.

### Hardware-in-the-Loop Tests

The `hil/` directory holds tests that need something a unit test cannot provide — real OS scheduling, or a real running server. They are not part of the CTest suite.

`jitter_bench` is built with the rest of the project on Linux and macOS (it is skipped on Windows) and needs root, or `CAP_SYS_NICE` + `CAP_IPC_LOCK`, to produce valid results.

#### jitter_bench

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

#### api integration tests

[`hil/api`](hil/api) drives the published TypeScript SDK against a real server with Vitest, so one run exercises the binary, the HTTP/WebSocket contract, and the client library together. The Docker lifecycle is fully managed by the test setup — no manual server startup:

```bash
pnpm install                # from the repo root — first time only
pnpm test:api               # build image → start container → run tests → stop & remove
```

The image runs with `--network host` because the server binds to `127.0.0.1`. Set `MM_SKIP_DOCKER=1` to skip Docker entirely and test against an already-running instance, e.g. one started by `./tools/run.sh`.

### Versioning

All components — C++ backend, React UI, OpenAPI spec, and npm packages — share a single semver. `VERSION` (repo root) is the **canonical source**: a one-line plain-text file (e.g. `6.0.0-alpha.31`). There is no auto-increment — a human picks the next version. Everything else is either *derived* from `VERSION` at build time or *kept in sync* with it by the bump script.

#### Bumping

Never edit `VERSION` by hand. Run the bump script with the new version:

```bash
./tools/bump-version.sh 6.1.0
./tools/bump-version.sh 6.1.0-alpha.0
```

It writes `VERSION`, then propagates the value to every location that *isn't* auto-derived: `vcpkg.json`, the `package.json` manifests (root workspace + `motion-master`, `motion-master-client`, `hil/api`), `swagger.yml` (`info.version`), the `version_test.cc` assertion, and the UI sidebar badge in `RootLayout.tsx`.

#### How it reaches the C++ binary

CMake does the propagation into native code at configure time — `version.h` is **generated, never edited by hand**:

1. `CMakeLists.txt` reads the file into a variable: `file(STRINGS "${CMAKE_SOURCE_DIR}/VERSION" MM_VERSION)`.
2. `libs/core/CMakeLists.txt` runs `configure_file(version.h.in …)`, substituting `@MM_VERSION@` in the template to produce the build-dir `version.h`:

   ```cpp
   constexpr std::string_view kVersion = "6.0.0-alpha.31";
   static_assert(semver::valid(kVersion));  // build fails on a malformed version
   ```

The `static_assert` is a compile-time guard: a malformed version in `VERSION` breaks the build rather than shipping a bad string. The `Doxyfile` version is propagated the same way.

#### Releasing

After bumping, commit the changed files, then push a `v<version>` tag to trigger the release workflow:

```bash
git add -A
git commit -m "chore: bump version to 6.1.0"
git tag v6.1.0
git push && git push --tags
```

The `v*` tag builds the platform binaries **and** publishes `@synapticon/motion-master-client@<version>` to npm (prereleases under the `next` dist-tag). Two drift nets back the sync: `version_test.cc` fails if its hard-coded string falls out of step, and the `api-client-drift` CI job fails if the committed API client is stale against `swagger.yml`.

### CI

| Workflow | Trigger | Purpose |
| --- | --- | --- |
| `build-linux-x64.yml` | push / PR to `main` | Build & test (Linux x64); vcpkg packages cached |
| `build-linux-arm64.yml` | push / PR to `main` | Build & test (Linux ARM64) |
| `build-macos-arm64.yml` | push / PR to `main` | Build & test (macOS Apple Silicon) |
| `build-windows-x64.yml` | push / PR to `main` | Build & test (Windows x64) |
| `lint.yml` | push / PR to `main` | Five gates: clang-format, cpplint, cppcheck, `api-client-drift` (the committed TS client must match `swagger.yml`), and `python-client-example` (every `operationId` the Python examples call must still resolve against the spec) |
| `api-tests.yml` | push to `main` | Run the [`hil/api`](#api-integration-tests) HTTP + WebSocket integration tests against a containerised server |
| `cert-renewal.yml` | 1st of every month | Renew Let's Encrypt cert via acme-dns; publish it to the rolling `tls-cert` release |
| `deploy-pages.yml` | push to `main`, `v*` tag | Publish `motion-master.synapticon.com` — landing page, `/docs` Doxygen, the `swagger.yml` spec, and each PWA under `/apps/<name>/`. Docs and landing track `main`; the **apps are pinned to the latest `v*` tag** so the hosted console always matches a released binary |
| `jitter.yml` | manual dispatch | Run `jitter_bench` on a `PREEMPT_RT` CI machine (duration / period / workload as inputs) |
| `release.yml` | `v*` tag push | Build all platforms, bundle cert + key from the rolling `tls-cert` release, publish GitHub Release with `.tar.gz`, `.deb`, `.rpm` (Linux x64 and aarch64), `.zip` (Windows), and `.tar.gz` (macOS arm64); then code-sign the Windows exe on a self-hosted runner and replace the zip asset |

The vcpkg cache key is OS + `vcpkg.json` hash, extended with the architecture where two legs share an OS (`build-linux-arm64.yml`) and with the build container where the toolchain differs too (the release workflow's Debian 13 aarch64 leg). The first run after a dependency change rebuilds from source; subsequent runs restore from cache.
