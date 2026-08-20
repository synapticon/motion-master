#!/usr/bin/env bash
# Copy a standalone-autotuning release into Motion Master's rolling `auto-tuning` release.
#
# install-auto-tuning.sh explains why Motion Master downloads auto-tuning instead of a copy in
# every release. This script is what makes that download possible. The repository that builds
# auto-tuning is private, so no script on a user's machine can read its assets. This script
# copies those assets to the public Motion Master repository, under one tag that never changes.
# Every install path then has one URL, and needs no credentials. The TLS certificate uses the
# same arrangement.
#
# Run this script by hand. It is not a workflow. It reads a private repository, and a workflow
# would need a token with that access stored on a public repository. Auto-tuning ships a few
# times a year, so the manual step is cheap.
#
# Usage: ./tools/publish-auto-tuning.sh [tag]
#
# Without a tag, the script copies the newest standalone-autotuning release. It needs gh,
# authenticated as somebody who can read synapticon/standalone-autotuning and can publish
# releases on synapticon/motion-master.
set -euo pipefail

SOURCE_REPO=synapticon/standalone-autotuning
TARGET_REPO=synapticon/motion-master
ROLLING_TAG=auto-tuning
TITLE="Auto-tuning (rolling)"

# The four platforms that auto-tuning is built for. There is no build for ARM64 Windows, because
# one mandatory numerical dependency has never shipped a wheel for it. ARM64 Windows runs the
# x86_64 executable under the emulation that the operating system provides. The upstream release
# also holds unsuffixed copies of the two x86_64 executables, for older consumers. Copy the names
# that carry the platform.
ASSETS=(
    standalone-autotuning-linux-x86_64
    standalone-autotuning-linux-arm64
    standalone-autotuning-macos-arm64
    standalone-autotuning-windows-x86_64.exe
)

tag="${1:-$(gh release view --repo "$SOURCE_REPO" --json tagName --jq .tagName)}"

dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT

echo "Copying $SOURCE_REPO $tag"
for asset in "${ASSETS[@]}"; do
    gh release download "$tag" --repo "$SOURCE_REPO" --pattern "$asset" --dir "$dir"
    echo "  downloaded $asset ($(du -h "$dir/$asset" | cut -f1))"
done

# Ask the executable for its version, when this machine can run one of the four. Nothing else
# ties the assets to the tag. This script writes the release notes from the tag, so those notes
# would repeat a wrong tag instead of catching it.
if [ "$(uname -s)-$(uname -m)" = "Linux-x86_64" ]; then
    chmod +x "$dir/standalone-autotuning-linux-x86_64"
    reported=$("$dir/standalone-autotuning-linux-x86_64" --version 2>/dev/null || true)
    if [ -z "$reported" ]; then
        echo "  the x86_64 executable does not answer --version. It is older than that flag."
    elif [ "$reported" != "${tag#v}" ]; then
        echo "The x86_64 executable reports $reported, and the tag says ${tag#v}." >&2
        exit 1
    else
        echo "  the x86_64 executable reports $reported"
    fi
fi

paths=()
for asset in "${ASSETS[@]}"; do
    paths+=("$dir/$asset")
done

notes="standalone-autotuning $tag, copied from the repository that builds it.

Motion Master starts this executable as a child process, and logs the version it answers with.
Motion Master downloads it instead of a copy in every release: the file is about 65 MB, the
server binary is about 5 MB, and auto-tuning changes a few times a year.

This release rolls. The tag stays \`auto-tuning\`, and the assets are replaced whenever a new
auto-tuning version is copied here. So \`install-auto-tuning.sh\`, \`setup.ps1\`, the deb and rpm
install scripts and the Dockerfile all name one URL that never changes.

Updated $(date -u +%Y-%m-%dT%H:%M:%SZ)."

if gh release view "$ROLLING_TAG" --repo "$TARGET_REPO" >/dev/null 2>&1; then
    gh release edit "$ROLLING_TAG" --repo "$TARGET_REPO" --title "$TITLE" --notes "$notes"
    gh release upload "$ROLLING_TAG" --repo "$TARGET_REPO" --clobber "${paths[@]}"
else
    # --prerelease keeps this release off the repository's "Latest" badge. --latest=false alone
    # does not, once this is the only release that is not a prerelease. The download URL of a
    # fixed tag is not affected. This is the same reason as the tls-cert release in
    # cert-renewal.yml.
    gh release create "$ROLLING_TAG" "${paths[@]}" \
        --repo "$TARGET_REPO" \
        --title "$TITLE" \
        --notes "$notes" \
        --prerelease
fi

echo "Published to https://github.com/$TARGET_REPO/releases/tag/$ROLLING_TAG"
