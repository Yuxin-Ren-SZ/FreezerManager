# Server entrypoint, dependency wiring, and configuration.

find_package(gRPC CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

add_library(freezermanager_server_lib STATIC
  AuthServiceImpl.cc
  SessionServiceImpl.cc
  LabServiceImpl.cc
  BoxServiceImpl.cc
  ItemTypeServiceImpl.cc
  SampleServiceImpl.cc
  RoleServiceImpl.cc
  AuditServiceImpl.cc
  ShareServiceImpl.cc
  BackupScheduler.cc
  FreezerServer.cc
  # CSV export reused from the CLI: ExportSamplesCsv shares the exact column model
  # and chain-of-custody header as `freezerctl sample export`.
  ${PROJECT_SOURCE_DIR}/src/cli/SampleCsv.cc
  ${PROJECT_SOURCE_DIR}/src/cli/CsvWriter.cc
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
    FreezerManager::backup
    FreezerManager::kms
    FreezerManager::crypto
    gRPC::grpc++
    spdlog::spdlog
)

add_executable(freezerd main.cc)
target_link_libraries(freezerd
  PRIVATE
    FreezerManager::server_lib
    FreezerManager::rest
    FreezerManager::storage_sqlite)
