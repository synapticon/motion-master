#!/usr/bin/env bash
# Run Motion Master for local UI development: CORS open to the Vite dev server
# (http://localhost:5173) and debug-level logging. Extra args are forwarded.
# CORS origin and log level are passed via env vars — run.sh bakes them into a generated
# config file, since there are no CLI flags for these settings.
# Debug, not trace: the background ParameterRefresher polls SDOs continuously, and its
# per-read logs (demoted to trace by ScopedQuietSdoLog in comm/sdo_log.h) would flood the
# terminal with SDO reads at trace level. Debug keeps user-initiated reads visible without them.
set -euo pipefail
preset="${1:-x64-linux-debug}"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CORS_ORIGIN=http://localhost:5173 LOG_LEVEL=debug "$script_dir/run.sh" "$preset" "${@:2}"
