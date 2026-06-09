# gRPC services and REST gateway wiring.

find_package(gRPC CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)

# Conan provides the package folder paths in <pkg>_PACKAGE_FOLDER_<CONFIG> variables.
# The protobuf-conan-protoc-target.cmake and grpc_cpp_plugin.cmake use find_program()
# which may pick up a system-installed (incompatible) version before the Conan one.
# Override the targets' IMPORTED_LOCATION to guarantee we use the Conan-installed binaries.
if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR NOT CMAKE_BUILD_TYPE)
  set(FMGR_PROTO_PKG "${protobuf_PACKAGE_FOLDER_DEBUG}")
  set(FMGR_GRPC_PKG  "${grpc_PACKAGE_FOLDER_DEBUG}")
else()
  set(FMGR_PROTO_PKG "${protobuf_PACKAGE_FOLDER_RELEASE}")
  set(FMGR_GRPC_PKG  "${grpc_PACKAGE_FOLDER_RELEASE}")
endif()
if(FMGR_PROTO_PKG)
  set_property(TARGET protobuf::protoc PROPERTY IMPORTED_LOCATION
    "${FMGR_PROTO_PKG}/bin/protoc")
endif()
if(FMGR_GRPC_PKG)
  set_property(TARGET gRPC::grpc_cpp_plugin PROPERTY IMPORTED_LOCATION
    "${FMGR_GRPC_PKG}/bin/grpc_cpp_plugin")
endif()

# ---- Proto code generation ----

get_target_property(GRPC_CPP_PLUGIN gRPC::grpc_cpp_plugin LOCATION)

set(PROTO_ROOT "${PROJECT_SOURCE_DIR}/proto")
set(PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

set(PROTO_FILES
  fmgr/v1/common/types.proto
  fmgr/v1/auth.proto
  fmgr/v1/session.proto
  fmgr/v1/sample.proto
  fmgr/v1/lab.proto
  fmgr/v1/box.proto
  fmgr/v1/item_type.proto
  fmgr/v1/role.proto
  fmgr/v1/audit.proto
  fmgr/v1/share.proto
)

set(PROTO_GENERATED_SRCS)
foreach(PROTO_REL ${PROTO_FILES})
  get_filename_component(PROTO_ABS "${PROTO_ROOT}/${PROTO_REL}" ABSOLUTE)
  get_filename_component(PROTO_NAME "${PROTO_REL}" NAME_WE)
  get_filename_component(PROTO_SUBDIR "${PROTO_REL}" DIRECTORY)
  set(OUT_DIR "${PROTO_GEN_DIR}/${PROTO_SUBDIR}")
  file(MAKE_DIRECTORY "${OUT_DIR}")

  add_custom_command(
    OUTPUT
      "${OUT_DIR}/${PROTO_NAME}.pb.cc"
      "${OUT_DIR}/${PROTO_NAME}.pb.h"
      "${OUT_DIR}/${PROTO_NAME}.grpc.pb.cc"
      "${OUT_DIR}/${PROTO_NAME}.grpc.pb.h"
    COMMAND protobuf::protoc
    ARGS
      "--cpp_out=${PROTO_GEN_DIR}"
      "--grpc_out=${PROTO_GEN_DIR}"
      "--plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}"
      "-I${PROTO_ROOT}"
      "${PROTO_ABS}"
    DEPENDS "${PROTO_ABS}"
    COMMENT "Generating C++ from ${PROTO_REL}"
    VERBATIM
  )

  list(APPEND PROTO_GENERATED_SRCS
    "${OUT_DIR}/${PROTO_NAME}.pb.cc"
    "${OUT_DIR}/${PROTO_NAME}.grpc.pb.cc"
  )
endforeach()

add_library(freezermanager_proto STATIC ${PROTO_GENERATED_SRCS})
add_library(FreezerManager::proto ALIAS freezermanager_proto)

target_include_directories(
  freezermanager_proto
  PUBLIC
    "${PROTO_GEN_DIR}"
)

target_link_libraries(
  freezermanager_proto
  PUBLIC
    gRPC::grpc++
    protobuf::libprotobuf
)

# Suppress warnings from generated proto code.
target_compile_options(
  freezermanager_proto
  PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-w>
)

# ---- RPC middleware (AuthMiddleware) ----

add_library(freezermanager_rpc STATIC AuthMiddleware.cc)
add_library(FreezerManager::rpc ALIAS freezermanager_rpc)

target_include_directories(
  freezermanager_rpc
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(
  freezermanager_rpc
  PUBLIC
    FreezerManager::core
    FreezerManager::auth
    FreezerManager::storage
    FreezerManager::proto
)
