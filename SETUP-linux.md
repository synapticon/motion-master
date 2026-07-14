# Motion Master — First-Run Setup

## 1. Grant capabilities (required once)

```bash
sudo ./setup.sh
```

This runs `setcap cap_sys_nice,cap_net_admin,cap_net_raw,cap_ipc_lock=eip` on the `motion-master` binary.

**Why it is needed:**

- `cap_sys_nice` — allows the server to set `SCHED_FIFO` real-time scheduling priority on the game loop thread. Without it the RT loop runs at normal priority, which increases cycle jitter.
- `cap_net_raw` + `cap_net_admin` — allow opening raw EtherCAT sockets for fieldbus communication. Without them `POST /api/init` with `driver: soem` will fail.
- `cap_ipc_lock` — allows `mlockall()` to pin the process's memory so a mid-cycle page fault cannot inject an unbounded RT latency spike. Without it the loop still runs, but memory is not pinned (the Game Loop page shows `mlockall: no`).

File capabilities are the least-privilege alternative to running as root — the binary gets only the four permissions it needs, nothing else.

## 2. Start the server

```bash
./motion-master
```

The server listens on `https://local.motion-master.synapticon.com:61447`. Open the Motion Master Console web app at `https://motion-master.synapticon.com/apps/console/` to connect — it is launched from the landing page at `https://motion-master.synapticon.com`.

For full usage options run `./motion-master --help`.
