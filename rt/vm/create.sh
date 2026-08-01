#!/usr/bin/env bash
# Create a fresh VM generation: SSH keypair, cloud-init seed ISO, and a copy-on-write
# overlay on top of the immutable base image. Cheap enough to redo on every reset.

# shellcheck source=rt/vm/common.sh
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

require qemu-img
require genisoimage
require ssh-keygen

[ -f "$BASE_IMAGE" ] || die "base image missing — run ./fetch-base.sh first"

if vm_is_running; then
    die "VM is running — stop it first (./stop.sh) or use ./reset.sh"
fi

mkdir -p "$CACHE_DIR"

# One dedicated key per checkout, so the VM never depends on (or trusts) the
# developer's personal SSH identity.
if [ ! -f "$SSH_KEY" ]; then
    log "generating SSH keypair"
    ssh-keygen -t ed25519 -N '' -C "$VM_NAME" -f "$SSH_KEY" >/dev/null
fi

log "writing cloud-init seed"
cat > "$CACHE_DIR/meta-data" <<EOF
instance-id: $VM_NAME
local-hostname: rt-vm
EOF

# The plaintext console password is intentional: it is the only way in when SSH
# or networking is what broke. Safe because the VM is loopback-only (see the
# hostfwd bind address in common.sh) and disposable.
cat > "$CACHE_DIR/user-data" <<EOF
#cloud-config
hostname: rt-vm
users:
  - name: $VM_USER
    gecos: Motion Master RT VM
    groups: [sudo]
    shell: /bin/bash
    sudo: "ALL=(ALL) NOPASSWD:ALL"
    lock_passwd: false
    plain_text_passwd: $VM_USER
    ssh_authorized_keys:
      - $(cat "$SSH_KEY.pub")
ssh_pwauth: false
package_update: false
package_upgrade: false
EOF

rm -f "$SEED_ISO"
genisoimage -quiet -output "$SEED_ISO" -volid cidata -joliet -rock \
    "$CACHE_DIR/user-data" "$CACHE_DIR/meta-data"

log "creating $VM_DISK_SIZE overlay on $BASE_NAME"
rm -f "$DISK"
qemu-img create -q -f qcow2 -F qcow2 -b "$BASE_IMAGE" "$DISK" "$VM_DISK_SIZE"

log "VM created — start it with ./start.sh"
