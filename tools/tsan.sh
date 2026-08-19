#!/usr/bin/env bash
# Builds and runs the concurrency tests under ThreadSanitizer.
#
# The design in docs/LOCKING.md is checked by review; this is the one place a machine checks it.
# Run it after any change to DeviceManager, Device, ProcessData, the game loop or a lock.
#
# Usage: tools/tsan.sh [gtest filter]        default filter: Concurrency.*:StalledCycle.*
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
filter="${1:-Concurrency.*:StalledCycle.*}"
preset=x64-linux-tsan
build="$repo/build/$preset"

# clang, because Fedora ships clang's TSan runtime while GCC's needs a separate libtsan package.
if [ ! -d "$build" ]; then
  cmake --preset "$preset"
fi
nice -n 10 cmake --build "$build" --target mm_node_tests -- -j"${JOBS:-8}"

# halt_on_error keeps the first report on screen; second_deadlock_stack makes a lock-order report
# name both sites. The suppressions file is documented in tsan.supp — read it before adding to it.
TSAN_OPTIONS="suppressions=$repo/tsan.supp halt_on_error=0 second_deadlock_stack=1" \
  nice -n 10 "$build/libs/node/tests/mm_node_tests" --gtest_filter="$filter"
