#!/usr/bin/env bash
set -euo pipefail
preset="${1:-x64-linux-debug}"

# The first configure of a preset builds every vcpkg dependency (uWebSockets et al.) — the
# heaviest burst of all. Cap vcpkg's own build concurrency and run niced (idle-class I/O
# where available) so it can't thrash the machine into a desktop-compositor crash. See the
# rationale in build.sh. Override the cap with MM_BUILD_JOBS=N; MM_BUILD_JOBS=0 disables it.
jobs="${MM_BUILD_JOBS:-}"
if [[ -z "$jobs" ]]; then
    cores="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
    jobs=$(( cores / 3 ))
    (( jobs < 1 )) && jobs=1
fi
(( jobs > 0 )) && export VCPKG_MAX_CONCURRENCY="$jobs"

nice_prefix=(nice -n 19)
if command -v ionice >/dev/null 2>&1; then
    nice_prefix=(ionice -c3 nice -n 19)
fi

"${nice_prefix[@]}" cmake --preset "$preset"
