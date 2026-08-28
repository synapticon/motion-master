#!/usr/bin/env bash
# Publish the Raspberry Pi 5 image so anyone can download and flash it.
#
#   ./publish-rpi-image.sh                 # compress, then ask before uploading
#   ./publish-rpi-image.sh --dry-run       # compress and print what would be sent
#   ./publish-rpi-image.sh --yes           # no prompt
#
# The image is compressed with xz because Raspberry Pi Imager and balenaEtcher
# both read .xz directly, so nobody has to unpack 8 GB by hand before writing a
# card. Most of that 8 GB is a root filesystem grown to make room for
# provisioning and then left empty, which is why the download is a fraction of
# the size.
#
# One object name, overwritten each time. The documentation points at a URL that
# has to keep working, and a version in the filename would move it on every
# release. The version is inside the image, and the .sha256 beside it says which
# build a given download is.
#
# The SSH key the image authorises goes up with it. See the comment on KEY.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly SCRIPT_DIR
readonly CACHE_DIR="${RT_IMAGE_CACHE:-$SCRIPT_DIR/.cache}"
readonly IMAGE="$CACHE_DIR/motion-master-rpi5.img"
readonly ARCHIVE="$IMAGE.xz"

readonly BUCKET="${RT_PUBLISH_BUCKET:-synapticon}"
readonly PREFIX="${RT_PUBLISH_PREFIX:-motion-master}"
readonly OBJECT="motion-master-rpi5.img.xz"
readonly ORIGIN_URL="https://$BUCKET.s3.amazonaws.com/$PREFIX/$OBJECT"

# The distribution serves the bucket prefix at the root of its own host, so the
# object key is the whole path. The host is a variable because it becomes
# cdn.motion-master.synapticon.com once that record exists.
readonly CDN_HOST="${RT_PUBLISH_CDN_HOST:-dezliul92qqoq.cloudfront.net}"
readonly CDN_DISTRIBUTION="${RT_PUBLISH_CDN_DISTRIBUTION:-EG4E5ZA03P946}"
readonly URL="https://$CDN_HOST/$OBJECT"

# The SSH key every image authorises, published as deliberately as the image is.
# Its public half is written into each card and the root password is the account
# name, so this key is not what keeps anybody out of a board: the API it serves
# has no authentication at all. What it buys is a board that stays reachable
# when the network does not come up. build-rpi-image.sh generates it on its
# first run and keeps it out of the repository, so it comes from the home
# directory rather than from the tree.
readonly KEY="${RT_PUBLISH_KEY:-$HOME/.ssh/motion-master-rpi}"
readonly KEY_OBJECT="motion-master-rpi"
readonly KEY_URL="https://$CDN_HOST/$KEY_OBJECT"

DRY_RUN=false
ASSUME_YES=false

log() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die() {
    printf '\033[1;31merror:\033[0m %s\n' "$*" >&2
    exit 1
}

# One byte, because this asks whether an anonymous reader is served, not what
# it is served. A private object returns 403 and looks identical from here,
# where the credentials work.
requireReadable() {
    local code
    code=$(curl -sS -o /dev/null -w '%{http_code}' -r 0-0 "$1")
    [ "$code" = "206" ] || [ "$code" = "200" ] ||
        die "$1 is not publicly readable (HTTP $code)"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run) DRY_RUN=true ;;
        --yes | -y) ASSUME_YES=true ;;
        --help | -h)
            sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *) die "unknown option: $1" ;;
    esac
    shift
done

command -v xz >/dev/null || die "xz is not installed"
command -v aws >/dev/null || die "the AWS CLI is not installed"
[ -f "$IMAGE" ] || die "no image at $IMAGE — run ./build-rpi-image.sh first"
# Refused rather than skipped. A card accepts only the key it was built with, so
# an image published without that key is one nobody can reach over SSH.
[ -f "$KEY" ] || die "no SSH key at $KEY — the image authorises it, so it goes with it"

# Rebuilt only when the image is newer. Compressing 8 GB is minutes of CPU, and
# publishing the same build twice is a normal thing to do.
if [ -f "$ARCHIVE" ] && [ "$ARCHIVE" -nt "$IMAGE" ]; then
    log "reusing $ARCHIVE (newer than the image)"
else
    log "compressing (this takes a few minutes)"
    # -T0 uses every core. -9 costs time and buys little on a filesystem image,
    # where the win comes from the empty space rather than from the dictionary.
    xz -T0 -6 -c "$IMAGE" >"$ARCHIVE.partial"
    mv "$ARCHIVE.partial" "$ARCHIVE"
fi

log "checksumming"
(cd "$CACHE_DIR" && sha256sum "$(basename "$ARCHIVE")" >"$(basename "$ARCHIVE").sha256")

rawSize=$(stat -c %s "$IMAGE")
archiveSize=$(stat -c %s "$ARCHIVE")
log "image     $(numfmt --to=iec "$rawSize")"
log "archive   $(numfmt --to=iec "$archiveSize")"
log "target    s3://$BUCKET/$PREFIX/$OBJECT"
log "origin    $ORIGIN_URL"
log "url       $URL"
log "key       $KEY_URL"

if [ "$DRY_RUN" = true ]; then
    log "dry run — nothing uploaded"
    exit 0
fi

# Asked because this is not undoable in the way a local file is: the object is
# world-readable the moment it lands, the URL is the one the documentation
# hands out, and this overwrites whatever a previous run put there.
if [ "$ASSUME_YES" = false ]; then
    printf 'Upload and overwrite %s, and the SSH key beside it? [y/N] ' "$URL"
    read -r reply
    case "$reply" in
        y | Y) ;;
        *) die "cancelled" ;;
    esac
fi

log "uploading"
aws s3 cp "$ARCHIVE" "s3://$BUCKET/$PREFIX/$OBJECT" --acl public-read
aws s3 cp "$ARCHIVE.sha256" "s3://$BUCKET/$PREFIX/$OBJECT.sha256" --acl public-read
aws s3 cp "$KEY" "s3://$BUCKET/$PREFIX/$KEY_OBJECT" --acl public-read

log "checking the origin is public"
requireReadable "$ORIGIN_URL"

# One key overwritten in place leaves the edges holding the previous build for
# as long as the cache allows, and the documentation hands out the CDN URL. So
# the invalidation is waited on: this script says published when the address a
# reader is given serves what was just uploaded.
log "invalidating the CDN cache"
invalidation=$(aws cloudfront create-invalidation \
    --distribution-id "$CDN_DISTRIBUTION" \
    --paths "/$OBJECT" "/$OBJECT.sha256" "/$KEY_OBJECT" \
    --query 'Invalidation.Id' --output text)
log "waiting for invalidation $invalidation"
aws cloudfront wait invalidation-completed \
    --distribution-id "$CDN_DISTRIBUTION" --id "$invalidation"

log "checking the CDN serves it"
requireReadable "$URL"
requireReadable "$KEY_URL"

log "published"
log "  $URL"
log "  $URL.sha256"
log "  $KEY_URL"
