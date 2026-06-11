# Server entrypoint, dependency wiring, and configuration.

find_package(gRPC CONFIG REQUIRED)

add_library(freezermanager_server_lib STATIC
  AuthServiceImpl.cc
  SessionServiceImpl.cc
  LabServiceImpl.cc
  BoxServiceImpl.cc
  FreezerServer.cc
  UnimplementedStubs.cc
)
add_library(FreezerManager::server_lib ALIAS freezermanager_server_lib)

target_include_directories(
  freezermanager_server_lib
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(
  freezermanager_server_lib
  PUBLIC
    FreezerManager::proto
    FreezerManager::rpc
    FreezerManager::auth
    FreezerManager::storage
    gRPC::grpc++
)

add_executable(freezerd main.cc)
target_link_libraries(freezerd PRIVATE FreezerManager::server_lib FreezerManager::storage_sqlite)
