#!/usr/bin/env bash
# Throw the VM away and boot a pristine one. Only the overlay is discarded, so
# this costs a few seconds rather than a re-download or a reinstall.
#
# This is the point of the whole directory: provisioning is only trustworthy if
# it has been proven from a clean machine, and a clean machine has to be cheap.

# shellcheck source=rt/vm/common.sh
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

log "resetting to a pristine $BASE_NAME"
"$VM_DIR/stop.sh"
"$VM_DIR/create.sh"
"$VM_DIR/start.sh"
