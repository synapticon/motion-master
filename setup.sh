#!/usr/bin/env bash
# Grant capabilities required for RT scheduling and raw EtherCAT socket access.
# Run once after extracting the release archive: sudo ./setup.sh
set -euo pipefail
dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
sudo setcap cap_sys_nice,cap_net_admin,cap_net_raw=eip "$dir/motion-master"
echo "Capabilities set on $dir/motion-master"
