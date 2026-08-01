#!/usr/bin/env bash
# Provision a real-time target. Takes an inventory; everything after it goes to
# ansible-playbook.
#
#   ./play-rt.sh inventory/qemu.yml                 # the throwaway VM (rt/vm)
#   ./play-rt.sh inventory/lab.yml --check          # real hardware, dry run
#   ./play-rt.sh inventory/lab.yml --tags benchmark # cyclictest on hardware
#
# No become password is prompted for: the VM has NOPASSWD sudo, and lab hosts
# should either match that or be run with --ask-become-pass.
set -euo pipefail
cd "$(dirname "$0")/ansible" || exit 1

if [ $# -lt 1 ]; then
    echo "usage: $0 <inventory> [ansible-playbook args...]" >&2
    echo "available:" >&2
    ls -1 inventory/*.yml >&2
    exit 1
fi

INVENTORY="$1"
shift

[ -f "$INVENTORY" ] || {
    echo "no such inventory: $INVENTORY (copy inventory/lab.yml.example to start)" >&2
    exit 1
}

exec ansible-playbook -i "$INVENTORY" rt-target.yml "$@"
