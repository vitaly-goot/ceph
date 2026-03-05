#!/bin/bash

# Fetch and build OpenSSL 3 from source.

tmpdir=$(mktemp -d "tmp.XXXXXXXXXX" -p "/tmp")
trap 'rm -rf $tmpdir' EXIT

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ "${AKCEPH_CONFIG_LOADED:-0}" != 1 ]]; then
    set -a
    source "${SCRIPT_DIR}/config.env"
    set +a
fi

INSTALL_DIR=/usr/local/openssl3

if [[ $AKCEPH_ENABLE_OPENSSL3 != 1 ]]; then
    echo "AKCEPH_ENABLE_OPENSSL3 is not 1, skipping OpenSSL 3.x build"
    mkdir -p "$INSTALL_DIR" # So the Dockerfile COPY has something to work with.
    exit 0
fi

set -x
cd "$tmpdir"
git clone git://git.openssl.org/openssl.git
cd openssl
git checkout -b openssl-3.2.1 tags/openssl-3.2.1
env CC="gcc" "CFLAGS=-march=$AKCEPH_GCC_TARGET_ARCH" \
    ./Configure --prefix="$INSTALL_DIR" --openssldir="$INSTALL_DIR" --libdir="$INSTALL_DIR/lib"
make -j"$(( $(nproc)/2 ))"
# install_sw doesn't build manpages.
make install_sw
