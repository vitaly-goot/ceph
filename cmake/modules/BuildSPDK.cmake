macro(build_spdk)
  set(DPDK_DIR ${CMAKE_BINARY_DIR}/src/dpdk)
  if(NOT TARGET dpdk-ext)
    include(BuildDPDK)
    build_dpdk(${DPDK_DIR})
  endif()
  find_package(CUnit REQUIRED)
  if(LINUX)
    find_package(aio REQUIRED)
    find_package(uuid REQUIRED)
  endif()
  include(FindMake)
  find_make("MAKE_EXECUTABLE" "make_cmd")

  set(spdk_CFLAGS "-fPIC")
  include(CheckCCompilerFlag)
  check_c_compiler_flag("-Wno-address-of-packed-member" HAVE_WARNING_ADDRESS_OF_PACKED_MEMBER)
  if(HAVE_WARNING_ADDRESS_OF_PACKED_MEMBER)
    string(APPEND spdk_CFLAGS " -Wno-address-of-packed-member")
  endif()
  check_c_compiler_flag("-Wno-unused-but-set-variable"
    HAVE_UNUSED_BUT_SET_VARIABLE)
  if(HAVE_UNUSED_BUT_SET_VARIABLE)
    string(APPEND spdk_CFLAGS " -Wno-unused-but-set-variable")
  endif()

  include(ExternalProject)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "amd64|x86_64|AMD64")
    # a safer option than relying on the build host's arch
    set(target_arch core2)
  else()
    # default arch used by SPDK
    set(target_arch native)
  endif()

  set(source_dir "${CMAKE_SOURCE_DIR}/src/spdk")
  foreach(c lvol env_dpdk sock nvmf bdev nvme conf thread trace notify accel event_accel blob vmd event_vmd event_bdev sock_posix event_sock event rpc jsonrpc json util log)
    add_library(spdk::${c} STATIC IMPORTED)
    set(lib_path "${source_dir}/build/lib/${CMAKE_STATIC_LIBRARY_PREFIX}spdk_${c}${CMAKE_STATIC_LIBRARY_SUFFIX}")
    set_target_properties(spdk::${c} PROPERTIES
      IMPORTED_LOCATION "${lib_path}"
      INTERFACE_INCLUDE_DIRECTORIES "${source_dir}/include")
    list(APPEND spdk_libs "${lib_path}")
    list(APPEND SPDK_LIBRARIES spdk::${c})
  endforeach()

  set(spdk_BUILD_ENV PATH=$ENV{PATH})
  foreach(env_var
      HOME
      XDG_CACHE_HOME
      CCACHE_BASEDIR
      CCACHE_COMPILERCHECK
      CCACHE_CONFIGPATH
      CCACHE_CPP2
      CCACHE_DIR
      CCACHE_HASHDIR
      CCACHE_MAXSIZE
      CCACHE_NAMESPACE
      CCACHE_NOHASHDIR
      CCACHE_REMOTE_ONLY
      CCACHE_REMOTE_STORAGE
      CCACHE_RESHARE
      CCACHE_SLOPPINESS
      CCACHE_TEMPDIR)
    if(DEFINED ENV{${env_var}})
      list(APPEND spdk_BUILD_ENV "${env_var}=$ENV{${env_var}}")
    endif()
  endforeach()
  # Build the libraries/modules Ceph links.  The default SPDK "all" target also
  # builds standalone apps such as spdk_top, which are not consumed by Ceph and
  # can trip Debian hardening warnings promoted to errors.  Do not use the
  # top-level "module" target: this pinned SPDK makefile forwards the original
  # goal into lib/, where "module" is not a valid target.  Still run the
  # top-level generated-header target first so subdirectory builds can include
  # spdk/config.h.
  set(spdk_BUILD_COMMAND
    "${MAKE_EXECUTABLE} build_dir include/spdk/config.h && ${MAKE_EXECUTABLE} -C lib all EXTRA_CFLAGS='${spdk_CFLAGS}' && ${MAKE_EXECUTABLE} -C module all EXTRA_CFLAGS='${spdk_CFLAGS}'")

  ExternalProject_Add(spdk-ext
    DEPENDS dpdk-ext
    SOURCE_DIR ${source_dir}
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env
      CC=${CMAKE_C_COMPILER}
      CXX=${CMAKE_CXX_COMPILER}
      ./configure
      --with-dpdk=${DPDK_DIR}
      --without-isal
      --without-vhost
      # Ceph consumes the SPDK libraries, not its functional test binaries.
      --disable-tests
      --disable-examples
      --target-arch=${target_arch}
    # unset $CFLAGS, otherwise it will interfere with how SPDK sets
    # its include directory.
    # unset $LDFLAGS, otherwise SPDK will fail to mock some functions.
    # Use the actual make executable here instead of ${make_cmd}.  When the
    # parent generator is Makefiles, FindMake returns "$(MAKE)" so recursive
    # make can inherit jobserver flags.  ExternalProject wraps this command in a
    # cmake -P script because LOG_BUILD is enabled, so "$(MAKE)" would be passed
    # literally to env rather than expanded by make.
    #
    # Keep env -i for SPDK's fragile CFLAGS/LDFLAGS handling, but preserve the
    # ccache operating context.  Debian container builds run as a numeric uid
    # without a passwd entry, so ccache cannot infer HOME after env -i strips it.
    BUILD_COMMAND env -i ${spdk_BUILD_ENV}
      CC=${CMAKE_C_COMPILER}
      CXX=${CMAKE_CXX_COMPILER}
      /bin/sh -c "${spdk_BUILD_COMMAND}"
    BUILD_IN_SOURCE 1
    BUILD_BYPRODUCTS ${spdk_libs}
    INSTALL_COMMAND ""
    LOG_CONFIGURE ON
    LOG_BUILD ON
    LOG_MERGED_STDOUTERR ON
    LOG_OUTPUT_ON_FAILURE ON)
  unset(make_cmd)
  unset(MAKE_EXECUTABLE)
  foreach(spdk_lib ${SPDK_LIBRARIES})
    add_dependencies(${spdk_lib} spdk-ext)
  endforeach()

  set(SPDK_INCLUDE_DIR "${source_dir}/include")
  unset(spdk_BUILD_COMMAND)
  unset(spdk_BUILD_ENV)
  add_library(spdk::spdk INTERFACE IMPORTED)
  add_dependencies(spdk::spdk
    ${SPDK_LIBRARIES})
  # workaround for https://review.spdk.io/gerrit/c/spdk/spdk/+/6798
  set_target_properties(spdk::spdk PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES ${SPDK_INCLUDE_DIR}
    INTERFACE_LINK_LIBRARIES
    "-Wl,--whole-archive $<JOIN:${spdk_libs}, > -Wl,--no-whole-archive;dpdk::dpdk;rt;${UUID_LIBRARIES}")
  unset(source_dir)
endmacro()
