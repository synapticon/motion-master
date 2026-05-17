#!/usr/bin/env bash
set -euo pipefail
preset="${1:-x64-linux-debug}"
rm -rf "build/$preset"
echo "Removed build/$preset"
