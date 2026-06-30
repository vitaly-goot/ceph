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
* keep IPO/LTO configurable, and enable it for the validated OSD, Crimson, and
  Debian package defaults;
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
  custom Abseil/gRPC/OpenSSL dependency builds, and provides default package
  build policy for ``make-debs.sh``;
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
   * - Debian packages
     - ``src/script/custom/config.env`` plus the supplied ``--env-file``
     - GCC 13
     - GCC default linker, normally GNU ``ld.bfd``
     - On by default
     - On by default
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

To keep those paths aligned, ``make-debs.sh`` sources the live checkout's
``src/script/custom/config.env`` before creating/extracting the source tarball
and exports default package-build values when the caller did not already
provide them.  The downstream defaults are:

* ``CC=gcc-13`` and ``CXX=g++-13``;
* ``WITH_SPDK=ON``;
* ``CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON``;
* ``AKCEPH_PACKAGE_NPROC_MAX=4``.

``RelWithDebInfo`` still comes from ``debian/rules``.  The package build is
therefore optimized with debug info.  Callers can still opt out of package IPO
explicitly with ``CMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF`` in the env file or
command environment.

For package builds with IPO/LTO enabled, ``make-debs.sh`` caps ``NPROC`` to
``AKCEPH_PACKAGE_NPROC_MAX=4`` by default unless the caller overrides that
policy.  This does not change optimization or generated code; it only lowers
concurrent build jobs so late RGW, gRPC, Abseil, and Arrow links are less
likely to be killed by memory pressure.

``debian/rules`` derives the package CMake arguments from these environment
variables:

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

Avoiding unshipped helper links in package builds
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``dbstore`` is RGW's database-backed storage driver plumbing.  The package
build still builds and links the real RGW dbstore libraries, including
``dbstore_lib``, ``sqlite_db``, and ``dbstore``.  RGW support is not disabled.

The only target excluded from the default package build is ``dbstore-bin``.  It
is a standalone helper executable from
``src/rgw/driver/dbstore/dbstore_main.cc``; the CMake file labels it "testing
purpose", and it is not installed by the Debian package manifests.  With
IPO/LTO enabled it also pulls in a large RGW/dbstore/sqlite/gRPC/Abseil link
graph without producing a package artifact.

The package default still keeps GCC 13, SPDK, ``RelWithDebInfo``, and IPO/LTO
enabled.  Excluding ``dbstore-bin`` from the default ``all`` target avoids
building an unshipped helper while leaving it buildable explicitly for
developers:

.. code-block:: console

   ninja dbstore-bin

This matters in CI because the workflow passes a generated
``src/script/custom/build-vars.config`` to ``build-with-container.py``.  That
file carries cache and parallelism settings, but it is not the same as
``ccache.osd.config`` or ``ccache.crimson.config``.  Sourcing
``src/script/custom/config.env`` early in ``make-debs.sh`` keeps package builds
on the same GCC 13 compiler, package defaults, and package parallelism cap as
the custom Abseil/gRPC dependency image.  The generated package-build env can
still set operational settings such as ccache remote storage and a requested
``NPROC``; the downstream cap reduces it when needed for IPO/LTO memory
headroom.

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
     - Controlled by ``CMAKE_INTERPROCEDURAL_OPTIMIZATION`` from the env file.
       The normal ``-e build`` OSD/Crimson configs and package defaults enable
       it; callers can opt out explicitly.
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
     - ``make-debs.sh`` exports package defaults for compiler, SPDK, and CMake
       IPO before ``debian/rules`` configures CMake.  Uninstalled helper
       binaries such as ``dbstore-bin`` are kept out of the default package
       ``all`` build.
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
   AKCEPH_WITH_SPDK=ON
   AKCEPH_CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON
   AKCEPH_PACKAGE_NPROC_MAX=4
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


Runtime libstdc++ on deployment targets
----------------------------------------

Building with GCC 13 (see `Toolchain choice`_) fixes the *build*, but it
creates a second, separate problem on the *deployment* side: a plain Ubuntu
22.04 target (for example a stock ``ubuntu:22.04`` image, or a host that never
had ``ppa:ubuntu-toolchain-r/test`` enabled) ships a ``libstdc++6`` built for
GCC 11/12.  The package install itself succeeds -- ``dh_shlibdeps`` does not
always capture the real minimum version a GCC 13 binary needs -- but the
binary aborts on first run:

.. code-block:: text

   $ ldd /usr/bin/ceph-osd
   /usr/bin/ceph-osd: ... libstdc++.so.6: version `GLIBCXX_3.4.31' not found
   /usr/bin/ceph-osd: ... libstdc++.so.6: version `GLIBCXX_3.4.32' not found

This is unrelated to the SPDK/IPO/linker choices above: those are all
compile-time decisions, this is a runtime shared-library version problem on
the *target* machine, which may never have had any part of this build's
toolchain installed.

The real fix is the deployment OS, not a bundled library
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

All of the bundling described below exists only because Ubuntu 22.04's
*native* ``libstdc++6`` predates GCC 13.  A distro whose default toolchain is
already GCC 13+ has none of this problem: its own ``libstdc++6`` already
provides the GLIBCXX symbol versions GCC 13 emits, with no PPA, no
repackaging step, and no separate runtime package to keep in sync.

Two candidates ship GCC 13+ by default:

* **Ubuntu 24.04 (noble)** -- ``gcc``/``g++`` *are* GCC 13.2 by default, no
  PPA needed even for the build itself.  This is likely the easier move of
  the two: it stays in the Ubuntu family (same package naming, same apt
  tooling, same support/compliance story this fork already relies on), and
  ``build-with-container.py`` already has a ``-d ubuntu24.04`` distro entry.
  See the `Ubuntu 24.04 note`_ above for the one thing that already bit this
  tree on noble (``g++-11`` isn't installable there) -- that was a build-side
  hardcoded-compiler bug, already fixed by this same toolchain-selection
  work, not a reason to avoid noble.
* **Debian trixie** -- ships GCC 14 by default.  ``build-with-container.py``/
  ``DistroKind`` already support it (``-d debian13``/``-d trixie``), and
  ``.github/workflows/build-deliverables.yml`` has a commented-out
  ``debian13``/``debian:trixie`` matrix entry for exactly this reason.  A
  bigger platform jump than noble if the rest of this fork's tooling assumes
  Ubuntu/apt conventions that happen to differ on Debian.

Either way, this is a deployment-OS decision, not a rebuild-the-toolchain
decision -- it does not require dropping GCC 13, only choosing a base image
that already ships it.

The bundling approach below stays useful for two cases that won't go away:
deployment targets that must stay on Ubuntu 22.04 for other reasons, and any
older system libstdc++6 in general (the same GLIBCXX-version-not-found
failure mode applies anywhere the deployment OS is older than the build
toolchain, not just on Ubuntu 22.04).  But if the deployment target is free to
move, switching the base/runtime image to a GCC-13-or-newer distro like
Ubuntu 24.04 or trixie removes the need for this workaround entirely rather
than working around it.

Why not bundle a newer distro's libstdc++ (e.g. Debian trixie)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The obvious-looking fix is to grab ``libstdc++.so.6`` from a newer distro
release and drop it in.  This was tried with Debian trixie and fails one
level down: trixie's ``libstdc++.so.6.0.33`` itself requires
``GLIBC_2.36``/``2.38``, which Ubuntu 22.04 does not have
(``GLIBC_2.35``).  Swapping in that library trades a GLIBCXX-version crash
for a GLIBC-version crash, and "fix" by upgrading glibc system-wide is a far
larger blast radius than the original bug -- glibc underlies every process on
the machine, not just Ceph's.

The fix: repackage the same PPA already used for the compiler
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``ppa:ubuntu-toolchain-r/test`` -- the PPA already used to install
``gcc-13``/``g++-13`` for this distro (see `Toolchain choice`_) -- also
publishes a ``libstdc++6`` build, built specifically for this distro's own
glibc.  It satisfies the GLIBCXX requirement without raising the glibc floor,
because it is built against the same glibc the target already has.

``src/script/custom/runtime-libstdcxx.sh`` repackages that PPA's
``libstdc++6``/``libgcc-s1`` into a standalone package,
``akceph-runtime-libstdcxx6``, at image-build time (it needs root and a fresh
``apt`` index, which the package-build step does not have).  It runs as one
of the ``run-all.sh`` custom-image scripts, gated by
``AKCEPH_BUNDLE_RUNTIME_LIBSTDCXX`` (default on) in
``src/script/custom/config.env``.  The package:

* ``Provides:``/``Replaces:`` ``libstdc++6``/``libgcc-s1``, so it upgrades the
  real system libraries in place via normal ``dpkg``/``apt`` mechanics rather
  than a private, easy-to-miss search path;
* keeps the PPA package's own ``Depends: libc6 (>= X)``, so a target whose
  glibc really is too old fails clearly at ``apt install`` time instead of
  crashing later at first daemon start.

``make-debs.sh`` adds the prebuilt ``.deb`` to the same local repo as the rest
of the Ceph packages via ``reprepro includedeb`` (no source package or
``.changes`` needed for a single binary package).  ``.github/docker/Dockerfile.debian-packages``
picks it up for free through its existing "install everything found in the
repo" logic, and that Dockerfile now runs a build-time smoke test
(``ldd /usr/bin/ceph-osd | grep -i "not found"``) so a regression here fails
the image build instead of shipping silently broken packages.

For non-Docker deployment targets, ship ``akceph-runtime-libstdcxx6_*.deb``
as a normal companion artifact alongside the other release ``.deb``\ s and
install it the same way, before or together with the Ceph packages.

Not covered by this fix
~~~~~~~~~~~~~~~~~~~~~~~~

A GCC-13-built binary can still hit an unrelated ``Illegal instruction``
(SIGILL) on hosts whose CPU does not support every instruction
``-march=x86-64-v3`` assumes is always present (in particular ``lzcnt``).
That is a CPU-microarchitecture question -- whether the real target fleet
supports ``x86-64-v3``, or whether ``build_arch`` needs a lower baseline for
some hosts -- and is independent of the libstdc++ version problem this
section fixes.  ``ldd`` cannot catch it: resolving symbol versions doesn't
execute any code, so a passing smoke test here does not guarantee the binary
runs on every target CPU.
