#!/usr/bin/env bash
# Shell into the VM, or run a command in it:  ./ssh.sh 'uname -r'

# shellcheck source=rt/vm/common.sh
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

[ -f "$SSH_KEY" ] || die "no SSH key — run ./create.sh first"
vm_is_running || die "VM is not running — ./start.sh"

mapfile -t SSH_OPTS < <(ssh_opts)

exec ssh "${SSH_OPTS[@]}" -p "$SSH_PORT" "$VM_USER@$SSH_HOST" "$@"
