# Ceph Copilot Instructions

## Repository Overview

Ceph is a distributed storage system (version 18.2.x). The codebase is primarily C++ (~16K files), with significant Python (~1.3K files) and Go (~420 files). It is built with CMake + Ninja and uses CTest for testing.

Major subsystems, each in `src/{name}/`:
- **OSD** – Object Storage Daemon (core storage backend)
- **MON** – Monitor (cluster state/map management)
- **MDS** – Metadata Server (CephFS filesystem metadata)
- **RGW** – RADOS Gateway (S3/Swift object API; large codebase with drivers in `src/rgw/`)
- **MGR** – Manager (performance metrics, orchestration modules)
- **LIBRADOS** / **LIBRBD** – Client libraries
- **CephFS client** – `src/client/`
- **CephADM** – Python-based deployment daemon, `src/cephadm/`

Supporting libraries: `src/common/`, `src/auth/` (CephX), `src/msg/` (networking), `src/os/` (BlueStore/FileStore), `src/crush/`, `src/cls/` (object class plugins), `src/kv/` (KV store abstraction over RocksDB).

## Build

### Prerequisites

```bash
git submodule update --init --recursive
./install-deps.sh
```

### Configure and build

```bash
./do_cmake.sh [-DCMAKE_BUILD_TYPE=RelWithDebInfo]
cd build
ninja -j3          # ~2.5 GiB RAM per job; default Debug build is slow
```

Default build type is `Debug`. Use `RelWithDebInfo` for performance-sensitive work.

Useful CMake flags:
- `-DWITH_RADOSGW=OFF` — skip RGW to speed up builds
- `-DWITH_CCACHE=ON` — enable ccache (auto-detected if available)
- `-DCMAKE_INSTALL_PREFIX=/opt/ceph`

### Custom build environment (`src/script/custom/`)

Used by CI to set up dependencies before CMake. Run once per environment:

```bash
./src/script/custom/run-all.sh
```

Controlled by `src/script/custom/config.env`. Key flags:
- `AKCEPH_ENABLE_GCC11=1` — installs GCC 11 on Ubuntu 20.04 via PPA
- `AKCEPH_ENABLE_GRPC=1` — builds gRPC 1.59.3 + Abseil from source → `/usr/local/grpc`
- `AKCEPH_ENABLE_CCACHE=1` — installs pre-built ccache 4.9.1
- `AKCEPH_ENABLE_GO=1` — installs Go 1.23.4 + `buf` v1.29.0
- `AKCEPH_ENABLE_OPENSSL3=0` — builds OpenSSL 3.2.1 (disabled by default)
- `CMAKE_CXX_STANDARD=20`

## Testing

### Run all tests

```bash
cd build
ninja check              # builds test deps then runs via ctest
ctest -j$(nproc)         # parallel
```

### Run a single test or subset

```bash
cd build
ctest -R <regex> -V      # e.g., ctest -R unittest_osd -V
```

Test binary names follow two patterns:
- `unittest_*` — GoogleTest binaries; registered with ctest
- `ceph_test_*` — run manually, not via ctest

### Python tests

```bash
cd src/test/pybind
pytest                   # uses pytest.ini in that directory
```

### Run a script-based test (mypy / tox)

```bash
./src/script/run_mypy.sh
./src/script/run_tox.sh
```

### Dev cluster (vstart)

```bash
cd build
ninja vstart
../src/vstart.sh --debug --new -x --localhost --bluestore
./bin/ceph -s       # cluster status
../src/stop.sh
```

## Code Style

### C++ (all core components)

- Google C++ Style Guide with Ceph modifications.
- **Naming**: `snake_case` functions/variables, `CamelCase` classes/structs, `UPPER_CASE` constants.
- **Member prefix**: `m_` prefix for class members (or none — be consistent within a class).
- **Struct types**: `my_type_t` (lowercase with `_t` suffix).
- **Indent**: 2 spaces; tabs represent 8 spaces (follow existing file style).
- Every `.cc`/`.h` file should start with:
  ```cpp
  // -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
  // vim: ts=8 sw=2 smarttab ft=cpp
  ```

### Python

PEP-8. Python bindings live in `src/pybind/`; shared utilities in `src/python-common/`.

### Go

Standard `gofmt` style. Code lives primarily in `src/ceph-node-proxy/` and build tooling.

## Contributing

- Every commit **must** have a `Signed-off-by` line (`git commit -s`).
- Whitespace-only changes are discouraged (causes rebase/backport pain).
- Performance improvements require benchmark data in the PR.
- See `SubmittingPatches.rst` for the full patch process.

## CI

Workflows in `.github/workflows/` target `aka_version_*` branches and run on a custom self-hosted `ceph` runner. The `build-binaries.yaml` and `build-debians.yaml` workflows call `run-all.sh` to set up the environment, then invoke a composite action from `StorageTeam/build-ceph-action`. CI uses Redis-backed remote ccache (configured in `src/script/custom/ccache.config`).
