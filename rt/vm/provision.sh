#!/usr/bin/env bash
# Run the RT playbook against the VM. Extra arguments go to ansible-playbook:
#
#   ./provision.sh --check
#   ./provision.sh --tags rt-boot
#   ./provision.sh --tags benchmark      # cyclictest (meaningless in a VM — see README)

# shellcheck source=rt/vm/common.sh
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

require ansible-playbook

vm_is_running || die "VM is not running — ./start.sh"

exec "$VM_DIR/../provision/play-rt.sh" inventory/qemu.yml "$@"
