# Motion Master — First-Run Setup

## 1. Grant capabilities (required once)

```bash
sudo ./setup.sh
```

This runs `setcap cap_sys_nice,cap_net_admin,cap_net_raw=eip` on the `motion-master` binary.

**Why it is needed:**

- `cap_sys_nice` — allows the server to set `SCHED_FIFO` real-time scheduling priority on the game loop thread. Without it the RT loop runs at normal priority, which increases cycle jitter.
- `cap_net_raw` + `cap_net_admin` — allow opening raw EtherCAT sockets for fieldbus communication. Without them `POST /api/init` with `driver: soem` will fail.

File capabilities are the least-privilege alternative to running as root — the binary gets only the three permissions it needs, nothing else.

## 2. Start the server

```bash
./motion-master
```

The server listens on `https://local.motion-master.synapticon.com:8443`. Open the Motion Master web app at `https://motion-master.synapticon.com` to connect.

For full usage options run `./motion-master --help`.
