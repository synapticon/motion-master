#!/usr/bin/env bash
# Update the project semver in every location that isn't auto-derived from VERSION.
# Usage: ./tools/bump-version.sh <new-version>
# Example: ./tools/bump-version.sh 6.0.0-alpha.1
set -euo pipefail

NEW_VERSION="${1:?Usage: $0 <new-version>  e.g. 6.0.0-alpha.0}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OLD_VERSION="$(cat "$ROOT/VERSION")"

if [[ "$NEW_VERSION" == "$OLD_VERSION" ]]; then
  echo "Already at $OLD_VERSION — nothing to do." >&2
  exit 0
fi

# Escape sed BRE metacharacters (.  [  \  *  ^  $) in the old version string.
esc() { printf '%s' "$1" | sed 's/[.[\*^$]/\\&/g'; }
OLD_ESC=$(esc "$OLD_VERSION")

echo "Bumping $OLD_VERSION → $NEW_VERSION"

# 1. VERSION — canonical source; CMake propagates it to version.h and Doxyfile
printf '%s\n' "$NEW_VERSION" > "$ROOT/VERSION"

# 2. vcpkg manifest
sed -i "s/\"version\": \"${OLD_ESC}\"/\"version\": \"${NEW_VERSION}\"/" \
  "$ROOT/vcpkg.json"

# 3. JS/TS package manifests (root workspace manifest + every workspace member)
for f in \
  "$ROOT/package.json" \
  "$ROOT/web/apps/console/package.json" \
  "$ROOT/web/packages/motion-master-client/package.json" \
  "$ROOT/hil/api/package.json"; do
  sed -i "s/\"version\": \"${OLD_ESC}\"/\"version\": \"${NEW_VERSION}\"/" "$f"
done

# 4. OpenAPI spec (info.version)
sed -i "s/version: \"${OLD_ESC}\"/version: \"${NEW_VERSION}\"/" \
  "$ROOT/apps/motion_master/swagger.yml"

# 5. C++ version unit test — StringConstant assertion
sed -i "s/kVersion, \"${OLD_ESC}\"/kVersion, \"${NEW_VERSION}\"/" \
  "$ROOT/libs/core/tests/version_test.cc"

# 6. UI sidebar version badge
sed -i "s/>v${OLD_ESC}</>v${NEW_VERSION}</" \
  "$ROOT/web/apps/console/src/layouts/RootLayout.tsx"

echo "Done — review with: git diff"
echo "Note: tagging v${NEW_VERSION} publishes @synapticon/motion-master-client@${NEW_VERSION} to npm (release.yml)."
