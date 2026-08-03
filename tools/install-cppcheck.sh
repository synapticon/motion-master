#!/usr/bin/env bash
#
# Builds and installs the pinned cppcheck (see .cppcheck-version) from source.
#
# Why from source rather than a package: cppcheck's findings change between releases — 2.13 reports a
# scope guard's null check as always-true where 2.21 correctly does not — so a suppression list is
# only valid for the version it was written against, and a local run is only predictive of CI when
# both run the same binary. No distribution offers a choice of version (Ubuntu 24.04 has 2.13,
# Fedora 44 has 2.21.x) and cppcheck publishes no Linux binaries, so pinning one version everywhere
# means compiling it. It is a small CMake project; the build takes a couple of minutes.
#
# Usage: tools/install-cppcheck.sh [prefix]
#   prefix defaults to ~/.local, so the binary lands at <prefix>/bin/cppcheck. Put that on PATH.
#   JOBS=<n> caps build parallelism (default: every core). Worth setting on a desktop — a full-fanout
#   compile of anything is enough to make an interactive session unusable while it runs.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
version="$(tr -d '[:space:]' < "$repo_root/.cppcheck-version")"
prefix="${1:-$HOME/.local}"

if [ -z "$version" ]; then
  echo "error: .cppcheck-version is empty" >&2
  exit 1
fi

installed="$prefix/bin/cppcheck"
if [ -x "$installed" ] && "$installed" --version 2>/dev/null | grep -qE "(^| )$version\$"; then
  echo "cppcheck $version already installed at $installed"
  exit 0
fi

for tool in git cmake; do
  if ! command -v "$tool" > /dev/null; then
    echo "error: $tool is required to build cppcheck" >&2
    exit 1
  fi
done

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo "Fetching cppcheck $version..."
git clone --depth 1 --branch "$version" https://github.com/danmar/cppcheck.git "$work/src"

# FILESDIR is set explicitly because cppcheck loads its library configuration (std.cfg and friends)
# from there at runtime; left to the default, an installed binary can start up unable to find them.
# USE_MATCHCOMPILER precompiles the token patterns, which is most of cppcheck's speed.
echo "Building cppcheck $version..."
cmake -S "$work/src" -B "$work/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DUSE_MATCHCOMPILER=ON \
  -DFILESDIR="$prefix/share/Cppcheck" \
  -DCMAKE_INSTALL_PREFIX="$prefix" > /dev/null
cmake --build "$work/build" --parallel "${JOBS:-$(getconf _NPROCESSORS_ONLN)}" > /dev/null
cmake --install "$work/build" > /dev/null

echo "Installed: $("$installed" --version) -> $installed"
