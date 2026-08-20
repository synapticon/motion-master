# Motion Master — First-Run Setup

## 1. Run the setup script (required once)

```bash
./setup.sh
```

Run the script as yourself. Do not run it with `sudo`. The script calls `sudo` itself for the one step that needs root, so the file it downloads belongs to you.

The script does two things.

**It downloads the auto-tuning executable.** The file is about 65 MB, and Motion Master starts it as a child process. It installs as `auto-tuning`, next to the server binary. The file is downloaded once, instead of a copy in every release. If the download fails, the script prints a message and continues. Motion Master then runs without auto-tuning, and only the auto-tuning endpoints are missing. To add the file later, run `./install-auto-tuning.sh`.

**It grants the capabilities.** The script runs `setcap cap_sys_nice,cap_net_admin,cap_net_raw,cap_ipc_lock=eip` on the `motion-master` binary.

**Why the capabilities are needed:**

- `cap_sys_nice` — allows the server to set `SCHED_FIFO` real-time scheduling priority on the game loop thread. Without it the RT loop runs at normal priority, which increases cycle jitter.
- `cap_net_raw` + `cap_net_admin` — allow opening raw EtherCAT sockets for fieldbus communication. Without them `POST /api/init` with `driver: soem` will fail.
- `cap_ipc_lock` — allows `mlockall()` to pin the process's memory so a mid-cycle page fault cannot inject an unbounded RT latency spike. Without it the loop still runs, but memory is not pinned (the Game Loop page shows `mlockall: no`).

File capabilities are the least-privilege alternative to running as root — the binary gets only the four permissions it needs, nothing else.

## 2. Start the server

```bash
./motion-master
```

The server listens on `https://local.motion-master.synapticon.com:61447`. To connect, open the Motion Master Console at `https://motion-master.synapticon.com/apps/console/`.

For full usage options run `./motion-master --help`.
