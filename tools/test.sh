#!/usr/bin/env bash
set -euo pipefail
preset="${1:-x64-linux-debug}"
ctest --test-dir "build/$preset" --output-on-failure
