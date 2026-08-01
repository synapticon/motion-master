#!/usr/bin/env bash
# Download and verify the Debian cloud image the VM overlays are built on.
# Cached in .cache/ and shared by every VM generation, so this runs once.

# shellcheck source=rt/vm/common.sh
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

require curl
require sha512sum

mkdir -p "$CACHE_DIR"

verify_base() {
    (cd "$CACHE_DIR" && grep " \*\?$BASE_NAME\$" SHA512SUMS | sha512sum --check --status -)
}

if [ -f "$BASE_IMAGE" ] && [ -f "$CACHE_DIR/SHA512SUMS" ] && verify_base; then
    log "base image already present and verified: $BASE_IMAGE"
    exit 0
fi

log "fetching checksums from $BASE_URL"
curl -fsSL --retry 3 -o "$CACHE_DIR/SHA512SUMS" "$BASE_URL/SHA512SUMS"

grep -q " \*\?$BASE_NAME\$" "$CACHE_DIR/SHA512SUMS" ||
    die "$BASE_NAME is not listed in $BASE_URL/SHA512SUMS"

log "downloading $BASE_NAME (~400 MB, cached for future runs)"
curl -fL --retry 3 --progress-bar -o "$BASE_IMAGE.part" "$BASE_URL/$BASE_NAME"
mv "$BASE_IMAGE.part" "$BASE_IMAGE"

log "verifying checksum"
verify_base || die "checksum mismatch for $BASE_NAME"

log "base image ready: $BASE_IMAGE"
