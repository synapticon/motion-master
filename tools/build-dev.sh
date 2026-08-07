#!/usr/bin/env bash
set -euo pipefail
here="$(dirname "$0")"

# build-dev is the developer flow: build, stamping capabilities by default so the binary can
# be run against hardware. Pass --no-setcap to skip the privileged step so no sudo is needed
# (CI / automated agents). Run ./tools/test.sh separately when you want the test suite.
setcap=1
extra=()
for arg in "$@"; do
    case "$arg" in
        --no-setcap) setcap=0 ;;
        --setcap) setcap=1 ;;
        *) extra+=("$arg") ;;
    esac
done

if [[ "$setcap" -eq 1 ]]; then
    "$here/build.sh" --setcap "${extra[@]}"
else
    "$here/build.sh" "${extra[@]}"
fi
