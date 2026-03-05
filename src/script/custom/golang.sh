#!/bin/bash

# Install golang and useful tools for building Ceph.

tmpdir=$(mktemp -d "tmp.XXXXXXXXXX" -p "/tmp")
trap 'rm -rf $tmpdir' EXIT

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ "${AKCEPH_CONFIG_LOADED:-0}" != 1 ]]; then
    set -a
    source "${SCRIPT_DIR}/config.env"
    set +a
fi

if [[ $AKCEPH_ENABLE_GO != 1 ]]; then
    echo "AKCEPH_ENABLE_GO is not 1, skipping golang build"
    exit 0
fi

echo "Fetching golang ${AKCEPH_GOLANG_VERSION}"
TARBALL="go${AKCEPH_GOLANG_VERSION}.linux-amd64.tar.gz"
URL="https://go.dev/dl/${TARBALL}"
DL="${tmpdir}/go.tar.gz"
curl -L -o "$DL" "${URL}"
sum="$(sha256sum "$DL" | cut -d' ' -f1)"
if [[ $sum != "$AKCEPH_GOLANG_CHECKSUM" ]]; then
    echo "Checksum mismatch for golang tarball"
    exit 1
fi

# The tarball unpacks to prefix 'go/', so we can just un-tar it.
rm -rf /usr/local/go
echo "Unpacking to /usr/local/go"
tar -C /usr/local -xzf "$DL"

# Quick check.
gobin=/usr/local/go/bin/go
$gobin version

# Install tools.
export GOBIN=/usr/local/bin
mkdir -p "$GOBIN"

# Most tools are going to be 'go install' one-liners.
$gobin install github.com/bufbuild/buf/cmd/buf@v1.29.0

if ! command -v buf >/dev/null 2>&1; then
    echo "buf was installed but not found on PATH" >&2
    exit 1
fi

buf --version
