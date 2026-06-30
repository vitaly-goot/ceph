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
echo "  - AKCEPH_C_COMPILER=${AKCEPH_C_COMPILER:-gcc}"
echo "  - AKCEPH_CXX_COMPILER=${AKCEPH_CXX_COMPILER:-g++}"
echo "  - AKCEPH_COMMON_FLAGS=${AKCEPH_COMMON_FLAGS:-unset}"
echo "  - AKCEPH_LINKER_FLAGS=${AKCEPH_LINKER_FLAGS:-unset}"
echo "  - AKCEPH_OPENSSL_CFLAGS=${AKCEPH_OPENSSL_CFLAGS:-unset}"
echo "  - AKCEPH_CCACHE_VERSION=${AKCEPH_CCACHE_VERSION:-unset}"
echo "  - AKCEPH_GOLANG_VERSION=${AKCEPH_GOLANG_VERSION:-unset}"
echo "  - AKCEPH_GRPC_VERSION=${AKCEPH_GRPC_VERSION:-unset}"
echo "  - CMAKE_CXX_STANDARD=${CMAKE_CXX_STANDARD:-unset}"

ensure_required_compilers() {
    local cc="${AKCEPH_C_COMPILER:-gcc}"
    local cxx="${AKCEPH_CXX_COMPILER:-g++}"

    if command -v "${cc}" > /dev/null && command -v "${cxx}" > /dev/null; then
        return
    fi

    if [[ "${cc}" =~ ^gcc-([0-9]+)$ && "${cxx}" == "g++-${BASH_REMATCH[1]}" ]] &&
       command -v apt-get > /dev/null; then
        local version="${BASH_REMATCH[1]}"

        echo "Installing requested GCC ${version} toolchain"
        export DEBIAN_FRONTEND=noninteractive
        apt-get update
        if ! apt-get install -y "gcc-${version}" "g++-${version}"; then
            local distro_id=""
            if [[ -r /etc/os-release ]]; then
                # shellcheck disable=SC1091
                source /etc/os-release
                distro_id="${ID:-}"
            fi
            if [[ "${distro_id}" != "ubuntu" ]]; then
                echo "Unable to install ${cc}/${cxx} from default repositories" >&2
                exit 1
            fi

            apt-get install -y software-properties-common ca-certificates
            add-apt-repository -y ppa:ubuntu-toolchain-r/test
            apt-get update
            apt-get install -y "gcc-${version}" "g++-${version}"
        fi
    fi

    for compiler in "${cc}" "${cxx}"; do
        if ! command -v "${compiler}" > /dev/null; then
            echo "Required compiler not found: ${compiler}" >&2
            exit 1
        fi
    done
}

if [[ "${AKCEPH_ENABLE_GRPC:-0}" == 1 || "${AKCEPH_ENABLE_OPENSSL3:-0}" == 1 ]]; then
    ensure_required_compilers
fi

scripts=(
    "${SCRIPT_DIR}/ccache-bin.sh"
    "${SCRIPT_DIR}/golang.sh"
    "${SCRIPT_DIR}/abseil.sh"
    "${SCRIPT_DIR}/openssl3.sh"
    "${SCRIPT_DIR}/grpc.sh"
    "${SCRIPT_DIR}/runtime-libstdcxx.sh"
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
