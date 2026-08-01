#!/usr/bin/env bash
# Run shellcheck over every shell script tracked in the repository.
#
# Usage: ./tools/shellcheck.sh [extra shellcheck args...]
#
# The file list comes from git rather than a glob, so a new script is covered
# the moment it is added and nothing under build/ or extern/ is ever picked up.
# packaging/postinst is included explicitly: it is a shell script with no
# extension (dpkg requires that name).
set -euo pipefail

cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

command -v shellcheck >/dev/null 2>&1 || {
    echo "shellcheck not installed — run ./tools/install-deps.sh" >&2
    exit 1
}

# -x follows 'source' directives, which is what makes the rt/vm scripts
# checkable against their shared common.sh.
{ git ls-files -- '*.sh' '*.bash'; echo packaging/postinst; } |
    xargs shellcheck -x "$@"

echo "shellcheck: clean"
