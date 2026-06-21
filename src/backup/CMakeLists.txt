# Encrypted-backup engines, retention policy, and the scheduled-backup tick.
# Shared by the freezerctl CLI and the in-server BackupScheduler so backup
# behavior is defined in exactly one place.

find_package(nlohmann_json CONFIG REQUIRED)
find_package(libpqxx CONFIG REQUIRED)
find_package(PostgreSQL REQUIRED) # libpq, for PQconninfoParse in PostgresDump

add_library(
  freezermanager_backup
  STATIC
    BackupCommands.cc
    BackupFilename.cc
    BackupRunner.cc
    PostgresDump.cc
    RetentionPolicy.cc
    SqliteBackup.cc
)
add_library(FreezerManager::backup ALIAS freezermanager_backup)

target_include_directories(
  freezermanager_backup
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(
  freezermanager_backup
  PUBLIC
    FreezerManager::core
    FreezerManager::audit
    FreezerManager::storage
    FreezerManager::storage_sqlite
    FreezerManager::storage_postgres
    FreezerManager::crypto
    FreezerManager::kms
    nlohmann_json::nlohmann_json
    libpqxx::pqxx
    PostgreSQL::PostgreSQL
)
