#!/usr/bin/env bash
set -euo pipefail

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

exec /opt/motion-master/motion-master --cert "$CERT" --key "$KEY" "$@"
