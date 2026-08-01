#!/usr/bin/env bash
# Shut the VM down, gracefully if it will cooperate.

# shellcheck source=rt/vm/common.sh
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

SHUTDOWN_TIMEOUT="${RT_VM_SHUTDOWN_TIMEOUT:-30}"

if ! vm_is_running; then
    log "not running"
    rm -f "$PID_FILE"
    exit 0
fi

pid="$(cat "$PID_FILE")"

if [ -S "$MONITOR_SOCKET" ] && command -v socat >/dev/null 2>&1; then
    log "sending ACPI powerdown"
    printf 'system_powerdown\n' | socat - "UNIX-CONNECT:$MONITOR_SOCKET" >/dev/null 2>&1 || true
else
    # No socat: SIGTERM makes QEMU perform the same orderly shutdown.
    log "sending SIGTERM to qemu (pid $pid)"
    kill "$pid" 2>/dev/null || true
fi

deadline=$((SECONDS + SHUTDOWN_TIMEOUT))
while kill -0 "$pid" 2>/dev/null; do
    if [ "$SECONDS" -ge "$deadline" ]; then
        log "still up after ${SHUTDOWN_TIMEOUT}s — killing"
        kill -9 "$pid" 2>/dev/null || true
        break
    fi
    sleep 1
done

rm -f "$PID_FILE" "$MONITOR_SOCKET"
log "stopped"
