#!/bin/bash

# Download and install ccache.
#
# Requires: gnupg (for verifying the tarball).
# Variables:
# - CCACHE_VERSION: The version of ccache to build.

tmpdir=$(mktemp -d "tmp.XXXXXXXXXX" -p "/tmp")
trap 'rm -rf $tmpdir' EXIT

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ "${AKCEPH_CONFIG_LOADED:-0}" != 1 ]]; then
    set -a
    source "${SCRIPT_DIR}/config.env"
    set +a
fi

if [[ $AKCEPH_ENABLE_CCACHE != 1 ]]; then
    echo "AKCEPH_ENABLE_CCACHE is not 1, skipping ccache build"
    exit 0
fi

if [[ -z ${AKCEPH_CCACHE_VERSION:-} ]]; then
    AKCEPH_CCACHE_VERSION=4.9.1
fi

echo "Fetching ccache ${AKCEPH_CCACHE_VERSION}"
TARBALL="ccache-${AKCEPH_CCACHE_VERSION}-linux-x86_64.tar.xz"
URL="https://github.com/ccache/ccache/releases/download/v4.9.1/${TARBALL}"
DL="${tmpdir}/${TARBALL}"
curl -L -o "$DL" "${URL}"
curl -L -o "${DL}.asc" "${URL}.asc"
gpg --keyserver hkps://keyserver.ubuntu.com --recv-keys "$AKCEPH_CCACHE_GPGKEYID"
gpg --verify "${DL}.asc" "$DL"

tar -C "$tmpdir" -xJf "$DL"
install -m 0755 -o root -g root "$tmpdir/ccache-${AKCEPH_CCACHE_VERSION}-linux-x86_64/ccache" /usr/local/bin/
/usr/local/bin/ccache --version
