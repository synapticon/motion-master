#!/usr/bin/env bash
set -euo pipefail
preset="${1:-x64-linux-debug}"
ninja -C "build/$preset" format
ninja -C "build/$preset" cppcheck
ninja -C "build/$preset" lint
"$(dirname "$0")/lint-cmake.sh"
"$(dirname "$0")/shellcheck.sh"
