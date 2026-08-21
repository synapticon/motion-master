#!/usr/bin/env bash
# Download the auto-tuning executable that Motion Master starts as a child process.
#
# The auto-tuning executable is about 65 MB. The Motion Master binary is about 5 MB. Motion
# Master ships on every tag, and auto-tuning changes a few times a year. A copy in every release
# would therefore make each download many times larger, for a file that rarely changes. So the
# file lives in one rolling release, at a URL that does not change, and every install path
# downloads it from there.
#
# Auto-tuning is a commissioning tool: it is used while a drive is being configured, and a machine
# that only runs an already-configured drive never calls it. So Motion Master runs without it, a
# failed download is not an error, and this script reports what happened and always exits with
# status 0. That is also what lets a package install succeed on a machine with no network.
#
# Usage: ./install-auto-tuning.sh [directory]
#
# The directory defaults to the directory of this script. That is where every install path needs
# the file: next to the motion-master binary.
set -euo pipefail

dir="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}"
target="$dir/auto-tuning"
url=https://github.com/synapticon/motion-master/releases/download/auto-tuning

# One asset per platform. On Windows, setup.ps1 does this instead. There is no build for 32-bit
# Linux, and none for an Intel Mac. A machine that matches nothing is therefore normal here.
case "$(uname -s)-$(uname -m)" in
  Linux-x86_64) asset=standalone-autotuning-linux-x86_64 ;;
  Linux-aarch64 | Linux-arm64) asset=standalone-autotuning-linux-arm64 ;;
  Darwin-arm64) asset=standalone-autotuning-macos-arm64 ;;
  *) asset= ;;
esac

# Keep a file that is already there. Motion Master is released often, and auto-tuning is not. A
# download of 65 MB on every upgrade is the cost this whole arrangement removes.
if [ -e "$target" ]; then
  echo "Auto-tuning is already installed: $("$target" --version 2>/dev/null || echo "version unknown")"
  echo "To replace it, delete $target and run this script again."
  exit 0
fi

if [ -z "$asset" ]; then
  echo "There is no auto-tuning build for $(uname -s) $(uname -m). Motion Master runs without it."
  exit 0
fi

# Download to a temporary name, then move the file. A partial download must never look like an
# installed executable.
if curl -fsSL --retry 2 -o "$target.part" "$url/$asset"; then
  chmod 755 "$target.part"
  mv "$target.part" "$target"
  # Run the executable once. This proves that the file works, and it prints the version. It
  # takes about one second, because the executable unpacks itself at every start.
  echo "Installed auto-tuning $("$target" --version 2>/dev/null || echo "of an unknown version") in $dir"
else
  rm -f "$target.part"
  echo "Could not download $asset. Motion Master runs without auto-tuning."
  echo "Run $(basename "${BASH_SOURCE[0]}") again when this machine is online."
fi
