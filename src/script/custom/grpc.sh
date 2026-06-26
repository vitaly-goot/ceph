#!/bin/bash

# Fetch and build gRPC from source.
#
# Requires: libcrypto-dev, zlib1g-dev.
#
# Variables (buildarg):
# - AKCEPH_BUILD_GRPC: If 1, build gRPC.
# - CMAKE_CXX_STANDARD: C++ standard to use.
#
# Note: Compiling with CMAKE_CXX_STANDARD=17. This might need parameterised

tmpdir=$(mktemp -d "tmp.XXXXXXXXXX" -p "/tmp")
trap 'rm -rf $tmpdir' EXIT

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ "${AKCEPH_CONFIG_LOADED:-0}" != 1 ]]; then
    set -a
    source "${SCRIPT_DIR}/config.env"
    set +a
fi

INSTALL_DIR=/usr/local/grpc

if [[ $AKCEPH_ENABLE_GRPC != 1 ]]; then
    echo "AKCEPH_ENABLE_GRPC is not 1, skipping gRPC build"
    mkdir -p "$INSTALL_DIR" # So the Dockerfile COPY has something to work with.
    exit 0
fi

if [[ -z $CMAKE_CXX_STANDARD ]]; then
    echo "CMAKE_CXX_STANDARD is not set, defaulting to 17"
    CMAKE_CXX_STANDARD=17
fi

set -x
cd "$tmpdir"
git clone --recurse-submodules --shallow-submodules --depth=1 \
    -c advice.detachedHead=false \
    -b "v${AKCEPH_GRPC_VERSION}" https://github.com/grpc/grpc.git
cd grpc

# gRPC 1.59.3 prefixes the non-template CallSeqFactory member with the
# dependent-template disambiguator. GCC accepts it, but Clang 19 rejects it.
basic_seq_header=src/core/lib/promise/detail/basic_seq.h
if [[ "${AKCEPH_CXX_COMPILER:-}" == clang++-19 ]]; then
    if ! grep -q 'Traits::template CallSeqFactory' "${basic_seq_header}"; then
        echo "Clang 19 gRPC compatibility patch no longer applies" >&2
        exit 1
    fi
    sed -i 's/Traits::template CallSeqFactory/Traits::CallSeqFactory/' \
        "${basic_seq_header}"
fi

cd cmake
rm -rf build
mkdir -p build
cd build
declare -a disable_plugins; disable_plugins=()
for p in CSHARP NODE OBJECTIVE_C PHP RUBY; do disable_plugins+=("-DgRPC_BUILD_GRPC_${p}_PLUGIN=OFF"); done

COMMON_FLAGS="${AKCEPH_COMMON_FLAGS:--march=${AKCEPH_GCC_TARGET_ARCH:-x86-64}}"
LINKER_FLAGS="${AKCEPH_LINKER_FLAGS:-}"
cmake -GNinja \
    -DCMAKE_C_COMPILER="${AKCEPH_C_COMPILER:-gcc}" \
    -DCMAKE_CXX_COMPILER="${AKCEPH_CXX_COMPILER:-g++}" \
    -DCMAKE_C_FLAGS="$COMMON_FLAGS" \
    -DCMAKE_CXX_FLAGS="$COMMON_FLAGS" \
    -DCMAKE_EXE_LINKER_FLAGS="$LINKER_FLAGS" \
    -DCMAKE_SHARED_LINKER_FLAGS="$LINKER_FLAGS" \
    -DCMAKE_MODULE_LINKER_FLAGS="$LINKER_FLAGS" \
    -DgRPC_INSTALL=ON \
    -DgRPC_ABSL_PROVIDER=package \
    -DgRPC_SSL_PROVIDER=package \
    -DgRPC_ZLIB_PROVIDER=package \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_STANDARD="${CMAKE_CXX_STANDARD}" \
    -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
    -DCMAKE_INSTALL_LIBDIR="${INSTALL_DIR}/lib" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_PREFIX_PATH=/usr/local/abseil-cpp/lib/cmake/absl \
    -DBUILD_SHARED_LIBS=OFF \
    "${disable_plugins[@]}" \
    ../..
ninja install
