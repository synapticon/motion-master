#!/usr/bin/env bash
# Boot the VM in the background and block until SSH answers.

# shellcheck source=rt/vm/common.sh
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

require qemu-system-x86_64

BOOT_TIMEOUT="${RT_VM_BOOT_TIMEOUT:-180}"

if vm_is_running; then
    log "already running (pid $(cat "$PID_FILE")) on ssh port $SSH_PORT"
    exit 0
fi

[ -f "$DISK" ] || die "no VM disk — run ./create.sh first"

[ -w /dev/kvm ] || die "/dev/kvm is not writable — KVM acceleration is required"

rm -f "$PID_FILE" "$MONITOR_SOCKET" "$SERIAL_LOG"

log "booting $VM_NAME (${VM_CPUS} vCPU, ${VM_MEMORY_MB} MB)"
qemu-system-x86_64 \
    -name "$VM_NAME" \
    -machine q35,accel=kvm \
    -cpu host \
    -smp "$VM_CPUS" \
    -m "$VM_MEMORY_MB" \
    -drive "if=virtio,format=qcow2,file=$DISK" \
    -drive "if=virtio,format=raw,file=$SEED_ISO,readonly=on" \
    -netdev "user,id=net0,hostfwd=tcp:$SSH_HOST:$SSH_PORT-:22" \
    -device virtio-net-pci,netdev=net0 \
    -display none \
    -serial "file:$SERIAL_LOG" \
    -monitor "unix:$MONITOR_SOCKET,server,nowait" \
    -pidfile "$PID_FILE" \
    -daemonize

mapfile -t SSH_OPTS < <(ssh_opts)

log "waiting for SSH on $SSH_HOST:$SSH_PORT (up to ${BOOT_TIMEOUT}s)"
deadline=$((SECONDS + BOOT_TIMEOUT))
until ssh "${SSH_OPTS[@]}" -o ConnectTimeout=3 -o BatchMode=yes \
    -p "$SSH_PORT" "$VM_USER@$SSH_HOST" true 2>/dev/null; do
    vm_is_running || die "QEMU exited during boot — see $SERIAL_LOG"
    [ "$SECONDS" -lt "$deadline" ] || die "timed out waiting for SSH — see $SERIAL_LOG"
    sleep 2
done

log "up — ./ssh.sh for a shell, ./provision.sh to run the playbook"
log "console log: $SERIAL_LOG"
