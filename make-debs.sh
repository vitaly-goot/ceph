#!/usr/bin/env bash
#
# Copyright (C) 2015 Red Hat <contact@redhat.com>
#
# Author: Loic Dachary <loic@dachary.org>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU Library Public License as published by
# the Free Software Foundation; either version 2, or (at your option)
# any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Library Public License for more details.
#
set -xe

. /etc/os-release
base=${1:-/tmp/release}
releasedir=$base/$ID/WORKDIR
rm -fr $(dirname $releasedir)

# git describe provides a version that is
# a) human readable
# b) is unique for each commit
# c) compares higher than any previous commit
# d) contains the short hash of the commit
#
# CI builds compute the version at an earlier stage, via the same method. Since
# git metadata is not part of the source distribution, we take the version as
# an argument to this script.
#
if [ -z "${2}" ]; then
    # Prefer git describe: unique per commit, ordered, human readable. Use --tags
    # so a lightweight tag is sufficient, and fall back to the debian/changelog
    # version when the checkout carries no tags at all (e.g. a tagless CI clone).
    # Without this fallback an empty $vers builds a "ceph-.tar.bz2" and later runs
    # "dch -v -1", which aborts with "-1 is not a valid version".
    vers=$(git describe --tags --match "v*" 2>/dev/null | sed s/^v//)
    if [ -z "$vers" ]; then
        vers=$(perl -ne 'if (/\(([^)]+)\)/) { my $v=$1; $v=~s/-[^-]*$//; print $v; exit }' debian/changelog)
    fi
    if [ -z "$vers" ]; then
        echo "make-debs.sh: cannot determine version (no v* git tag and no debian/changelog version)" >&2
        exit 1
    fi
    dvers=${vers}-1
else
    vers=${2}
    dvers=${vers}-1${VERSION_CODENAME}
fi

#
# Align Debian package builds with the custom dependency image.  The normal
# build path discovers the compiler from src/script/custom/config.env, but
# dpkg-buildpackage goes directly through debian/rules.  Export the same
# defaults here so CMake does not build Ceph with a different compiler than
# the Abseil/gRPC archives produced by the custom image script.
#
if [ -r src/script/custom/config.env ]; then
   # shellcheck disable=SC1091
   . src/script/custom/config.env
   if [ -z "${CC:-}" ] && [ -n "${AKCEPH_C_COMPILER:-}" ]; then
      export CC="${AKCEPH_C_COMPILER}"
   fi
   if [ -z "${CXX:-}" ] && [ -n "${AKCEPH_CXX_COMPILER:-}" ]; then
      export CXX="${AKCEPH_CXX_COMPILER}"
   fi
   if [ -z "${WITH_SPDK:-}" ] && [ -n "${AKCEPH_WITH_SPDK:-}" ]; then
      export WITH_SPDK="${AKCEPH_WITH_SPDK}"
   fi
   if [ -z "${CMAKE_INTERPROCEDURAL_OPTIMIZATION:-}" ] &&
      [ -n "${AKCEPH_CMAKE_INTERPROCEDURAL_OPTIMIZATION:-}" ]; then
      export CMAKE_INTERPROCEDURAL_OPTIMIZATION="${AKCEPH_CMAKE_INTERPROCEDURAL_OPTIMIZATION}"
   fi
fi

test -f "ceph-$vers.tar.bz2" || ./make-dist $vers

# Optionally run an extra command after the source tarball exists (intended for
# trivy or similar). This historically reused $2 -- which is the *version*
# argument -- so passing a version (e.g. build-with-container.py --ceph-version)
# executed it as a shell command ("20.2.1: command not found"). Use a dedicated
# variable so the version argument is never run.
if [[ -n "${MAKE_DEBS_EXTRA_CMD:-}" ]]; then
    echo "** Running extra command '${MAKE_DEBS_EXTRA_CMD}'"
    ${MAKE_DEBS_EXTRA_CMD}
    echo "** Extra command '${MAKE_DEBS_EXTRA_CMD}' completed"
fi
#
# rename the tarball to match debian conventions and extract it
#
mkdir -p $releasedir
mv ceph-$vers.tar.bz2 $releasedir/ceph_$vers.orig.tar.bz2
tar -C $releasedir --no-same-owner -jxf $releasedir/ceph_$vers.orig.tar.bz2

#
# Optionally disable -dbg package builds
# because they are large and take time to build
#
cp -a debian $releasedir/ceph-$vers/debian
cd $releasedir
if [[ -n "$SKIP_DEBUG_PACKAGES" ]] ; then
	perl -ni -e 'print if(!(/^Package: .*-dbg$/../^$/))' ceph-$vers/debian/control
	perl -pi -e 's/--dbg-package.*//' ceph-$vers/debian/rules
fi

#
# update the changelog to match the desired version
#
cd ceph-$vers
chvers=$(head -1 debian/changelog | perl -ne 's/.*\(//; s/\).*//; print')
if [ "$chvers" != "$dvers" ]; then
   DEBEMAIL="contact@ceph.com" dch -D $VERSION_CODENAME --force-distribution -b -v "$dvers" "new version"
fi

echo "Package build CC=${CC:-unset}"
echo "Package build CXX=${CXX:-unset}"
echo "Package build WITH_SPDK=${WITH_SPDK:-unset}"
echo "Package build CMAKE_INTERPROCEDURAL_OPTIMIZATION=${CMAKE_INTERPROCEDURAL_OPTIMIZATION:-unset}"
echo "Package build AKCEPH_PACKAGE_NPROC_MAX=${AKCEPH_PACKAGE_NPROC_MAX:-unset}"

#
# Add a -j option if $DEB_BUILD_OPTIONS doesn't have parallel=n in it.
# Default: use half of the available processors
PARALLEL="parallel"
echo "DEB_BUILD_OPTIONS " $DEB_BUILD_OPTIONS
if [[ ! $DEB_BUILD_OPTIONS =~ $PARALLEL ]] ; then
   : ${NPROC:=$(($(nproc) / 2))}
   if [[ -n "${AKCEPH_PACKAGE_NPROC_MAX:-}" &&
         "${AKCEPH_PACKAGE_NPROC_MAX}" =~ ^[0-9]+$ &&
         "${AKCEPH_PACKAGE_NPROC_MAX}" -gt 0 &&
         "${NPROC}" =~ ^[0-9]+$ &&
         "${NPROC}" -gt "${AKCEPH_PACKAGE_NPROC_MAX}" ]]; then
      echo "Package build NPROC capped from ${NPROC} to ${AKCEPH_PACKAGE_NPROC_MAX}"
      NPROC="${AKCEPH_PACKAGE_NPROC_MAX}"
   fi
   echo "Package build NPROC=${NPROC}"
   if test $NPROC -gt 1 ; then
      j=-j${NPROC}
   fi
fi
#
# create the packages
# a) with ccache to speed things up when building repeatedly
# b) do not sign the packages
#
if [ "$SCCACHE" != "true" ] ; then
    PATH=/usr/lib/ccache:$PATH
fi
PATH=$PATH dpkg-buildpackage $j -uc -us
cd ../..
mkdir -p $VERSION_CODENAME/conf
cat > $VERSION_CODENAME/conf/distributions <<EOF
Codename: $VERSION_CODENAME
Suite: stable
Components: main
Architectures: $(dpkg --print-architecture) source
EOF
if [ ! -e conf ]; then
    ln -s $VERSION_CODENAME/conf conf
fi
reprepro --basedir $(pwd) include $VERSION_CODENAME WORKDIR/*.changes
#
# Pick up the runtime libstdc++6/libgcc-s1 package built at image-build time
# (src/script/custom/runtime-libstdcxx.sh, run via run-all.sh) so it rides
# through the same local repo as the rest of the ceph packages. Built as a
# plain .deb (no source/.changes), so it's added with includedeb rather than
# include.
#
runtime_libstdcxx_deb=/usr/local/akceph-runtime/akceph-runtime-libstdcxx6.deb
if [ "${AKCEPH_BUNDLE_RUNTIME_LIBSTDCXX:-1}" = 1 ] && [ -f "$runtime_libstdcxx_deb" ]; then
    reprepro --basedir $(pwd) includedeb $VERSION_CODENAME "$runtime_libstdcxx_deb"
fi
#
# teuthology needs the version in the version file
#
echo $dvers > $VERSION_CODENAME/version
