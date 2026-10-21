#!/usr/bin/env bash
# Build the Motion Master Docker image from the current VERSION and tag it with that version.
#
# Docker tags are bare semver with no `v` prefix, unlike git tags, so the tag is the VERSION file
# verbatim, for example 6.0.0-alpha.95. The version is the only tag. `latest` on
# synapticon/motion-master is not this software, and this line adds no rolling tag.
#
# docker-publish.yml publishes the image for amd64 and arm64 on request. This script is for a local
# build, which runs for the architecture of the Docker host.
#
# Usage:
#   ./tools/docker-build.sh                       # image repository: synapticon/motion-master
#   ./tools/docker-build.sh myuser/motion-master  # another image repository
#   IMAGE=myuser/motion-master ./tools/docker-build.sh
#
# Prerequisite: the vcpkg submodule must be initialised (git submodule update --init --recursive).
# Pushing is left to you (it needs `docker login`); the push command is printed at the end.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION=$(cat "$REPO_DIR/VERSION")
IMAGE="${1:-${IMAGE:-synapticon/motion-master}}"

export DOCKER_BUILDKIT=1

echo "Building $IMAGE:$VERSION..."
docker build -t "$IMAGE:$VERSION" "$REPO_DIR"

echo
echo "Built and tagged:"
echo "  $IMAGE:$VERSION"
echo
echo "Push with:"
echo "  docker push $IMAGE:$VERSION"
