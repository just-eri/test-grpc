cmake_minimum_required(VERSION 3.8)

project(TestGrpc C CXX)

include(common.cmake)

get_filename_component(tg_proto "api.proto" ABSOLUTE)
get_filename_component(tg_proto_path "${tg_proto}" PATH)

find_package(Qt6 COMPONENTS Widgets REQUIRED)
set(CMAKE_AUTOMOC ON)
set(tg_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/api.pb.cc")
set(tg_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/api.pb.h")
set(tg_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/api.grpc.pb.cc")
set(tg_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/api.grpc.pb.h")
add_custom_command(
      OUTPUT "${tg_proto_srcs}" "${tg_proto_hdrs}" "${tg_grpc_srcs}" "${tg_grpc_hdrs}"
      COMMAND ${_PROTOBUF_PROTOC}
      ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
        --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
        -I "${tg_proto_path}"
        --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
        "${tg_proto}"
      DEPENDS "${tg_proto}")

include_directories("${CMAKE_CURRENT_BINARY_DIR}")

add_library(tg_grpc_proto
  ${tg_grpc_srcs}
  ${tg_grpc_hdrs}
  ${tg_proto_srcs}
  ${tg_proto_hdrs})
target_link_libraries(tg_grpc_proto
  absl::check
  ${_REFLECTION}
  ${_GRPC_GRPCPP}
  ${_PROTOBUF_LIBPROTOBUF})
target_link_libraries(tg_grpc_proto Qt6::Widgets)

foreach(_target
  testgrpc_client testgrpc_server)
  add_executable(${_target} "${_target}.cc")
  target_link_libraries(${_target}
    tg_grpc_proto
    absl::check
    absl::flags
    absl::flags_parse
    absl::log
    ${_REFLECTION}
    ${_GRPC_GRPCPP}
    ${_PROTOBUF_LIBPROTOBUF})
endforeach()
