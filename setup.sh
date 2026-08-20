#!/usr/bin/env bash
# Set up a Motion Master archive after you extract it. The script is safe to run again.
#
# The script does two things. It downloads the auto-tuning executable. Then it grants the
# capabilities that the real-time loop and the fieldbus need.
#
# Run the script as yourself. Do not run it with sudo. The script calls sudo itself for the one
# step that needs root, so the file it downloads belongs to you.
set -euo pipefail
dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

"$dir/install-auto-tuning.sh" "$dir"

# Capabilities are a Linux mechanism. On macOS, sudo at run time grants the same two things, and
# SETUP.md explains that. So there is nothing to set here.
if [ "$(uname -s)" = Linux ]; then
  sudo setcap cap_sys_nice,cap_net_admin,cap_net_raw,cap_ipc_lock=eip "$dir/motion-master"
  echo "Capabilities set on $dir/motion-master"
fi
