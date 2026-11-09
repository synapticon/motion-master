#!/usr/bin/env bash
# Switch the power supply output: ./power.sh on|off
# Set P1535_HOST to reach a board with an address other than the default.
set -euo pipefail

host="${P1535_HOST:-10.10.1.250}"

case "${1:-}" in
  on | off)
    curl -fsS -m 5 "http://$host/output-$1" > /dev/null
    echo "Output $1."
    ;;
  *)
    echo "Usage: $0 on|off" >&2
    exit 2
    ;;
esac
