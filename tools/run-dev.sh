#!/usr/bin/env bash
# Run Motion Master for local UI development: CORS open to the Vite dev server
# (http://localhost:5173) and trace-level logging. Extra args are forwarded.
# CORS origin and log level are passed via env vars — run.sh bakes them into a generated
# config file, since there are no CLI flags for these settings.
set -euo pipefail
preset="${1:-x64-linux-debug}"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CORS_ORIGIN=http://localhost:5173 LOG_LEVEL=trace "$script_dir/run.sh" "$preset" "${@:2}"
