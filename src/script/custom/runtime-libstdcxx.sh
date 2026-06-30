#!/usr/bin/env bash

# Repackage the ubuntu-toolchain-r/test PPA's libstdc++6/libgcc-s1 into a
# standalone .deb so GCC-13-built Ceph/Crimson binaries find the GLIBCXX
# symbol versions they need on targets whose system libstdc++6 predates
# GCC 13.
#
# The PPA build is used rather than e.g. Debian trixie's libstdc++6: trixie's
# build requires a newer glibc (GLIBC_2.36/2.38) than this distro ships, so it
# trades the GLIBCXX mismatch for a GLIBC one. The PPA's libstdc++6 is built
# for this distro's own glibc (same PPA already used to install
# AKCEPH_C_COMPILER/AKCEPH_CXX_COMPILER), so it satisfies the GLIBCXX
# requirement without raising the glibc floor.
#
# Runs at image-build time (as root, via run-all.sh) rather than at
# make-debs.sh's package-build time, because apt-get needs root and an
# up-to-date package index that the package-build container does not have.
# make-debs.sh just picks up the fixed-path output below with `reprepro
# includedeb`.
#
# Variables (config.env):
# - AKCEPH_BUNDLE_RUNTIME_LIBSTDCXX: if 1, build the package (default 1).
#
# Output: OUTPUT_DEB (fixed path, consumed by make-debs.sh).

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ "${AKCEPH_CONFIG_LOADED:-0}" != 1 ]]; then
    set -a
    source "${SCRIPT_DIR}/config.env"
    set +a
fi

OUTPUT_DIR=/usr/local/akceph-runtime
OUTPUT_DEB="${OUTPUT_DIR}/akceph-runtime-libstdcxx6.deb"
mkdir -p "${OUTPUT_DIR}"

if [[ "${AKCEPH_BUNDLE_RUNTIME_LIBSTDCXX:-1}" != 1 ]]; then
    echo "AKCEPH_BUNDLE_RUNTIME_LIBSTDCXX is not 1, skipping runtime libstdc++ bundling"
    exit 0
fi

if ! command -v apt-get > /dev/null; then
    echo "apt-get not found, skipping runtime libstdc++ bundling" >&2
    exit 0
fi

# shellcheck disable=SC1091
. /etc/os-release
if [[ "${ID:-}" != ubuntu ]]; then
    echo "Not an Ubuntu base image (ID=${ID:-unknown}), skipping runtime libstdc++ bundling"
    exit 0
fi

set -x

if ! grep -rq "ubuntu-toolchain-r/test" /etc/apt/sources.list.d/ 2>/dev/null; then
    apt-get update
    apt-get install -y software-properties-common ca-certificates
    add-apt-repository -y ppa:ubuntu-toolchain-r/test
fi
apt-get update

tmpdir=$(mktemp -d "tmp.XXXXXXXXXX" -p "/tmp")
trap 'rm -rf "$tmpdir"' EXIT
cd "$tmpdir"

apt-get download libstdc++6 libgcc-s1

stdcxx_deb=$(ls libstdc++6_*.deb)
gccs_deb=$(ls libgcc-s1_*.deb)

mkdir -p stage/DEBIAN
dpkg-deb -x "$stdcxx_deb" stage
dpkg-deb -x "$gccs_deb" stage

stdcxx_version=$(dpkg-deb -f "$stdcxx_deb" Version)
gccs_version=$(dpkg-deb -f "$gccs_deb" Version)
depends=$(dpkg-deb -f "$stdcxx_deb" Depends)

cat > stage/DEBIAN/control <<EOF
Package: akceph-runtime-libstdcxx6
Version: ${stdcxx_version}
Architecture: amd64
Maintainer: Akamai Ceph Build <noreply@akamai.com>
Section: libs
Priority: optional
Depends: ${depends}
Provides: libstdc++6 (= ${stdcxx_version}), libgcc-s1 (= ${gccs_version})
Replaces: libstdc++6, libgcc-s1
Description: Bundled libstdc++6/libgcc-s1 matching the GCC 13 build toolchain
 Repackaged from ppa:ubuntu-toolchain-r/test (the same PPA used to build
 GCC 13 for this image) so GCC-13-built Ceph/Crimson binaries find the
 GLIBCXX symbol versions they need on targets whose system libstdc++6
 predates GCC 13. Built for this distro's own glibc, unlike a donor library
 pulled from a newer distro release.
EOF

cat > stage/DEBIAN/postinst <<'EOF'
#!/bin/sh
set -e
ldconfig
EOF
chmod 755 stage/DEBIAN/postinst

dpkg-deb --build --root-owner-group stage "${OUTPUT_DEB}"
echo "Built ${OUTPUT_DEB}"
