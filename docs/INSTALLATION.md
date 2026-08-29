# Installing Motion Master

Motion Master ships as a release package for Linux, Windows and macOS, and as a container image. This document covers every install path: what each artefact is, what the install scripts do, and the privileges the binary needs on each platform. To build from a source checkout instead, read [Development](DEVELOPMENT.md).

Release packages are available on the [Releases](https://github.com/synapticon/motion-master/releases) page. Every release ships binaries for Linux (x86-64 and aarch64), Windows, and macOS:

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

> **aarch64 note:** the arm64 artefacts are built on **Debian 13 (trixie)** and need glibc 2.38 or newer, so they run on Debian 13 and Raspberry Pi OS trixie. Debian 12 (bookworm, glibc 2.36) is too old — [build from source](DEVELOPMENT.md#building-from-source) there.

[Download statistics](https://github.com/synapticon/motion-master/releases/tag/stats) shows how many times each file in each release has been downloaded. A workflow regenerates it once a day. The same text is also an asset on that release, at [`STATS.md`](https://github.com/synapticon/motion-master/releases/download/stats/STATS.md), for a script that wants to read it.

## Auto-tuning

The tuning calculations run in a separate executable: the controller-gain functions, and the fit that turns recorded measurements into a plant model. Motion Master starts it as a child process, and calls it over HTTP on loopback. The installed file is `auto-tuning`, and `auto-tuning.exe` on Windows.

That executable drives nothing. It computes on the numbers sent to it. The measurement a plant model is fitted to is recorded on the drive, by the system identification procedure.

It is also **optional**, and that is not only a matter of size. Auto-tuning is a commissioning tool: gains are computed while a drive is being configured, and a machine that only runs an already-configured drive never calls it. An installation that commissions nothing can leave the executable off entirely, or keep it and set `autoTuning.enabled` to `false`.

The executable is not in the release archives. It is about 65 MB, and the Motion Master binary is about 5 MB. Motion Master ships on every tag, and auto-tuning changes a few times a year. A copy in every release would therefore make each download many times larger, for a file that rarely changes. Each install path downloads the file once instead, from one rolling release: [`releases/tag/auto-tuning`](https://github.com/synapticon/motion-master/releases/tag/auto-tuning).

| Install path | What downloads the file |
| --- | --- |
| Tarball (Linux, macOS) | `./setup.sh`, or `./install-auto-tuning.sh` on its own |
| `.deb` / `.rpm` | the `postinst` or `%post` scriptlet, during install |
| Zip (Windows) | `.\setup.ps1` |
| Docker | the image build, which puts the file in the image |

If the download fails, each of them prints a message and continues. Motion Master then runs without auto-tuning. The HTTP API, the fieldbus and the real-time loop are not affected, and only the auto-tuning endpoints are missing. To add the file later, run the script again on a machine that is online.

A machine that already has the file keeps it. An upgrade of Motion Master therefore never downloads the file a second time.

Each download is named for its platform, as `standalone-autotuning-linux-x86_64` is. Every install path renames the file to `auto-tuning` when it puts the file in place, so one name works on every platform.

There is no build for ARM64 Windows, and none for an Intel Mac. ARM64 Windows runs the x64 executable under the emulation that the operating system provides.

## Debian / Ubuntu

```bash
sudo apt install ./motion-master-<version>-amd64.deb  # install or upgrade (aarch64: -arm64.deb)
sudo apt remove motion-master                         # remove (leaves cert.pem / key.pem)
sudo apt purge motion-master                          # full removal including certs
```

The `postinst` script automatically sets the four required capabilities (`cap_sys_nice`, `cap_net_admin`, `cap_net_raw`, `cap_ipc_lock`) on the binary — see [Linux capabilities](#linux-capabilities) for what each one does. On upgrade the capabilities are re-applied to the new binary automatically. The script also downloads [auto-tuning](#auto-tuning), when the machine does not have that file yet. This is why the package depends on `curl`. `apt purge` deletes the file again.

> **Note:** `apt remove` leaves `cert.pem` and `key.pem` behind as conffiles. Use `apt purge` for a complete uninstall.

## Fedora / RHEL / openSUSE

```bash
sudo dnf install ./motion-master-<version>-x86_64.rpm     # Fedora / RHEL (install or upgrade)
sudo zypper install ./motion-master-<version>-x86_64.rpm  # openSUSE (install or upgrade)
sudo dnf remove motion-master                             # full removal
```

On aarch64 use `motion-master-<version>-aarch64.rpm` instead.

The `%post` scriptlet sets the capabilities and downloads [auto-tuning](#auto-tuning). It does what the deb `postinst` does. An uninstall deletes the downloaded file again.

On uninstall, unmodified `cert.pem` and `key.pem` are removed automatically. If you replaced them with your own, they are saved as `cert.pem.rpmsave` / `key.pem.rpmsave`.

## Tarball

```bash
tar -xzf motion-master-<version>-linux-x64.tar.gz  # aarch64: -linux-arm64.tar.gz
cd motion-master-<version>-linux-x64
./setup.sh  # downloads auto-tuning, then sets capabilities; run it again after an OS update
./motion-master --help
```

Run `setup.sh` as yourself. Do not use `sudo`. The script calls `sudo` itself for the capability step, so the file it downloads belongs to you.

Alongside the binary the tarball carries `setup.sh` and `install-auto-tuning.sh`, a `SETUP.md` with the same first-run notes, the bundled `cert.pem`/`key.pem`, and the annotated `motion-master.example.jsonc`.

## Linux capabilities

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

## Windows

Unzip `motion-master-<version>-windows-x64.zip` — it contains `motion-master.exe`, the bundled `cert.pem`/`key.pem`, an auto-loaded `motion-master.jsonc` (preset to a 4 ms real-time cycle, robust on stock Windows timers), the annotated `motion-master.example.jsonc`, and the required vcpkg runtime DLLs. The zip also contains `setup.ps1`, which downloads [auto-tuning](#auto-tuning). Windows has no capability step, so the script does nothing else. Install the two runtime dependencies listed under [Usage → Prerequisites](../README.md#prerequisites) (Visual C++ Redistributable and Npcap), then run `motion-master.exe` from the extracted directory — it picks up the neighbouring `motion-master.jsonc` automatically (edit it to change the cycle period or any other setting).

`motion-master.exe` is **Authenticode code-signed** (with an RFC 3161 timestamp), so Windows shows Synapticon as the verified publisher instead of an "unknown publisher" SmartScreen block. Signing happens on a self-hosted runner holding the certificate token, as a final step of the release workflow — it replaces the zip asset in place, so the published archive is always the signed one.

## macOS (Apple Silicon)

```bash
tar -xzf motion-master-<version>-macos-arm64.tar.gz
cd motion-master-<version>-macos-arm64
xattr -dr com.apple.quarantine motion-master  # required once — see below
./setup.sh                                    # downloads auto-tuning
sudo ./motion-master
```

Two macOS specifics, both covered by the bundled `SETUP.md`:

- **The build is not notarized**, so Gatekeeper quarantines it on download and refuses to launch it. Clear the quarantine attribute as shown above (or right-click → **Open** once in Finder and confirm the dialog, which whitelists that exact binary).
- **`sudo` is needed** for the same two reasons capabilities are needed on Linux: the fieldbus opens the NIC through the root-only BPF devices (`/dev/bpf*`), so `POST /api/init` with `driver: soem` fails without it, and raising the game-loop thread to `SCHED_FIFO` is privileged. Without `sudo` the server still runs and serves the API — it just cannot reach a fieldbus and logs a warning as the RT loop drops to normal priority. `SETUP.md` also shows how to grant BPF access to your user instead, if you would rather not use `sudo`.

## Docker

Motion Master also runs as a container image. Building the image and publishing it to a registry is covered under [Development → Docker image](DEVELOPMENT.md#docker-image); this section covers running an image you built locally or pulled from a registry.

`--network host` is required on all `docker run` commands — the server binds to `127.0.0.1` and Docker's port forwarding never reaches the loopback interface.

### TLS certificates

Every image bakes `cert.pem`/`key.pem` in at build time by fetching them from the rolling `tls-cert` release (see [Development → Docker image](DEVELOPMENT.md#docker-image)); an image built offline ships with empty placeholders and self-heals on start instead. The runtime discovery order is the same as `tools/run.sh`:

```bash
# Bundled cert used automatically
docker run --rm --network host motion-master

# Mount an acme.sh cert from the host instead (e.g. for a locally-issued cert)
docker run --rm --network host \
  -v "$HOME/.acme.sh/local.motion-master.synapticon.com_ecc:/root/.acme.sh/local.motion-master.synapticon.com_ecc:ro" \
  motion-master
```

### Updating an expired cert on an older image

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

### Capabilities

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

### Run from a registry

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
