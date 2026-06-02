#!/usr/bin/env bash
set -euo pipefail
preset="${1:-x64-linux-debug}"
here="$(dirname "$0")"
"$here/build.sh" "$preset"
"$here/test.sh" "$preset"
