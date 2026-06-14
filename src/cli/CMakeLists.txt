# freezerctl command-line client.

find_package(CLI11 CONFIG REQUIRED)

# Command logic lives in a library so it can be unit-tested without spawning a
# process; the executable is a thin argv shim over it.
add_library(
  freezermanager_cli
  AuditCommands.cc
  BackendFactory.cc
  CliApp.cc
  CsvReader.cc
  CsvWriter.cc
  LabCommands.cc
  NounCommands.cc
  SampleCommands.cc
  SampleCsv.cc
  SampleImport.cc
  SampleQuery.cc
)
add_library(FreezerManager::cli ALIAS freezermanager_cli)

target_include_directories(
  freezermanager_cli
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(
  freezermanager_cli
  PUBLIC
    FreezerManager::core
    FreezerManager::audit
    FreezerManager::storage_sqlite
    FreezerManager::storage_postgres
  PRIVATE
    CLI11::CLI11
)

add_executable(freezerctl main.cc)
target_link_libraries(freezerctl PRIVATE FreezerManager::cli)
