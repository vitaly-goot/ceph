#!/usr/bin/env bash
# Focused developer build using an existing, compatible Ceph dependency build.
# No existing sources/build artifacts are modified. Prefer the normal CMake
# unittest_ec_write_journal target when a full build is configured.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
: "${CEPH_DEPS_SOURCE:?Set to the Ceph checkout containing fmt and googletest}"
: "${CEPH_DEPS_BUILD:?Set to its compatible configured/built Ceph directory}"
build=${EC_JOURNAL_TEST_BUILD:-"$root/build-journal-poc"}
cxx=${CXX:-g++}

for input in \
  "$CEPH_DEPS_BUILD/include/acconfig.h" \
  "$CEPH_DEPS_BUILD/src/include/ceph_release.h" \
  "$CEPH_DEPS_BUILD/lib/libceph-common.so" \
  "$CEPH_DEPS_SOURCE/src/fmt/include/fmt/format.h" \
  "$CEPH_DEPS_SOURCE/src/googletest/googletest/src/gtest-all.cc"; do
  if [[ ! -f "$input" ]]; then
    printf 'Missing build dependency: %s\n' "$input" >&2
    exit 1
  fi
done

mkdir -p "$build"
flags=(-std=c++20 -g -O1 -Wall -Wextra -Wno-unused-parameter
       -Wno-unknown-pragmas -Wno-ignored-qualifiers -pthread
       -D_GNU_SOURCE -DHAVE_CONFIG_H)
if [[ ${EC_JOURNAL_SANITIZE:-0} == 1 ]]; then
  flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
if [[ -n ${BOOST_ROOT:-} ]]; then
  flags+=(-I"$BOOST_ROOT/include")
fi

# Build gtest from source rather than reusing archives built by another GCC:
# GCC LTO bytecode is not compatible across major compiler versions.
"$cxx" "${flags[@]}" \
  -I"$root/src" -I"$root/src/include" \
  -I"$CEPH_DEPS_BUILD/src/include" -I"$CEPH_DEPS_BUILD/include" \
  -I"$CEPH_DEPS_SOURCE/src/fmt/include" \
  -I"$CEPH_DEPS_SOURCE/src/googletest/googletest/include" \
  -I"$CEPH_DEPS_SOURCE/src/googletest/googletest" \
  "$root/src/osd/ECWriteJournal.cc" \
  "$root/src/test/osd/test_ec_write_journal.cc" \
  "$CEPH_DEPS_SOURCE/src/googletest/googletest/src/gtest-all.cc" \
  "$CEPH_DEPS_SOURCE/src/googletest/googletest/src/gtest_main.cc" \
  -L"$CEPH_DEPS_BUILD/lib" -Wl,-rpath,"$CEPH_DEPS_BUILD/lib" \
  -lceph-common -o "$build/unittest_ec_write_journal"

"$build/unittest_ec_write_journal" "$@"