#!/usr/bin/env bash
# Shared configuration for the throwaway Debian 14 QEMU VM used to develop and
# test the RT provisioning playbook. Sourced by every script in this directory.
#
# Every value can be overridden from the environment, e.g.
#   RT_VM_CPUS=8 RT_VM_SSH_PORT=2299 ./start.sh

# Everything defined here exists to be read by the scripts that source this
# file, so shellcheck's unused-variable check cannot see the use and fires on
# all of them.
# shellcheck disable=SC2034

set -euo pipefail

VM_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CACHE_DIR="${RT_VM_CACHE_DIR:-$VM_DIR/.cache}"

# Debian 14 (forky) generic cloud image.
#
# Forky rather than stable because that is what the boards will run: Debian 13
# cannot boot a Raspberry Pi 5 at all, and 14 carries the PREEMPT_RT kernel
# (7.1.3) in main on both architectures, so no backports pinning is needed. It
# is still testing today, which is the trade — a daily image moves under you,
# and that is fine for a machine whose whole purpose is to be thrown away.
#
# Set RT_VM_BASE_URL / RT_VM_BASE_NAME to go back to a stable base:
#   RT_VM_BASE_URL=https://cloud.debian.org/images/cloud/trixie/latest \
#   RT_VM_BASE_NAME=debian-13-genericcloud-amd64.qcow2 ./fetch-base.sh
# (then set rt_kernel_suite: trixie-backports in the inventory).
#
# The checksum is fetched alongside the image, so an upstream refresh is
# detected rather than silently accepted.
BASE_URL="${RT_VM_BASE_URL:-https://cloud.debian.org/images/cloud/forky/daily/latest}"
BASE_NAME="${RT_VM_BASE_NAME:-debian-14-genericcloud-amd64-daily.qcow2}"
BASE_IMAGE="$CACHE_DIR/$BASE_NAME"

# Per-VM state. All of it is disposable: reset.sh deletes and rebuilds it.
DISK="$CACHE_DIR/disk.qcow2"
SEED_ISO="$CACHE_DIR/seed.iso"
SSH_KEY="$CACHE_DIR/id_ed25519"
PID_FILE="$CACHE_DIR/qemu.pid"
MONITOR_SOCKET="$CACHE_DIR/qemu.monitor"
SERIAL_LOG="$CACHE_DIR/serial.log"

# 4 vCPUs / 4 GB mirrors the AAeon E3940 reference board, so an isolcpus=2,3
# split means the same thing here as it does on the real target.
VM_NAME="${RT_VM_NAME:-motion-master-rt-vm}"
VM_CPUS="${RT_VM_CPUS:-4}"
VM_MEMORY_MB="${RT_VM_MEMORY_MB:-4096}"
VM_DISK_SIZE="${RT_VM_DISK_SIZE:-20G}"
VM_USER="${RT_VM_USER:-debian}"

# Bound to the loopback interface on purpose — this VM has passwordless sudo and
# a known console password, and must never be reachable from the LAN.
SSH_HOST="127.0.0.1"
SSH_PORT="${RT_VM_SSH_PORT:-2222}"

log() {
    printf '[rt-vm] %s\n' "$*"
}

die() {
    printf '[rt-vm] error: %s\n' "$*" >&2
    exit 1
}

require() {
    command -v "$1" >/dev/null 2>&1 || die "'$1' is not installed"
}

# ssh/scp options shared by ssh.sh and the Ansible inventory. The host key
# changes on every reset, so it is deliberately not recorded.
ssh_opts() {
    printf '%s\n' \
        -i "$SSH_KEY" \
        -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null \
        -o LogLevel=ERROR
}

vm_is_running() {
    [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null
}
