#!/usr/bin/env bash
# Lint all of the project's CMake files (CMakeLists.txt + *.cmake) with
# cmake-lint, using the repo's .cmake-format.yaml (100-column limit).
# Third-party trees (extern/, build/, node_modules/) and the vcpkg overlay
# ports (ports/ — kept in upstream vcpkg style) are excluded. Exits non-zero
# on any finding, so it plugs straight into tools/check.sh.
#
# cmake-lint ships with the cmakelang package — the same suite as cmake-format.
# Install it with `pip install cmakelang`, or point CMAKE_LINT at any copy
# (e.g. a Mason install under ~/.local/share/nvim).
#
# Usage:
#   ./tools/lint-cmake.sh
set -euo pipefail

cd "$(dirname "$0")/.."

case "${1:-}" in
  -h | --help)
    sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
    ;;
  "") ;;
  *)
    echo "unknown option: $1 (try --help)" >&2
    exit 2
    ;;
esac

# Locate cmake-lint: an explicit override, then PATH, then a Mason install.
cl="${CMAKE_LINT:-}"
if [[ -z "$cl" ]]; then
  if command -v cmake-lint >/dev/null 2>&1; then
    cl="cmake-lint"
  elif [[ -x "$HOME/.local/share/nvim/mason/bin/cmake-lint" ]]; then
    cl="$HOME/.local/share/nvim/mason/bin/cmake-lint"
  else
    echo "cmake-lint not found — install cmakelang (pip install cmakelang) or set CMAKE_LINT" >&2
    exit 1
  fi
fi

# Collect the project's CMake files, NUL-delimited so paths with spaces survive.
mapfile -d '' files < <(
  find . \( -path ./extern -o -path ./build -o -path ./node_modules -o -path ./ports \) -prune -o \
    \( -name CMakeLists.txt -o -name '*.cmake' \) -type f -print0
)

if [[ ${#files[@]} -eq 0 ]]; then
  echo "no CMake files found" >&2
  exit 1
fi

"$cl" "${files[@]}"
echo "cmake-lint clean (${#files[@]} CMake files)"
