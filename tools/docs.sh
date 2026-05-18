#!/usr/bin/env bash
set -euo pipefail

PRESET="${1:-x64-linux-debug}"
cmake --build --preset "$PRESET" --target docs
