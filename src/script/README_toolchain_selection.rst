Ubuntu 22 toolchain selection for OSD and Crimson
=================================================

This document describes the container-build setup used to build the classic
``ceph-osd`` target and the Crimson ``crimson-osd`` target on Ubuntu 22.04.

The current downstream configs use GCC 13 for both targets:

* ``crimson-osd`` uses GCC 13 with SPDK disabled;
* classic ``ceph-osd`` uses GCC 13 with SPDK enabled.

The short version is:

* build Ceph, Crimson, Abseil, gRPC, SPDK, and DPDK with GCC 13 downstream;
* avoid forcing LLD in the GCC 13 configs;
* keep the Clang 19 + LLD ``librados.so`` fallback documented for upstream
  comparison and optional Clang experiments;
* keep one set of env knobs for ``-e build`` and ``-e debs``:
  ``WITH_CRIMSON``, ``WITH_SPDK``, ``CC``, ``CXX``, and
  ``CMAKE_INTERPROCEDURAL_OPTIMIZATION``;
* keep ``RelWithDebInfo`` as the default build type;
* keep IPO/LTO configurable, but enable it for the validated OSD and Crimson
  configs;
* keep SPDK enabled for classic OSD builds and disabled for Crimson builds.

The config files are:

* ``src/script/ccache.crimson.config`` for ``crimson-osd``;
* ``src/script/ccache.osd.config`` for classic ``ceph-osd``.
* ``src/script/custom/config.env`` for the custom Abseil/gRPC/OpenSSL
  dependency image.


Why this exists
---------------

The original Ubuntu container path mixed compiler assumptions:

* Ceph/Crimson could be configured with Clang.
* Abseil and gRPC were hardcoded to ``gcc-11``/``g++-11``.
* SPDK inherited its own compiler defaults instead of the selected CMake
  compiler.
* ``run-make.sh`` forced SPDK and IPO on, making them hard to vary from the
  environment.
* Debian package builds used ``debian/rules`` and did not naturally consume the
  same ``ARGS`` line used by the normal ``-e build`` path.

That was fragile for Ubuntu 24.04, where ``g++-11`` is not available by
default, and it also created unnecessary mixed-toolchain risk on Ubuntu 22.04.
It is also not sufficient for this downstream Crimson build because Crimson
requires a GCC 13-capable toolchain.

The current approach makes the selected build knobs explicit and shared:

* ``src/script/ccache.crimson.config`` selects GCC 13, disables SPDK, enables
  IPO, and pins ``RelWithDebInfo`` for Crimson;
* ``src/script/ccache.osd.config`` selects GCC 13, keeps SPDK enabled, enables
  IPO, and pins ``RelWithDebInfo`` for classic OSD;
* ``src/script/custom/config.env`` selects the compiler and flags used for
  custom Abseil/gRPC/OpenSSL dependency builds;
* ``debian/rules`` derives Debian package CMake arguments from the same
  ``WITH_CRIMSON``, ``WITH_SPDK``, ``CC``, ``CXX``, and
  ``CMAKE_INTERPROCEDURAL_OPTIMIZATION`` environment variables.

The GCC 13 downstream configs do not force LLD.  They use GCC's normal linker
path and GCC LTO flags.  The ``librados.so`` GNU ``ld.bfd`` fallback remains
useful for optional Clang 19 + LLD experiments because ``librados`` has legacy
symbol-versioning requirements that are accepted by GNU ``ld.bfd`` and rejected
by LLD.


Current config matrix
---------------------

.. list-table::
   :header-rows: 1

   * - Target
     - Env file
     - Toolchain
     - Linker
     - SPDK
     - IPO/LTO
     - Build type
   * - ``crimson-osd``
     - ``src/script/ccache.crimson.config``
     - GCC 13
     - GCC default linker, normally GNU ``ld.bfd``
     - Off
     - On
     - ``RelWithDebInfo``
   * - ``ceph-osd``
     - ``src/script/ccache.osd.config``
     - GCC 13
     - GCC default linker, normally GNU ``ld.bfd``
     - On
     - On
     - ``RelWithDebInfo``

Example Crimson build:

.. code-block:: console

   BUILDKIT_PROGRESS=plain python3 src/script/build-with-container.py \
     -d ubuntu22.04 \
     -b build-crimson-gcc13 \
     --ccache-dir /ceph/.ccache/ \
     --custom-image-script run-all.sh \
     --env-file src/script/ccache.crimson.config \
     --image-sources build \
     -e build 2>&1 | tee /tmp/crimson-u22-gcc13-image.log

The log filename is intentionally ``u22`` here because the command selects
``-d ubuntu22.04``.  A filename such as ``crimson-u24-image.log`` would not
change the build target; it would only make the saved log harder to interpret.

Example Crimson Debian package build:

.. code-block:: console

   BUILDKIT_PROGRESS=plain python3 src/script/build-with-container.py \
     -d ubuntu22.04 \
     -b build-crimson-gcc13-debs \
     --ccache-dir /ceph/.ccache/ \
     --custom-image-script run-all.sh \
     --env-file src/script/ccache.crimson.config \
     --image-sources build \
     -e debs 2>&1 | tee /tmp/crimson-u22-gcc13-debs.log

Example classic OSD build:

.. code-block:: console

   BUILDKIT_PROGRESS=plain python3 src/script/build-with-container.py \
     -d ubuntu22.04 \
     -b build-osd-gcc13 \
     --ccache-dir /ceph/.ccache/ \
     --custom-image-script run-all.sh \
     --env-file src/script/ccache.osd.config \
     --image-sources build \
     -e build 2>&1 | tee /tmp/osd-u22-gcc13-image.log


One config for ``-e build`` and ``-e debs``
-------------------------------------------

``build-with-container.py`` has many internal steps, but the two relevant
end-user build products here are:

* ``-e build`` for a normal CMake/Ninja build;
* ``-e debs`` for Debian/Ubuntu packages.

The two paths reach CMake differently:

* ``-e build`` goes through ``run-make.sh`` and ``do_cmake.sh``.  It consumes
  ``WITH_CRIMSON``, ``WITH_SPDK``, ``CC``, ``CXX``,
  ``CMAKE_INTERPROCEDURAL_OPTIMIZATION``, and the env-file ``ARGS`` value.
* ``-e debs`` goes through ``make-debs.sh``, ``dpkg-buildpackage``, and
  ``debian/rules``.  It does not consume ``ARGS`` directly.

To keep those paths aligned, ``debian/rules`` now derives the package CMake
arguments from the same environment variables:

.. list-table::
   :header-rows: 1

   * - Env variable
     - CMake argument added by ``debian/rules``
   * - ``WITH_CRIMSON=true``
     - ``-DWITH_CRIMSON=ON`` and ``CEPH_OSD_BASENAME=crimson-osd``
   * - ``WITH_SPDK=ON`` or ``WITH_SPDK=OFF``
     - ``-DWITH_SPDK=ON`` or ``-DWITH_SPDK=OFF``
   * - ``CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON``
     - ``-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON``
   * - ``CC=gcc-13``
     - ``-DCMAKE_C_COMPILER=gcc-13``
   * - ``CXX=g++-13``
     - ``-DCMAKE_CXX_COMPILER=g++-13``

``CEPH_EXTRA_CMAKE_ARGS`` is still supported for ad hoc extra package-build
arguments, but the common Crimson/SPDK/toolchain/IPO choices should come from
the env file instead of being duplicated there.

On Ubuntu/Debian, ``-e packages`` routes to ``-e debs``.  On RPM-based distros,
it routes to the RPM path.


Before and after
----------------

.. list-table::
   :header-rows: 1

   * - Area
     - Before
     - After
   * - Main Ceph compiler
     - Configurable, but some scripts rediscovered or overrode compilers.
     - Explicit ``CC``/``CXX`` are honored by both the normal build path and
       the Debian package path.
   * - Abseil/gRPC
     - Forced to ``gcc-11``/``g++-11``, which also broke
       ``WITH_CRIMSON=ON`` with our downstream Crimson version because it
       requires GCC 13.
     - Built with the custom dependency compiler from
       ``src/script/custom/config.env``; the downstream default is GCC 13.
   * - SPDK/DPDK
     - SPDK configure/make could use defaults different from Ceph.
     - SPDK receives ``CC``/``CXX`` from CMake for OSD builds; Crimson disables
       SPDK through the shared ``WITH_SPDK=OFF`` env knob.
   * - SPDK tests
     - SPDK test binaries were part of the external SPDK build.
     - SPDK functional tests are disabled; Ceph consumes the SPDK libraries,
       not those test binaries.
   * - IPO/LTO
     - Forced on by ``run-make.sh``.
     - Controlled by ``CMAKE_INTERPROCEDURAL_OPTIMIZATION`` from the env file
       for both ``-e build`` and ``-e debs``.
   * - Linker
     - Upstream Clang builds use the default system linker unless callers pass
       linker flags.  Downstream env files forced LLD globally, exposing
       ``librados`` symbol-versioning incompatibilities.
     - Downstream GCC 13 configs do not force LLD.  Optional Clang/LLD builds
       keep the ``librados.so`` ``-fuse-ld=bfd`` fallback.
   * - Build type
     - Could be accidentally changed by caller arguments.
     - Env files and ``debian/rules`` pin ``RelWithDebInfo``.
   * - Debian packages
     - Package builds required separate ``CEPH_EXTRA_CMAKE_ARGS`` duplication
       for common Crimson/toolchain settings.
     - ``debian/rules`` derives Crimson, SPDK, IPO, and compiler settings from
       the same env variables used by ``-e build``.
   * - Crimson/AlienStore link
     - Hit a ``PluginRegistry`` symbol collision when linking ``crimson-osd``.
     - Crimson uses a separate implementation name while preserving the public
       ``PluginRegistry`` alias.


Toolchain choice
----------------

Use GCC 13 for the downstream Ubuntu 22.04 OSD and Crimson configs.

This downstream Crimson path cannot use GCC 11, and moving classic OSD to GCC
13 gives the downstream build matrix one native GCC/LTO baseline.  GCC 13 also
aligns with the upstream RPM ``gcc-c++ >= 13.3`` LTO requirement.  The OSD and
Crimson configs therefore use ``CC=gcc-13`` and ``CXX=g++-13`` and remove
Clang-only LLD flags.

Upstream still has a separate Clang story for Ubuntu/Debian-style CI builds:
``src/script/lib-build.sh`` says Ubuntu/Debian CI builds prefer Clang, and
``src/script/run-make.sh`` installs/uses Clang 19 in Jenkins.  Upstream's
Debian packaging still has a broad ``g++ (>= 11)`` build dependency, while the
RPM spec requires ``gcc-c++ >= 13.3`` for LTO-related GCC fixes.

So the practical split is:

* upstream Ubuntu/Debian CI-style builds: Clang 19 is the preferred helper
  path;
* downstream Ubuntu 22 OSD/Crimson container builds: GCC 13 is the selected
  path;
* Debian package builds: GCC remains accepted by packaging metadata.
* RPM/LTO GCC builds: GCC 13.3 or newer is required.

Keeping the compiler choice explicit avoids a class of hard-to-debug
mixed-toolchain failures: ABI mismatch risk, linker-plugin differences, LTO
object incompatibility, and different warning/diagnostic behavior between
dependencies and the final Ceph link.

The custom dependency image is controlled by ``src/script/custom/config.env``.
It defaults to GCC 13 and GCC LTO flags:

.. code-block:: sh

   AKCEPH_C_COMPILER=gcc-13
   AKCEPH_CXX_COMPILER=g++-13
   AKCEPH_COMMON_FLAGS="-march=x86-64 -flto=auto -ffat-lto-objects"
   AKCEPH_LINKER_FLAGS=
   AKCEPH_OPENSSL_CFLAGS="-march=x86-64"

``AKCEPH_COMMON_FLAGS`` applies to the CMake-based custom dependencies such as
Abseil and gRPC.  The optional OpenSSL custom build intentionally has separate
``AKCEPH_OPENSSL_CFLAGS`` and does not inherit LTO by default, because OpenSSL
has historically been finicky with LTO.

These defaults are GCC-style.  ``-flto=auto`` and ``-ffat-lto-objects`` are GCC
LTO spellings that Clang does not accept.  A Clang 19 dependency build must
therefore override both the compiler and the flags, for example:

.. code-block:: sh

   AKCEPH_C_COMPILER=clang-19
   AKCEPH_CXX_COMPILER=clang++-19
   AKCEPH_COMMON_FLAGS="-march=x86-64 -flto=thin"
   AKCEPH_LINKER_FLAGS="-fuse-ld=lld"

``src/script/custom/grpc.sh`` already special-cases ``clang++-19`` to apply a
gRPC 1.59.3 source patch, so the custom dependency scripts support a Clang 19
build once these flags are overridden.


Clang/LLD linker note
---------------------

The downstream GCC 13 configs do not force LLD.  This section documents the
Clang 19 + LLD behavior because upstream prefers Clang for Ubuntu/Debian helper
builds and because the tree still contains a ``librados.so`` fallback for
global LLD experiments.

This is the key upstream/downstream distinction:

* upstream supports Clang by selecting ``clang``/``clang++`` in the
  Ubuntu/Debian CI helper scripts;
* upstream does not force ``-fuse-ld=lld`` for Ceph targets;
* a Clang experiment may force LLD globally through ``CMAKE_EXE_LINKER_FLAGS``,
  ``CMAKE_SHARED_LINKER_FLAGS``, and ``CMAKE_MODULE_LINKER_FLAGS``.

Most targets link correctly with Clang 19 + LLD.  ``librados`` is the special
case because its C ABI compatibility depends on symbol-versioning patterns
that are accepted by GNU ``ld.bfd`` but rejected by LLD.  In particular,
``src/librados/librados_c.cc`` emits empty-version aliases such as
``foo@@``/``foo@`` to preserve legacy unversioned C API names, and
``src/librados/librados.map`` contains explicit symbol assignments for a few
implementation details.

With LLD, the failure looks like:

.. code-block:: text

   ld.lld: error: symbol rados_service_register@@ has undefined version
   ld.lld: error: version script assignment ... failed: symbol not defined

The fix is target-local: keep Clang 19 and IPO for the objects, keep LLD for
the broader build, but append ``-fuse-ld=bfd`` to the ``librados`` link when
the global shared-linker flags select LLD.  The trailing target-local flag wins
for that link only.

That gives us both supported modes in practice:

* Clang 19 + GNU ``ld.bfd`` for the ``librados`` ABI-sensitive shared library;
* Clang 19 + LLD for the rest of the Clang container build.

This applies to any build that links ``librados`` while global shared-linker
flags select LLD.  For the current GCC 13 downstream configs, this fallback is
transparent because the configs do not force LLD in the first place.


LLD vs GNU ld.bfd
~~~~~~~~~~~~~~~~~

There are two supported Clang 19 linker models:

.. list-table::
   :header-rows: 1

   * - Model
     - Pros
     - Cons
   * - Clang 19 + GNU ``ld.bfd`` everywhere
     - Closest to upstream Ubuntu/Debian Clang CI behavior.  ``librados``
       symbol versioning works naturally.  Simpler to reason about because the
       whole build uses one linker.
     - Usually slower than LLD for large links.  Less aligned with the LLVM
       ThinLTO fast path.  Does not validate LLD-specific link behavior.
   * - Clang 19 + LLD broadly, GNU ``ld.bfd`` for ``librados.so``
     - Faster links for most targets.  Pairs well with LLVM ThinLTO.  Validates
       the modern Clang+LLD path while preserving ``librados`` ABI behavior.
     - More downstream-specific than upstream's helper path.  Uses two linkers,
       so failures require checking whether a target used LLD or ``ld.bfd``.
   * - Clang 19 + LLD everywhere
     - Cleanest all-LLVM model in theory.
     - Not currently valid for this tree because LLD rejects ``librados`` legacy
       symbol-versioning constructs.

If the goal is minimum surprise and maximum alignment with upstream's
Ubuntu/Debian helper scripts, use Clang 19 with the default linker, which is
typically GNU ``ld.bfd`` on Ubuntu.  The ``librados`` target-local fallback is
transparent in that mode because it only activates when the global shared
linker flags contain ``-fuse-ld=lld``.

If the goal is faster LLVM-style links while keeping the build green, use LLD
globally and keep the ``librados.so`` ``ld.bfd`` fallback.


Why not GCC 11
--------------

GCC 11 is not a good default for these builds.

It was hardcoded in some dependency scripts, but that was a build-script
assumption rather than a requirement from Abseil, gRPC, SPDK, or DPDK.  It also
breaks naturally on Ubuntu 24.04 images where ``g++-11`` is not present by
default, and it breaks when ``WITH_CRIMSON=ON`` because Crimson requires GCC 13.

For a GCC-based path, GCC 13.3 or newer is a better baseline because upstream
RPM packaging explicitly requires it for LTO correctness.


Why GCC 13 downstream
---------------------

GCC 13 is a good downstream baseline for both OSD and Crimson because it:

* satisfies the Crimson GCC requirement;
* aligns with the upstream RPM ``gcc-c++ >= 13.3`` LTO requirement;
* keeps GCC diagnostics and code generation consistent across the downstream
  build matrix;
* avoids the old GCC 11 hardcoding issue;
* avoids Clang-only LLD flags in the default downstream configs.

The tradeoff is that this diverges from upstream's Ubuntu/Debian helper path,
which still prefers Clang 19.  That is acceptable here because the downstream
goal is to validate a native GCC 13/LTO stack for both ``ceph-osd`` and
``crimson-osd``.


Feature choices
---------------

SPDK
~~~~

SPDK is the Storage Performance Development Kit.  It provides user-space
storage libraries, especially for high-performance NVMe access.  In this tree,
SPDK also pulls DPDK as a dependency.

Keep SPDK enabled for classic ``ceph-osd`` builds:

* classic OSD/BlueStore paths may need SPDK support;
* this validates that SPDK and DPDK build under the selected OSD toolchain;
* only SPDK's own functional test binaries are skipped.

Keep SPDK disabled for Crimson builds:

* Crimson/SeaStore does not need SPDK for the validated build target;
* disabling it reduces the dependency and linker surface;
* it avoids pulling DPDK/SPDK failures into Crimson when the target being
  validated is ``crimson-osd`` itself.

DPDK
~~~~

DPDK is the Data Plane Development Kit.  It provides user-space packet,
memory, and device frameworks.  In this build path it is mainly relevant as an
SPDK dependency.

Ceph also has Seastar/DPDK-related integration, but that is separate from the
SPDK dependency path.  ``BuildDPDK.cmake`` explicitly prevents enabling the
Seastar DPDK path together with ``WITH_SPDK``.

IPO/LTO
~~~~~~~

``CMAKE_INTERPROCEDURAL_OPTIMIZATION`` enables CMake's IPO/LTO support.  With
GCC 13 this uses GCC LTO, and with Clang 19 this uses LLVM LTO/ThinLTO for
optimized cross-translation-unit code generation.  In Clang/LLD builds,
``librados.so`` links with GNU ``ld.bfd`` for the ABI reason described above.

Keep IPO enabled for validated OSD and Crimson builds because it is closer to a
production/performance-oriented build and may catch LTO-only link issues early.
The optional custom OpenSSL build is the exception: it keeps conservative
non-LTO CFLAGS unless ``AKCEPH_OPENSSL_CFLAGS`` is overridden explicitly.

Keep it configurable because disabling IPO is still useful for:

* faster local iteration;
* lower linker memory use;
* clearer debugging and stack inspection;
* sanitizer or instrumentation experiments;
* isolating compiler/linker regressions during bisection.

``RelWithDebInfo``
~~~~~~~~~~~~~~~~~~

``RelWithDebInfo`` is the right default for these container builds because it
combines optimization with debuggability:

* optimized code, typically ``-O2``;
* debug symbols via ``-g``;
* ``NDEBUG`` enabled;
* useful stack traces, core dumps, and profiler output.

The tradeoff is larger artifacts and the usual optimized-code debugging
quirks, but it is a better default than ``Debug`` for performance-sensitive
targets and more diagnosable than plain ``Release``.


Ubuntu 24.04 note
-----------------

The original failure was observed on Ubuntu 24.04 because the dependency
scripts forced ``g++-11``.  The script changes remove that hardcoded compiler
choice and the downstream OSD/Crimson configs now select GCC 13 explicitly.

This document's validated matrix is Ubuntu 22.04.  Ubuntu 24.04 should follow
the same single-toolchain principle, but it should be treated as a separate
validation target because distro package versions and dependency behavior can
change.
