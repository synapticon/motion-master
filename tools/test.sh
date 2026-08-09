#!/usr/bin/env bash
set -euo pipefail
preset="${1:-x64-linux-debug}"

# Run niced (and idle-class I/O where available) so the test binaries never starve the
# desktop compositor — see the rationale in build.sh.
nice_prefix=(nice -n 19)
if command -v ionice >/dev/null 2>&1; then
    nice_prefix=(ionice -c3 nice -n 19)
fi

# The ThreadSanitizer preset needs its suppression file, or every run drowns in the one deliberate
# race in the tree (ProcessDataRing's seqlock). Exported rather than baked into the preset because
# it is a runtime option, not a build one. halt_on_error makes a reported race fail the test that
# found it instead of being buried in passing output.
if [[ "$preset" == *tsan* ]]; then
    export TSAN_OPTIONS="suppressions=$PWD/tools/tsan.supp halt_on_error=1 second_deadlock_stack=1"
fi

"${nice_prefix[@]}" ctest --test-dir "build/$preset" --output-on-failure
