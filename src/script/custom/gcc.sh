#!/bin/bash

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ "${AKCEPH_CONFIG_LOADED:-0}" != 1 ]]; then
    set -a
    source "${SCRIPT_DIR}/config.env"
    set +a
fi

if [[ ${AKCEPH_ENABLE_GCC11:-0} != 1 ]]; then
    echo "AKCEPH_ENABLE_GCC11 is not 1, skipping GCC 11 install"
    exit 0
fi

if [[ ! -f /etc/os-release ]]; then
    echo "Cannot detect OS, skipping GCC 11 install"
    exit 0
fi

source /etc/os-release

case "${ID}:${VERSION_ID}" in
    ubuntu:20.04)
        echo "Installing GCC 11 on Ubuntu 20.04"
        apt-get update -qq
        apt-get install -y -qq software-properties-common
        add-apt-repository -y ppa:ubuntu-toolchain-r/test
        apt-get update -qq
        apt-get install -y -qq gcc-11 g++-11
        update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-11 110 \
            --slave /usr/bin/g++ g++ /usr/bin/g++-11
        if [[ -x /usr/bin/gcc-10 ]]; then
            update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-10 100 \
                --slave /usr/bin/g++ g++ /usr/bin/g++-10
        fi
        update-alternatives --set gcc /usr/bin/gcc-11
        export AKCEPH_CC=gcc-11
        export AKCEPH_CXX=g++-11
        ;;
    debian:13)
        echo "Using GCC 14 on Debian 13"
        export AKCEPH_CC=gcc-14
        export AKCEPH_CXX=g++-14
        ;;
    *)
        echo "GCC install not needed on ${ID} ${VERSION_ID}, using system default"
        export AKCEPH_CC=gcc
        export AKCEPH_CXX=g++
        ;;
esac

echo "GCC version now: $(${AKCEPH_CC} --version | head -1)"
echo "G++ version now: $(${AKCEPH_CXX} --version | head -1)"