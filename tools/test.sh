#!/usr/bin/env bash
set -euo pipefail
preset="${1:-x64-linux-debug}"

# Run niced (and idle-class I/O where available) so the test binaries never starve the
# desktop compositor — see the rationale in build.sh.
nice_prefix=(nice -n 19)
if command -v ionice >/dev/null 2>&1; then
    nice_prefix=(ionice -c3 nice -n 19)
fi

"${nice_prefix[@]}" ctest --test-dir "build/$preset" --output-on-failure
