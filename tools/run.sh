#!/usr/bin/env bash
set -euo pipefail

preset="${1:-x64-linux-debug}"
binary="build/${preset}/apps/motion_master/motion-master"
cors_origin="${CORS_ORIGIN:-https://motion-master.synapticon.com}"
log_level="${LOG_LEVEL:-info}"

if [[ ! -x "$binary" ]]; then
    echo "Binary not found: $binary — run ./tools/build.sh first" >&2
    exit 1
fi

binary_dir="$(dirname "$binary")"
bundled_cert_path="$binary_dir/cert.pem"
bundled_key_path="$binary_dir/key.pem"

acme_dir="$HOME/.acme.sh/local.motion-master.synapticon.com_ecc"
acme_cert_path="$acme_dir/fullchain.cer"
acme_key_path="$acme_dir/local.motion-master.synapticon.com.key"

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

# Resolve a cert/key, mirroring the binary's own discovery order.
if [[ -f "$bundled_cert_path" && -f "$bundled_key_path" ]]; then
    cert_path="$bundled_cert_path"; key_path="$bundled_key_path"; cert_kind="bundled"
elif [[ -f "$acme_cert_path" && -f "$acme_key_path" ]]; then
    cert_path="$acme_cert_path"; key_path="$acme_key_path"; cert_kind="Let's Encrypt"
else
    # Fall back to a short-lived self-signed cert (requires accepting the browser exception).
    # SAN is required — browsers ignore CN for hostname validation since ~2017.
    cert_path="$tmpdir/cert.pem"; key_path="$tmpdir/key.pem"
    cert_kind="self-signed — browser exception required"
    openssl req -x509 -newkey rsa:2048 -keyout "$key_path" -out "$cert_path" -days 1 -nodes \
        -subj "/CN=local.motion-master.synapticon.com" \
        -addext "subjectAltName=DNS:local.motion-master.synapticon.com,IP:127.0.0.1" \
        2>/dev/null
fi

# Find the auto-tuning executable. Motion Master looks for it next to itself, which is where every
# install path puts it — but a development build lives under build/<preset>/, and
# install-auto-tuning.sh downloads into the directory it sits in, which is the repository root. So
# name the path explicitly for this run. An absent file needs no entry: Motion Master then warns
# about the path it looked at, which is the same message a real install would give.
auto_tuning_path=""
for candidate in "$binary_dir/auto-tuning" "auto-tuning"; do
    if [[ -x "$candidate" ]]; then
        auto_tuning_path="$(cd "$(dirname "$candidate")" && pwd)/$(basename "$candidate")"
        break
    fi
done
auto_tuning_config=""
if [[ -n "$auto_tuning_path" ]]; then
    auto_tuning_config="\"autoTuning\": { \"binaryPath\": \"$auto_tuning_path\" },"
fi

# Settings now live only in a JSONC config file (no CLI flags), so synthesise one for this run.
config_path="$tmpdir/config.jsonc"
cat > "$config_path" <<EOF
{
  "server": { "corsOrigin": "$cors_origin" },
  "logging": { "level": "$log_level" },
  $auto_tuning_config
  "tls": { "certPath": "$cert_path", "keyPath": "$key_path" }
}
EOF

echo "Starting Motion Master (preset: $preset, cert: $cert_kind, cors: $cors_origin)"
"$binary" --config "$config_path" "${@:2}"
