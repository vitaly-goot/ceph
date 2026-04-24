#!/bin/bash

# Fetch and build ccache from source.
#
# Requires: cmake, ninja.
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

set -x
cd "$tmpdir"
git clone https://github.com/ccache/ccache.git
cd ccache
git checkout -b "v${AKCEPH_CCACHE_VERSION}" tags/"v${AKCEPH_CCACHE_VERSION}"
mkdir build
cd build
env CXXFLAGS="-march=$AKCEPH_GCC_TARGET_ARCH" \
    cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_TESTING=OFF \
    -DENABLE_DOCUMENTATION=OFF \
    -GNinja \
    ..
ninja install
