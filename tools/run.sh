#!/usr/bin/env bash
set -euo pipefail

preset="${1:-x64-linux-debug}"
binary="build/${preset}/apps/motion_master/motion-master"
cors_origin="${CORS_ORIGIN:-https://motion-master.synapticon.com}"

if [[ ! -x "$binary" ]]; then
    echo "Binary not found: $binary — run ./tools/build.sh first" >&2
    exit 1
fi

binary_dir="$(dirname "$binary")"
bundled_cert="$binary_dir/cert.pem"
bundled_key="$binary_dir/key.pem"

acme_dir="$HOME/.acme.sh/local.motion-master.synapticon.com_ecc"
acme_cert="$acme_dir/fullchain.cer"
acme_key="$acme_dir/local.motion-master.synapticon.com.key"

if [[ -f "$bundled_cert" && -f "$bundled_key" ]]; then
    echo "Starting Motion Master (preset: $preset, cert: bundled, cors: $cors_origin)"
    "$binary" --cert "$bundled_cert" --key "$bundled_key" --cors-origin "$cors_origin" "${@:2}"
elif [[ -f "$acme_cert" && -f "$acme_key" ]]; then
    echo "Starting Motion Master (preset: $preset, cert: Let's Encrypt, cors: $cors_origin)"
    "$binary" --cert "$acme_cert" --key "$acme_key" --cors-origin "$cors_origin" "${@:2}"
else
    # Fall back to a short-lived self-signed cert for environments without the
    # acme.sh certificate (requires accepting the browser security exception).
    tmpdir=$(mktemp -d)
    trap 'rm -rf "$tmpdir"' EXIT

    cert="$tmpdir/cert.pem"
    key="$tmpdir/key.pem"

    # SAN is required — browsers ignore CN for hostname validation since ~2017.
    openssl req -x509 -newkey rsa:2048 -keyout "$key" -out "$cert" -days 1 -nodes \
        -subj "/CN=local.motion-master.synapticon.com" \
        -addext "subjectAltName=DNS:local.motion-master.synapticon.com,IP:127.0.0.1" \
        2>/dev/null

    echo "Starting Motion Master (preset: $preset, cert: self-signed — browser exception required, cors: $cors_origin)"
    "$binary" --cert "$cert" --key "$key" --cors-origin "$cors_origin" "${@:2}"
fi
