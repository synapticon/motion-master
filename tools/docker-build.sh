#!/usr/bin/env bash
# Build the Motion Master Docker image from the current VERSION and tag it with the exact
# version plus a rolling tag: `latest` for a stable release, `next` for a prerelease.
#
# Docker/OCI tag convention: tags are BARE semver with no `v` prefix (unlike git tags),
# e.g. 6.0.0-alpha.34 — so the version tag is taken verbatim from the VERSION file. A semver
# prerelease has a `-` (6.0.0-alpha.34); such builds move `next`, not `latest`, so a plain
# `docker pull <repo>` (which resolves `latest`) always lands on the newest STABLE image.
#
# Usage:
#   ./tools/docker-build.sh                       # image repo: markosankovic/motion-master
#   ./tools/docker-build.sh myuser/motion-master  # override the image repository
#   IMAGE=myuser/motion-master ./tools/docker-build.sh
#
# Prerequisite: the vcpkg submodule must be initialised (git submodule update --init --recursive).
# Pushing is left to you (it needs `docker login`); the push commands are printed at the end.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION=$(cat "$REPO_DIR/VERSION")
IMAGE="${1:-${IMAGE:-markosankovic/motion-master}}"

if [[ "$VERSION" == *-* ]]; then
    ROLLING="next"
else
    ROLLING="latest"
fi

export DOCKER_BUILDKIT=1

echo "Building $IMAGE:$VERSION (also tagging :$ROLLING)..."
docker build \
    -t "$IMAGE:$VERSION" \
    -t "$IMAGE:$ROLLING" \
    "$REPO_DIR"

echo
echo "Built and tagged:"
echo "  $IMAGE:$VERSION"
echo "  $IMAGE:$ROLLING"
echo
echo "Push with:"
echo "  docker push $IMAGE:$VERSION"
echo "  docker push $IMAGE:$ROLLING"
