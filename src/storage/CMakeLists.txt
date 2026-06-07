find_package(nlohmann_json CONFIG REQUIRED)
find_package(SQLite3 CONFIG REQUIRED)

add_library(freezermanager_storage INTERFACE)
add_library(FreezerManager::storage ALIAS freezermanager_storage)

target_include_directories(
  freezermanager_storage
  INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(
  freezermanager_storage
  INTERFACE
    FreezerManager::core
    nlohmann_json::nlohmann_json
)

add_library(
  freezermanager_storage_sqlite
  sqlite/AuditRepositories.cc
  sqlite/BoxGeometryRepositories.cc
  sqlite/IdentityRepositories.cc
  sqlite/ItemTypeRepositories.cc
  sqlite/LayoutRepositories.cc
  sqlite/RoleRepositories.cc
  sqlite/SampleRepositories.cc
  sqlite/SessionRepositories.cc
  sqlite/ShareRequestRepositories.cc
  sqlite/SqliteBackend.cc
)
add_library(FreezerManager::storage_sqlite ALIAS freezermanager_storage_sqlite)

target_include_directories(
  freezermanager_storage_sqlite
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(
  freezermanager_storage_sqlite
  PUBLIC
    FreezerManager::audit
    FreezerManager::storage
    SQLite::SQLite3
)

add_subdirectory(postgres)
