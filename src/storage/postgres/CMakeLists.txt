find_package(libpqxx CONFIG REQUIRED)
find_package(libsodium CONFIG REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Regenerate the embedded Postgres migration header from .sql at configure time
# (see the sibling comment in ../CMakeLists.txt for SQLite).
file(GLOB _fmgr_pg_sql CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/migrations/*.sql)
execute_process(
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_migrations_header.py
          ${CMAKE_CURRENT_SOURCE_DIR}/migrations
          ${CMAKE_CURRENT_SOURCE_DIR}/PostgresMigrationsEmbedded.h
          k_postgres_embedded_migrations
  RESULT_VARIABLE _fmgr_pg_gen_rc)
if(NOT _fmgr_pg_gen_rc EQUAL 0)
  message(FATAL_ERROR "failed to generate PostgresMigrationsEmbedded.h (rc=${_fmgr_pg_gen_rc})")
endif()

add_library(
  freezermanager_storage_postgres
  PostgresBackend.cc
  IdentityRepositories.cc
  RoleRepositories.cc
  LayoutRepositories.cc
  BoxGeometryRepositories.cc
  ItemTypeRepositories.cc
  SampleRepositories.cc
  SessionRepositories.cc
  LoginAttemptRepositories.cc
  ShareRequestRepositories.cc
  AuditRepositories.cc
)
add_library(FreezerManager::storage_postgres ALIAS freezermanager_storage_postgres)

target_include_directories(
  freezermanager_storage_postgres
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(
  freezermanager_storage_postgres
  PUBLIC
    FreezerManager::audit
    FreezerManager::storage
    libpqxx::pqxx
    libsodium::libsodium
)
