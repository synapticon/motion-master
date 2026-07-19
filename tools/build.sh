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

# Cap build parallelism. Without a limit, ninja fans out to nproc+2 jobs; on a many-core
# workstation that burst of heavy C++23 translation units (uWebSockets templates run 1-3 GB
# each) thrashes the machine hard enough to crash the desktop compositor and take the
# developer's terminal session down with it. Default to a third of the cores (min 1), and
# de-prioritise the build (nice + idle I/O class) so the desktop always wins. Override the
# job count with MM_BUILD_JOBS=N; MM_BUILD_JOBS=0 restores the unbounded default.
jobs="${MM_BUILD_JOBS:-}"
if [[ -z "$jobs" ]]; then
    cores="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
    jobs=$(( cores / 3 ))
    (( jobs < 1 )) && jobs=1
fi

parallel_args=()
(( jobs > 0 )) && parallel_args=(--parallel "$jobs")

# Prefer the desktop: run niced, and idle-class I/O where ionice exists (Linux; macOS has no
# ionice, so fall back to nice alone).
nice_prefix=(nice -n 19)
if command -v ionice >/dev/null 2>&1; then
    nice_prefix=(ionice -c3 nice -n 19)
fi

"${nice_prefix[@]}" cmake --build --preset "$preset" "${parallel_args[@]}" "${args[@]:1}"

binary="build/${preset}/apps/motion_master/motion-master"
if [[ -x "$binary" && "$setcap" -eq 1 ]]; then
    echo "Setting capabilities on $binary (sudo required)..."
    sudo setcap cap_sys_nice,cap_net_admin,cap_net_raw,cap_ipc_lock=eip "$binary"
fi
