# freezerctl command-line client.

find_package(CLI11 CONFIG REQUIRED)

# Command logic lives in a library so it can be unit-tested without spawning a
# process; the executable is a thin argv shim over it.
add_library(
  freezermanager_cli
  AuditCommands.cc
  AuditCsv.cc
  BackendFactory.cc
  CliApp.cc
  BoxImport.cc
  CreateCommands.cc
  CsvReader.cc
  CsvWriter.cc
  CustomFieldDefImport.cc
  ItemTypeImport.cc
  KeyCommands.cc
  LabCommands.cc
  NounCommands.cc
  UserImport.cc
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
    FreezerManager::backup
    FreezerManager::storage_sqlite
    FreezerManager::storage_postgres
    FreezerManager::crypto
    FreezerManager::kms
  PRIVATE
    CLI11::CLI11
)

add_executable(freezerctl main.cc)
target_link_libraries(freezerctl PRIVATE FreezerManager::cli)
