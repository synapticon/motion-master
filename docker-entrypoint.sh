#!/usr/bin/env bash
set -euo pipefail

# Settings (ports, fieldbus, log level, TLS paths) are configured only through a JSONC config file
# passed with --config; there are no CLI flags for them.
#
# If MM_CONFIG is set, that config is used verbatim (the user owns every setting, including TLS
# paths — leave tls.certPath/keyPath empty to fall back to the baked-in cert next to the binary).
# Otherwise this entrypoint resolves a TLS cert/key and generates a minimal config pointing at it.

# `exec` replaces this shell with the binary rather than spawning a child: the binary becomes the
# container's PID 1 so `docker stop`'s SIGTERM reaches it directly for a graceful shutdown (a
# lingering parent shell would not forward signals). exec never returns on success, so if this
# branch is taken the process is replaced here and nothing below runs; the fall-through path at the
# end applies only when MM_CONFIG is unset and this exec was skipped.
if [[ -n "${MM_CONFIG:-}" ]]; then
    exec /opt/motion-master/motion-master --config "$MM_CONFIG" "$@"
fi

# TLS certificate resolution — mirrors the same priority as tools/run.sh:
#   1. CERT / KEY env vars (explicit override)
#   2. cert.pem / key.pem next to the binary (baked in on release image builds)
#   3. ~/.acme.sh/local.motion-master.synapticon.com_ecc/ (mount from host for dev builds)
#   4. Self-signed fallback (browser security exception required)

CERT="${CERT:-}"
KEY="${KEY:-}"

BUNDLED_CERT="/opt/motion-master/cert.pem"
BUNDLED_KEY="/opt/motion-master/key.pem"

ACME_DIR="$HOME/.acme.sh/local.motion-master.synapticon.com_ecc"
ACME_CERT="$ACME_DIR/fullchain.cer"
ACME_KEY="$ACME_DIR/local.motion-master.synapticon.com.key"

if [[ -n "$CERT" && -n "$KEY" ]]; then
    : # use env vars as-is
elif [[ -f "$BUNDLED_CERT" && -s "$BUNDLED_CERT" && -f "$BUNDLED_KEY" && -s "$BUNDLED_KEY" ]]; then
    CERT="$BUNDLED_CERT"
    KEY="$BUNDLED_KEY"
elif [[ -f "$ACME_CERT" && -f "$ACME_KEY" ]]; then
    CERT="$ACME_CERT"
    KEY="$ACME_KEY"
else
    tmpdir=$(mktemp -d)
    trap 'rm -rf "$tmpdir"' EXIT
    CERT="$tmpdir/cert.pem"
    KEY="$tmpdir/key.pem"
    openssl req -x509 -newkey rsa:2048 -keyout "$KEY" -out "$CERT" -days 1 -nodes \
        -subj "/CN=local.motion-master.synapticon.com" \
        -addext "subjectAltName=DNS:local.motion-master.synapticon.com,IP:127.0.0.1" \
        2>/dev/null
fi

CONFIG="$(mktemp --suffix=.jsonc)"
cat > "$CONFIG" <<EOF
{ "tls": { "certPath": "$CERT", "keyPath": "$KEY" } }
EOF

# exec (see the note above) — replaces the shell so the binary is PID 1 and gets SIGTERM directly.
# Because exec never returns, the EXIT trap set above never fires; that is intentional, as the
# self-signed cert in $tmpdir must outlive this script and stay readable for the binary's lifetime.
exec /opt/motion-master/motion-master --config "$CONFIG" "$@"
