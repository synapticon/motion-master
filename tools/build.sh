#!/usr/bin/env bash
set -euo pipefail
preset="${1:-x64-linux-debug}"
cmake --build --preset "$preset" "${@:2}"

binary="build/${preset}/apps/motion_master/motion-master"
if [[ -x "$binary" ]]; then
    echo "Setting capabilities on $binary (sudo required)..."
    sudo setcap cap_sys_nice,cap_net_admin,cap_net_raw=eip "$binary"
fi
