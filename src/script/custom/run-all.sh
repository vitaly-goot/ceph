#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ -f "${SCRIPT_DIR}/config.env" ]]; then
    set -a
    source "${SCRIPT_DIR}/config.env"
    set +a
fi
export AKCEPH_CONFIG_LOADED=1

print_flag() {
    local key="$1"
    local value="${!key:-0}"
    local state="disabled"
    if [[ "${value}" == "1" ]]; then
        state="enabled"
    fi
    echo "  - ${key}=${value} (${state})"
}

echo "Custom build config from ${SCRIPT_DIR}/config.env"
print_flag AKCEPH_ENABLE_CCACHE
print_flag AKCEPH_ENABLE_GO
print_flag AKCEPH_ENABLE_GRPC
print_flag AKCEPH_ENABLE_OPENSSL3
echo "  - AKCEPH_CCACHE_VERSION=${AKCEPH_CCACHE_VERSION:-unset}"
echo "  - AKCEPH_GOLANG_VERSION=${AKCEPH_GOLANG_VERSION:-unset}"
echo "  - AKCEPH_GRPC_VERSION=${AKCEPH_GRPC_VERSION:-unset}"
echo "  - CMAKE_CXX_STANDARD=${CMAKE_CXX_STANDARD:-unset}"

scripts=(
    "${SCRIPT_DIR}/ccache-bin.sh"
    "${SCRIPT_DIR}/golang.sh"
    "${SCRIPT_DIR}/abseil.sh"
    "${SCRIPT_DIR}/openssl3.sh"
    "${SCRIPT_DIR}/grpc.sh"
)

total="${#scripts[@]}"
index=0

for script in "${scripts[@]}"; do
    if [[ ! -f "${script}" ]]; then
        echo "Missing custom script: ${script}" >&2
        exit 1
    fi
    index=$((index + 1))
    start="${SECONDS}"
    echo "==> [${index}/${total}] $(basename "${script}")"
    bash "${script}"
    elapsed=$((SECONDS - start))
    echo "<== [${index}/${total}] $(basename "${script}") (${elapsed}s)"
done

echo "Completed ${total} custom script(s)"