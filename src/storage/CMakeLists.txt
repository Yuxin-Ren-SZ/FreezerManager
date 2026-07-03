find_package(nlohmann_json CONFIG REQUIRED)
find_package(SQLite3 CONFIG REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Regenerate the build-time-embedded migration header from the .sql files at
# configure time. CONFIGURE_DEPENDS re-runs configure when a .sql file changes,
# so the committed header stays in sync; a unit test also asserts the match.
set(_fmgr_gen_migrations ${CMAKE_SOURCE_DIR}/tools/gen_migrations_header.py)
file(GLOB _fmgr_sqlite_sql CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/sqlite/migrations/*.sql)
execute_process(
  COMMAND ${Python3_EXECUTABLE} ${_fmgr_gen_migrations}
          ${CMAKE_CURRENT_SOURCE_DIR}/sqlite/migrations
          ${CMAKE_CURRENT_SOURCE_DIR}/sqlite/SqliteMigrationsEmbedded.h
          k_sqlite_embedded_migrations
  RESULT_VARIABLE _fmgr_gen_rc)
if(NOT _fmgr_gen_rc EQUAL 0)
  message(FATAL_ERROR "failed to generate SqliteMigrationsEmbedded.h (rc=${_fmgr_gen_rc})")
endif()

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
  sqlite/LoginAttemptRepositories.cc
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

add_library(freezermanager_storage_backend_factory STATIC BackendFactory.cc)
add_library(FreezerManager::storage_backend_factory ALIAS freezermanager_storage_backend_factory)

target_include_directories(
  freezermanager_storage_backend_factory
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(
  freezermanager_storage_backend_factory
  PUBLIC
    FreezerManager::storage_sqlite
    FreezerManager::storage_postgres
)
