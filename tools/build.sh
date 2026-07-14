#!/usr/bin/env bash
set -euo pipefail

# By default the build does NOT stamp capabilities, so it needs no sudo — suitable for CI
# and automated agents. Pass --setcap to run the privileged setcap step the binary needs to
# open raw EtherCAT sockets (cap_net_raw/cap_net_admin), use SCHED_FIFO RT scheduling
# (cap_sys_nice), and mlockall() its memory so a page fault can't spike RT latency
# (cap_ipc_lock) when run against hardware. Everything else is forwarded to the build
# (first positional = preset, the rest = extra cmake args).
setcap=0
args=()
for arg in "$@"; do
    case "$arg" in
        --setcap) setcap=1 ;;
        *) args+=("$arg") ;;
    esac
done

preset="${args[0]:-x64-linux-debug}"
cmake --build --preset "$preset" "${args[@]:1}"

binary="build/${preset}/apps/motion_master/motion-master"
if [[ -x "$binary" && "$setcap" -eq 1 ]]; then
    echo "Setting capabilities on $binary (sudo required)..."
    sudo setcap cap_sys_nice,cap_net_admin,cap_net_raw,cap_ipc_lock=eip "$binary"
fi
