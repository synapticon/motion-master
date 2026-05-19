#!/usr/bin/env bash
set -euo pipefail

# Use CERT and KEY env vars if provided; otherwise generate a short-lived
# self-signed cert for the dev hostname.
CERT="${CERT:-}"
KEY="${KEY:-}"

if [[ -z "$CERT" || -z "$KEY" ]]; then
    tmpdir=$(mktemp -d)
    trap 'rm -rf "$tmpdir"' EXIT
    CERT="$tmpdir/cert.pem"
    KEY="$tmpdir/key.pem"
    openssl req -x509 -newkey rsa:2048 -keyout "$KEY" -out "$CERT" -days 1 -nodes \
        -subj "/CN=local.motion-master.synapticon.com" \
        -addext "subjectAltName=DNS:local.motion-master.synapticon.com,IP:127.0.0.1" \
        2>/dev/null
fi

exec /app/motion-master --cert "$CERT" --key "$KEY" "$@"
