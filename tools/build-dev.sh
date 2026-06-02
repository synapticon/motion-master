#!/usr/bin/env bash
set -euo pipefail
here="$(dirname "$0")"

# build-dev is the developer flow: build (stamping capabilities by default, so the binary
# can be run against hardware) then run the test suite. Pass --no-setcap to skip the
# privileged step so no sudo is needed (CI / automated agents).
setcap=1
preset=""
extra=()
for arg in "$@"; do
    case "$arg" in
        --no-setcap) setcap=0 ;;
        --setcap) setcap=1 ;;
        *) if [[ -z "$preset" ]]; then preset="$arg"; fi; extra+=("$arg") ;;
    esac
done
preset="${preset:-x64-linux-debug}"

if [[ "$setcap" -eq 1 ]]; then
    "$here/build.sh" --setcap "${extra[@]}"
else
    "$here/build.sh" "${extra[@]}"
fi
"$here/test.sh" "$preset"
