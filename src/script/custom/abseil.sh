#!/bin/bash

tmpdir=$(mktemp -d "tmp.XXXXXXXXXX" -p "/tmp")
trap 'rm -rf $tmpdir' EXIT

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ "${AKCEPH_CONFIG_LOADED:-0}" != 1 ]]; then
    set -a
    source "${SCRIPT_DIR}/config.env"
    set +a
fi

INSTALL_DIR=/usr/local/abseil-cpp


# Bypasses disabling 
if [[ $AKCEPH_ENABLE_GRPC != 1 ]]; then
    echo "AKCEPH_ENABLE_GRPC is not 1, skipping abseil-cpp build"
    mkdir -p "$INSTALL_DIR" # So the Dockerfile COPY has something to work with.
    exit 0
fi

if [[ -z $ABSEIL_VERSION ]]; then
    ABSEIL_VERSION=20240116.0
fi

if [[ -z $CMAKE_CXX_STANDARD ]]; then
    echo "CMAKE_CXX_STANDARD is not set, defaulting to 17"
    CMAKE_CXX_STANDARD=17
fi

set -x
cd "$tmpdir"
git clone https://github.com/abseil/abseil-cpp.git
cd abseil-cpp
git checkout -b "$ABSEIL_VERSION" "tags/$ABSEIL_VERSION"
mkdir -p build
cd build
COMMON_FLAGS="${AKCEPH_COMMON_FLAGS:--march=${AKCEPH_GCC_TARGET_ARCH:-x86-64}}"
LINKER_FLAGS="${AKCEPH_LINKER_FLAGS:-}"
cmake \
    -DCMAKE_C_COMPILER="${AKCEPH_C_COMPILER:-gcc}" \
    -DCMAKE_CXX_COMPILER="${AKCEPH_CXX_COMPILER:-g++}" \
    -DCMAKE_C_FLAGS="$COMMON_FLAGS" \
    -DCMAKE_CXX_FLAGS="$COMMON_FLAGS" \
    -DCMAKE_EXE_LINKER_FLAGS="$LINKER_FLAGS" \
    -DCMAKE_SHARED_LINKER_FLAGS="$LINKER_FLAGS" \
    -DCMAKE_MODULE_LINKER_FLAGS="$LINKER_FLAGS" \
    -DABSL_ENABLE_INSTALL=ON \
    -DCMAKE_INSTALL_PREFIX=${INSTALL_DIR} \
    -DCMAKE_INSTALL_LIBDIR=${INSTALL_DIR}/lib \
    -DCMAKE_CXX_STANDARD="${CMAKE_CXX_STANDARD}" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -GNinja \
    ..
ninja install
