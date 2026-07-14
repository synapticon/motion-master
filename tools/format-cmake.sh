#!/usr/bin/env bash
# Format all of the project's CMake files (CMakeLists.txt + *.cmake) with
# cmake-format, using the repo's .cmake-format.yaml (100-column limit).
# Third-party trees (extern/, build/, node_modules/) and the vcpkg overlay
# ports (ports/ — kept in upstream vcpkg style) are excluded.
#
# cmake-format ships with the cmakelang package — the same suite as the
# cmake-lint your editor runs. Install it with `pip install cmakelang`, or
# point CMAKE_FORMAT at any copy (e.g. a Mason install under ~/.local/share/nvim).
#
# Usage:
#   ./tools/format-cmake.sh           # format in place
#   ./tools/format-cmake.sh --check   # report files that would change, don't edit (exit 1 if any)
set -euo pipefail

cd "$(dirname "$0")/.."

check=0
case "${1:-}" in
  --check) check=1 ;;
  -h | --help)
    sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
    ;;
  "") ;;
  *)
    echo "unknown option: $1 (try --help)" >&2
    exit 2
    ;;
esac

# Locate cmake-format: an explicit override, then PATH, then a Mason install.
cf="${CMAKE_FORMAT:-}"
if [[ -z "$cf" ]]; then
  if command -v cmake-format >/dev/null 2>&1; then
    cf="cmake-format"
  elif [[ -x "$HOME/.local/share/nvim/mason/bin/cmake-format" ]]; then
    cf="$HOME/.local/share/nvim/mason/bin/cmake-format"
  else
    echo "cmake-format not found — install cmakelang (pip install cmakelang) or set CMAKE_FORMAT" >&2
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

if [[ "$check" -eq 1 ]]; then
  status=0
  for f in "${files[@]}"; do
    if ! "$cf" --check "$f" >/dev/null 2>&1; then
      echo "would reformat: $f"
      status=1
    fi
  done
  [[ "$status" -eq 0 ]] && echo "all ${#files[@]} CMake files are formatted"
  exit "$status"
fi

"$cf" -i "${files[@]}"
echo "formatted ${#files[@]} CMake files"
