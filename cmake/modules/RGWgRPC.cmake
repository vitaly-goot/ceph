# cmake/RGWgRPC.cmake

# FUNCTION: buf_grpc_expand(var_proto, var_header, var_source, proto_dir,
# proto_list, output_dir)
#
# Automate expanding protobuf path names into lists of .proto, .cc and .h
# files for protobuf and grpc generated sources.
#
# This precise specification means we can use the .proto list as DEPENDS input
# for add_custom_command(), and use the .cc and .h lists as source files for
# add_library() when building the grpc object files.
#
# This means we can rebuild only when necessary, but still be confident that
# changes in our sources will result in cascading rebuilds when necessary.
# gRPC and protobuf are *not* header-only - there are object files that need
# to be linked.
#
# var_proto, var_header and var_source are strings containing the names of
# variables to receive the expansions.
#
# proto_dir is the root location of source files, e.g.
# ${CMAKE_SOURCE_DIR}/src/rgw.
#
# proto_list is a list of paths to protobuf files without the .proto suffix,
# e.g. rgw/auth/v1/auth;rgw/auth/v1/other.
#
# output_dir is the root location of buf generated files, e.g.
# ${CMAKE_BINARY_DIR}/bufgen.
#
# So with input proto list "rgw/auth/v1/auth;rgw/auth/v1/other", expand such
# that (newlines added):
#
# var_proto = ${proto_dir}/rgw/auth/v1/auth.proto
#
# var_header = ${output_dir}/rgw/auth/v1/auth.pb.h;
# ${output_dir}/rgw/auth/v1/auth.grpc.pb.h;
# ${output_dir}/rgw/auth/v1/other.pb.h;
# ${output_dir}/rgw/auth/v1/other.grpc.pb.h
#
# var_source = ${output_dir}/rgw/auth/v1/auth.pb.cc;
# ${output_dir}/rgw/auth/v1/auth.grpc.pb.cc;
# ${output_dir}/rgw/auth/v1/other.pb.cc;
# ${output_dir}/rgw/auth/v1/other.grpc.pb.cc;
#
function(buf_grpc_expand var_proto var_header var_source proto_dir proto_list output_dir)
    message(VERBOSE "grpc_expand: begin")
    unset(_proto)
    unset(_hdr)
    unset(_src)

    foreach(input ${proto_list})
        message(VERBOSE "grpc_expand: input ${input}")
        list(APPEND _proto ${proto_dir}/${input}.proto)
        list(APPEND _hdr ${output_dir}/${input}.pb.h)
        list(APPEND _hdr ${output_dir}/${input}.grpc.pb.h)
        list(APPEND _src ${output_dir}/${input}.pb.cc)
        list(APPEND _src ${output_dir}/${input}.grpc.pb.cc)
        # list(APPEND _src ${output_dir}/${input}_pb2.py)
        # list(APPEND _src ${output_dir}/${input}_pb2_grpc.py)
    endforeach()

    # Export var_header and var_source (can't use return(PROPAGATE), we can't
    # rely on having CMake >= 3.25).
    set(${var_proto} "${_proto}" PARENT_SCOPE)
    set(${var_header} "${_hdr}" PARENT_SCOPE)
    set(${var_source} "${_src}" PARENT_SCOPE)

    message(VERBOSE "grpc_expand: ${var_proto}: ${_proto}")
    message(VERBOSE "grpc_expand: ${var_header}: ${_hdr}")
    message(VERBOSE "grpc_expand: ${var_source}: ${_src}")
    message(VERBOSE "grpc_expand: done")
endfunction(buf_grpc_expand)
