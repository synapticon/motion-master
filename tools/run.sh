#!/usr/bin/env bash
set -euo pipefail

preset="${1:-x64-linux-debug}"
binary="build/${preset}/apps/motion_master/motion-master"

if [[ ! -x "$binary" ]]; then
    echo "Binary not found: $binary — run ./tools/build.sh first" >&2
    exit 1
fi

# Use a temp dir so the key never touches the working tree and is cleaned up
# automatically even if the server is killed.
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

cert="$tmpdir/cert.pem"
key="$tmpdir/key.pem"

# Generate a short-lived self-signed cert for the dev hostname.
# The SAN is required — browsers ignore CN for hostname validation since ~2017.
openssl req -x509 -newkey rsa:2048 -keyout "$key" -out "$cert" -days 1 -nodes \
    -subj "/CN=local.motion-master.synapticon.com" \
    -addext "subjectAltName=DNS:local.motion-master.synapticon.com,IP:127.0.0.1" \
    2>/dev/null

echo "Starting Motion Master (preset: $preset)"
"$binary" --cert "$cert" --key "$key" "${@:2}"
