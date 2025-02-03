function(target_create _target _lib)
  add_library(${_target} STATIC IMPORTED)
  set_target_properties(
    ${_target} PROPERTIES IMPORTED_LOCATION
                          "${opentelemetry_BINARY_DIR}/${_lib}")
endfunction()

function(build_opentelemetry)
  set(opentelemetry_SOURCE_DIR "${PROJECT_SOURCE_DIR}/src/jaegertracing/opentelemetry-cpp")
  set(opentelemetry_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/opentelemetry-cpp")
  set(opentelemetry_cpp_targets opentelemetry_trace opentelemetry_exporter_jaeger_trace opentelemetry_exporter_otlp_grpc opentelemetry_otlp_recordable opentelemetry_proto)
  # Akamai: We have to help OTel build in a way compatible with our external
  # abseil-cpp and gRPC. We *do* need to pass the CMAKE_PREFIX_PATH through
  # (this includes absl as well as some gRPC parts) but we'll do that in
  # ExternalProject_add later - it's complicated. We also need to build in
  # C++17 mode, and specify that we already have abseil and don't need to use
  # the opentelemetry in-tree version. We may need to switch this to C++20
  # mode at some point.
  set(opentelemetry_CMAKE_ARGS -DCMAKE_POSITION_INDEPENDENT_CODE=ON
                               -DCMAKE_CXX_STANDARD=17
                               -DWITH_ABSEIL=ON
                               -DWITH_JAEGER=ON
                               -DWITH_OTLP=ON
                               -DBUILD_TESTING=OFF
                               -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
                               -DWITH_EXAMPLES=OFF
                               -DProtobuf_PROTOC_EXECUTABLE=${RGW_GRPC_ROOT_DIR}/bin/protoc
                               )

  set(opentelemetry_libs
      ${opentelemetry_BINARY_DIR}/sdk/src/trace/libopentelemetry_trace.a
      ${opentelemetry_BINARY_DIR}/sdk/src/resource/libopentelemetry_resources.a
      ${opentelemetry_BINARY_DIR}/sdk/src/common/libopentelemetry_common.a
      ${opentelemetry_BINARY_DIR}/exporters/jaeger/libopentelemetry_exporter_jaeger_trace.a
      ${opentelemetry_BINARY_DIR}/exporters/otlp/libopentelemetry_exporter_otlp_grpc.a
      ${opentelemetry_BINARY_DIR}/exporters/otlp/libopentelemetry_otlp_recordable.a
      ${opentelemetry_BINARY_DIR}/libopentelemetry_proto.a
      ${opentelemetry_BINARY_DIR}/ext/src/http/client/curl/libopentelemetry_http_client_curl.a
      ${CURL_LIBRARIES}
  )
  set(opentelemetry_include_dir ${opentelemetry_SOURCE_DIR}/api/include/
                                ${opentelemetry_SOURCE_DIR}/exporters/jaeger/include/
                                ${opentelemetry_SOURCE_DIR}/exporters/otlp/include/
                                ${opentelemetry_SOURCE_DIR}/ext/include/
                                ${opentelemetry_SOURCE_DIR}/sdk/include/
                                ${opentelemetry_BINARY_DIR}/generated/third_party/opentelemetry-proto/
                                ${RGW_GRPC_ROOT_DIR}/include/
                                )
  # TODO: add target based propogation
  set(opentelemetry_deps opentelemetry_trace opentelemetry_resources opentelemetry_common
                         opentelemetry_exporter_jaeger_trace opentelemetry_exporter_otlp_grpc opentelemetry_otlp_recordable opentelemetry_proto http_client_curl
			 ${CURL_LIBRARIES})

  if(CMAKE_MAKE_PROGRAM MATCHES "make")
    # try to inherit command line arguments passed by parent "make" job
    set(make_cmd $(MAKE) ${opentelemetry_cpp_targets})
  else()
    set(make_cmd ${CMAKE_COMMAND} --build <BINARY_DIR> --target
                 ${opentelemetry_cpp_targets})
  endif()

  if(WITH_SYSTEM_BOOST)
    list(APPEND opentelemetry_CMAKE_ARGS -DBOOST_ROOT=${BOOST_ROOT})
  else()
    list(APPEND dependencies Boost)
    list(APPEND opentelemetry_CMAKE_ARGS -DBoost_INCLUDE_DIR=${CMAKE_BINARY_DIR}/boost/include)
  endif()

  include(ExternalProject)
  
  # We have to pass CMAKE_PREFIX_PATH into the subordinate CMake invocation to
  # configure OpenTelemetry. CMAKE_PREFIX_PATH is a list, so it's
  # semicolon-separated. This breaks ExternalProject_Add, and it's not
  # specific to Ceph - see here:
  #  https://stackoverflow.com/questions/45414507/pass-a-list-of-prefix-paths-to-externalproject-add-in-cmake-args
  #
  # Use the solution the SO article prescribes: Create a version of
  # CMAKE_PREFIX_PATH with a different separator, and tell ExternalProject_Add
  # what that separator is using LIST_SEPARATOR. It's a kludge, but it means
  # the OTel build can find abseil-cpp, protobuf and gRPC.
  
  # Also, PATCH_COMMAND is used to patch the in-tree jaegertracing source to
  # detect our protobuf (and, by extension, gRPC). Seems that the source
  # expects 'Protobuf', and we have 'protobuf'. The capitalisation matters.
  
  # Create a list with an alternate separator e.g. pipe symbol
  string(REPLACE ";" "|" CMAKE_PREFIX_PATH_ALT_SEP "${CMAKE_PREFIX_PATH}")
  
  set(OTEL_PATCH ${CMAKE_SOURCE_DIR}/cmake/modules/jaegertracing_akamai_grpc_detect.cmakepatch)
  
  ExternalProject_Add(opentelemetry-cpp
    SOURCE_DIR ${opentelemetry_SOURCE_DIR}
    PREFIX "opentelemetry-cpp"
    PATCH_COMMAND ${CMAKE_SOURCE_DIR}/cmake/modules/forwardpatch.sh ${OTEL_PATCH}
    LIST_SEPARATOR |
    CMAKE_ARGS ${opentelemetry_CMAKE_ARGS} -DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH_ALT_SEP}
    BUILD_COMMAND ${make_cmd}
    BINARY_DIR ${opentelemetry_BINARY_DIR}
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS ${opentelemetry_libs}
    DEPENDS ${dependencies}
    LOG_BUILD ON)

  # CMake doesn't allow to add a list of libraries to the import property, hence
  # we create individual targets and link their libraries which finally
  # interfaces to opentelemetry target
  target_create("opentelemetry_trace" "sdk/src/trace/libopentelemetry_trace.a")
  target_create("opentelemetry_resources"
                "sdk/src/resource/libopentelemetry_resources.a")
  target_create("opentelemetry_common"
                "sdk/src/common/libopentelemetry_common.a")
  target_create("opentelemetry_exporter_jaeger_trace"
                "exporters/jaeger/libopentelemetry_exporter_jaeger_trace.a")
  target_create("opentelemetry_exporter_otlp_grpc"
                "exporters/otlp/libopentelemetry_exporter_otlp_grpc.a")
  target_create("opentelemetry_otlp_recordable"
                "exporters/otlp/libopentelemetry_otlp_recordable.a")
  target_create("opentelemetry_proto"
                "libopentelemetry_proto.a")
  target_create("http_client_curl"
                "ext/src/http/client/curl/libopentelemetry_http_client_curl.a")

  # will do all linking and path setting fake include path for
  # interface_include_directories since this happens at build time
  file(MAKE_DIRECTORY ${opentelemetry_include_dir})
  add_library(opentelemetry::libopentelemetry INTERFACE IMPORTED)
  add_dependencies(opentelemetry::libopentelemetry opentelemetry-cpp)
  set_target_properties(
    opentelemetry::libopentelemetry
    PROPERTIES
      INTERFACE_LINK_LIBRARIES "${opentelemetry_deps}"
      INTERFACE_INCLUDE_DIRECTORIES "${opentelemetry_include_dir}")
endfunction()
