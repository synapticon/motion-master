#!/usr/bin/env bash
# Generate the object addresses for C++, Python and TypeScript from the pinned ESI, in one run.
#
#   tools/generate-object-addresses.sh [preset]           write the five files, format the headers
#   tools/generate-object-addresses.sh [preset] --check   fail when a committed file differs
#
# The check compares the Python and TypeScript files byte for byte. It compares the C++ headers
# with every whitespace character removed, because clang-format lays them out and the clang-format
# job checks that layout. So the check needs no clang-format, and a different clang-format version
# cannot fail it.
set -euo pipefail

preset="x64-linux-debug"
check=false
for arg in "$@"; do
  case "$arg" in
    --check) check=true ;;
    *) preset="$arg" ;;
  esac
done

root="$(cd "$(dirname "$0")/.." && pwd)"
binary="$root/build/$preset/apps/motion_master/motion-master"
esi="$root/libs/etg/tests/data/somanet-v5.6.6.xml"
headers=(profile_device_objects.h cia402_drive_objects.h somanet_drive_objects.h)
python="clients/python/motion_master_objects.py"
typescript="web/packages/motion-master-client/src/objects.ts"

if [[ ! -x "$binary" ]]; then
  echo "error: $binary not found. Build the preset $preset first." >&2
  exit 1
fi

if [[ "$check" == false ]]; then
  "$binary" generate-object-addresses --esi "$esi" --out "$root/libs/node" \
    --python "$root/$python" --typescript "$root/$typescript"
  for header in "${headers[@]}"; do
    clang-format -i "$root/libs/node/$header"
  done
  exit 0
fi

out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
# The ESI warnings are the same on every run, so the check prints them only when the run fails.
if ! "$binary" generate-object-addresses --esi "$esi" --out "$out" \
  --python "$out/$(basename "$python")" --typescript "$out/$(basename "$typescript")" \
  >"$out/log" 2>&1; then
  cat "$out/log" >&2
  exit 1
fi

differs=()
for header in "${headers[@]}"; do
  if ! cmp -s <(tr -d '[:space:]' <"$out/$header") <(tr -d '[:space:]' <"$root/libs/node/$header"); then
    differs+=("libs/node/$header")
  fi
done
for file in "$python" "$typescript"; do
  if ! cmp -s "$out/$(basename "$file")" "$root/$file"; then
    differs+=("$file")
  fi
done

if ((${#differs[@]} > 0)); then
  echo "::error::these files differ from what the generator writes from the pinned ESI:" >&2
  printf '  %s\n' "${differs[@]}" >&2
  echo "Run tools/generate-object-addresses.sh and commit the result." >&2
  exit 1
fi
echo "the object addresses match the pinned ESI in C++, Python and TypeScript"
