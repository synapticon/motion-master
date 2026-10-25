# Motion Master

Next-generation motion control software for SOMANET servo drives. Browser-based control interface, real-time process data exchange, and a secure HTTP API and WebSocket interface — control from any language, any tool, any AI agent.

See [FEATURES.md](FEATURES.md) for a full catalog of capabilities.

## Architecture

Design documents (the Mermaid diagrams render natively on GitHub):

- [Class diagram](https://github.com/synapticon/motion-master/blob/main/docs/CLASS_DIAGRAM.md) — class structure, ownership, and inheritance.
- [RT scheduling primer](https://github.com/synapticon/motion-master/blob/main/docs/RT_SCHEDULING.md) — `SCHED_FIFO`, `mlockall`, and absolute-deadline sleeping: the three primitives the cycle depends on, plus the optional core pinning that an `isolcpus` core needs.
- [Threading model](https://github.com/synapticon/motion-master/blob/main/docs/THREADS.md) — the built-in threads, the RT cycle, and why the RT loop never takes a lock.
- [Locking and synchronisation](https://github.com/synapticon/motion-master/blob/main/docs/LOCKING.md) — every mutex, the lock ordering, and the lock-free protocols that carry the RT path.
- [LAN deployment](https://github.com/synapticon/motion-master/blob/main/docs/LAN_DEPLOYMENT.md) — running the server on a separate machine (Raspberry Pi, industrial PC) and reaching it from the Console over the network.
- [Raspberry Pi 5](https://github.com/synapticon/motion-master/blob/main/docs/RASPBERRY_PI.md) — the card image: download it, write it, set its Wi-Fi, reach it, and keep it up to date.
- [Writing guide](https://github.com/synapticon/motion-master/blob/main/docs/WRITING.md) — the English style for documentation, code comments, and commit messages.

## Installation

Release packages are on the [Releases](../../releases) page: a tarball, a `.deb` and an `.rpm` for Linux x86-64 and aarch64, a zip for Windows x64, and a tarball for macOS on Apple Silicon. Selected versions are on Docker Hub as well, as `synapticon/motion-master`, for amd64 and arm64. See [Docker](#docker) under Usage for the run command.

[Installing Motion Master](docs/INSTALLATION.md) covers each of those paths. Read it for the steps on your platform, for the [auto-tuning](docs/INSTALLATION.md#auto-tuning) executable that every install path downloads, and for the [capabilities](docs/INSTALLATION.md#linux-capabilities) the binary needs on Linux.

## Usage

### First run

Start the server (on Linux from a package install, `motion-master` is already on `PATH`):

```bash
motion-master           # macOS: sudo ./motion-master   Windows: motion-master.exe
motion-master --open    # the same, and opens the console in the default browser
```

It serves two ports, both bound to `127.0.0.1` and each on its own event loop and thread, so a slow HTTP request can never stall the live data stream:

| Port | Purpose | Config key |
| --- | --- | --- |
| `61447` | HTTPS API — everything request/response (init, scan, SDO, FoE, state, parameters) | `server.httpPort` |
| `62281` | Secure WebSocket (`wss://`) — monitoring batches and notifications out, topic subscriptions in | `server.wsPort` |

`--open` points the browser at `https://motion-master.synapticon.com/apps/console/`. The console is a PWA hosted by Synapticon. It talks to the server on your machine over `https://local.motion-master.synapticon.com:61447`, a name that resolves to `127.0.0.1`, which is why the bundled certificate is valid for it. Your data stays there. The page is the only thing that comes from Synapticon, and no parameter, recording, or drive identity is sent back.

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

### Docker

Selected versions are on [Docker Hub](https://hub.docker.com/r/synapticon/motion-master/tags) as `synapticon/motion-master`, for `linux/amd64` and `linux/arm64`. One tag covers both architectures, and Docker picks the image for the host. The tag is the bare version, with no `v`. There is no `latest` tag, so name the version in every command. The example below runs `6.0.0-alpha.95`:

```bash
docker run --rm --network host \
  --cap-add NET_RAW --cap-add NET_ADMIN \
  --cap-add SYS_NICE --cap-add IPC_LOCK --ulimit memlock=-1 \
  synapticon/motion-master:6.0.0-alpha.95
```

`docker run` pulls the image when it is not present. The server then serves the same two ports as a package install, and the console at `https://motion-master.synapticon.com/apps/console/` reaches it the same way. The image holds the TLS certificate and the auto-tuning executable, so there is no setup step. An older image renews an expired certificate at start.

Three flags in that command have no equivalent on a package install:

- `--network host` is required. The server binds to `127.0.0.1`, and the port forwarding of Docker never reaches the loopback interface.
- The four `--cap-add` flags grant what `setcap` grants on a package install, because a container ignores file capabilities. Without `NET_RAW` and `NET_ADMIN`, `POST /api/init` fails for the SOEM driver. Without `SYS_NICE` and `IPC_LOCK`, the game loop runs without real-time priority and logs a warning.
- `--ulimit memlock=-1` is required with `IPC_LOCK`. Without it the kernel rejects `mlockall()`, even when the capability is present.

To apply a [config file](#configuration), for example to initialise the fieldbus at start, mount the file and name it in `MM_CONFIG`. Do not pass `--config` after the image name. The entrypoint passes that flag itself, and the binary refuses a second one.

```bash
docker run --rm --network host \
  --cap-add NET_RAW --cap-add NET_ADMIN \
  --cap-add SYS_NICE --cap-add IPC_LOCK --ulimit memlock=-1 \
  -v "$(pwd)/motion-master.jsonc:/config.jsonc:ro" -e MM_CONFIG=/config.jsonc \
  synapticon/motion-master:6.0.0-alpha.95
```

Give `-v` an absolute source path. Docker reads a relative path as the name of a volume, and the container then sees an empty directory instead of your file. The parameter cache, the log file and the `.mmpd` dumps live under `/root/.cache/motion-master` in the container. Mount a host directory there to keep them across runs. [Installation → Docker](docs/INSTALLATION.md#docker) covers the certificate discovery order, and how to replace the certificate on an older image.

### Prerequisites

Requirements for running a release binary (building from source has its own — see [Development](docs/DEVELOPMENT.md)):

- **Linux (both architectures):** glibc **2.38** or newer and libstdc++ from GCC 13.2 or newer (`GLIBCXX_3.4.32`) — the only dynamic dependencies are `libc`, `libm`, `libstdc++` and `libgcc_s`. The floor comes from the symbols the binary references, not from the distro it was built on: the x64 artefacts are built on Ubuntu 24.04 and the arm64 ones on Debian 13 (trixie), and both land on the same 2.38 requirement. So Ubuntu 24.04+, Debian 13+ and Raspberry Pi OS trixie all work, while Ubuntu 22.04 (glibc 2.35) and Debian 12 (bookworm, 2.36) fail at load with a `version 'GLIBC_2.3x' not found` error — [build from source](docs/DEVELOPMENT.md#building-from-source) there.
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

On Linux, `motion-master` requires raw socket and RT scheduling capabilities. Release packages set all four automatically (deb `postinst`, rpm `%post`, tarball `setup.sh`) — see [Linux capabilities](docs/INSTALLATION.md#linux-capabilities). Without them the binary still runs and serves the API, but EtherCAT initialisation fails and the game loop cannot go real-time.

### Configuration

Every setting lives in a JSONC config file — comments are allowed, all keys are optional, and each one falls back to a built-in default. [`motion-master.example.jsonc`](apps/motion_master/motion-master.example.jsonc) documents every key inline with its default; copy it to `motion-master.jsonc` next to the binary (auto-discovered) or pass it with `--config`. The top-level blocks:

| Block | What it covers |
| --- | --- |
| `server` | `httpPort` (61447), `wsPort` (62281), `corsOrigin` |
| `fieldbus` | `driver` + `adapter` to initialise at startup — omit the block to wait for `POST /api/init` instead. Also `mailboxStatusFmmu`, which must stay `false` on TI PRU-ICSS ESCs |
| `logging` | `level` for the console and `GET /api/log` (`trace` … `critical` \| `off`, default `info`), and `file` — the rotating log file that outlives the process: `enabled`, its own `level` (default `debug`, so the terminal stays readable while the file keeps the detail), `directory` (default: a `logs` subdirectory of the user-cache root, so it is downloadable from the Console), `maxSizeMb` and `maxFiles` |
| `gameLoop` | `periodUs` — the real-time cycle period (1000 = 1 ms; the Windows release ships 4000) |
| `recorder` | `capacity` of the lossless process-data ring in cycles (default 300000 ≈ 5 min at 1 ms for one drive, ≈ 38 MB) and `dumpDir` for `.mmpd` dumps |
| `parameterCache` | On-disk cache of CoE object-dictionary *definitions* (never values) keyed by vendor/product/firmware version (0x100A) — `enabled`, `cacheAllVendors`, `directory` |
| `esiCache` | ESI files that add units to live parameters, found by vendor and firmware version (0x100A) as `esi-<vendor>-<version>.xml` — `enabled`, `directory` (default: the `esis` subdirectory of the user cache) |
| `parameters` | `readObjectDictionaryOnPreop` (enumerate on the first PRE-OP) and `useCompleteAccess` (one CoE Complete Access upload per ARRAY/RECORD instead of per-subindex reads) |
| `tls` | `certPath`, `keyPath` (empty = auto-discover), and `autoUpdate` — set `false` for air-gapped installs |

If a raised `skippedCycles` count shows up right after start (visible on the console's Game Loop page), the configured `gameLoop.periodUs` is too aggressive for that machine's timer resolution — raise it. A 1 ms cycle is realistic on a `PREEMPT_RT` Linux host; stock Windows often cannot sustain better than ~1.5 ms, which is why the Windows release defaults to 4 ms.

### Clients

The API is plain HTTPS + JSON, so any language works. Two reference clients ship in the repo:

- **[`clients/python`](clients/python)** — a standalone reference client (`requests` + `websockets`) with one example script per action, each named after it: `scan.py`, `read-parameter.py`, `monitor.py`, `offset-detection.py`, `firmware-installation.py` and more. Each script runs on its own. It opens the fieldbus driver, scans, and moves the device to the AL state it needs, so no script has to run before another, and a second run gives the same result. The client resolves every call by `operationId` against the spec it fetches from `GET /api/swagger.yml` at startup, so it holds no hardcoded URLs and adapts to whichever server version it is pointed at. Copy `motion_master_client.py` anywhere — no checkout needed.

  ```bash
  cd clients/python
  source setup.sh                      # venv + pip install -r requirements.txt
  cp config.example.toml config.toml   # point it at your server
  python examples/scan.py
  ```

- **[`@synapticon/motion-master-client`](web/packages/motion-master-client)** — the published, isomorphic TypeScript SDK (browser and Node): the generated HTTP client plus the WebSocket connection, and a parser for `.mmpd` recorder dumps. It ships on npm at the same version as the server binary, so `client@X` is known-good against `binary@X`. This is what the console and the integration tests use.

  ```bash
  npm install @synapticon/motion-master-client
  ```

## Development

[Development](docs/DEVELOPMENT.md) covers building from a source checkout, running the server against a development Console, the `web/` workspace, adding your own HTTP routes in C++, the developer scripts, and the hardware-in-the-loop tests.

[Releasing](docs/RELEASING.md) covers the single version number every artefact shares, the bump script, and the CI workflows.
