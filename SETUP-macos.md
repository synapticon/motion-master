# Motion Master — First-Run Setup (macOS)

## 1. Clear the quarantine flag (required once)

The macOS build is not notarized, so Gatekeeper quarantines it after download and
refuses to launch it. Remove the quarantine attribute from the extracted files:

```bash
xattr -dr com.apple.quarantine motion-master
```

(Alternatively, right-click `motion-master` in Finder → **Open** once, and confirm
the dialog — this whitelists that exact binary.)

## 2. Download auto-tuning (optional)

```bash
./setup.sh
```

The auto-tuning executable is about 65 MB, and Motion Master starts it as a child process. It installs as `auto-tuning`, next to the server binary. The file is downloaded once, instead of a copy in every release. If the download fails, the script prints a message and continues. Motion Master then runs without auto-tuning, and only the auto-tuning endpoints are missing. To add the file later, run `./install-auto-tuning.sh`.

On macOS the script does nothing else. The capability step it runs on Linux has no equivalent here, and step 3 uses `sudo` instead.

## 3. Start the server

```bash
sudo ./motion-master
```

`sudo` is needed for two reasons:

- **EtherCAT raw sockets** — on macOS the fieldbus driver opens the NIC through the
  BPF devices (`/dev/bpf*`), which are root-only by default. Without elevated
  privileges, `POST /api/init` with `driver: soem` will fail.
- **Real-time scheduling** — raising the game-loop thread to `SCHED_FIFO` priority
  requires elevated privileges. Without it the server still runs, but the RT loop
  drops to normal priority (warning logged) and cycle jitter increases.

> To run without `sudo`, grant your user access to the BPF devices — e.g. install
> Wireshark's **ChmodBPF** helper, which sets up a `access_bpf` group and relaxes
> `/dev/bpf*` permissions on boot. RT scheduling will still be unavailable without
> elevated privileges, but the server runs fine non-RT for non-latency-critical use.

The server listens on `https://local.motion-master.synapticon.com:61447`. To connect, open
the Motion Master Console at `https://motion-master.synapticon.com/apps/console/`.

For full usage options run `./motion-master --help`.
