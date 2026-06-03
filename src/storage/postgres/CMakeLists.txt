find_package(libpqxx CONFIG REQUIRED)
find_package(libsodium CONFIG REQUIRED)

add_library(
  freezermanager_storage_postgres
  PostgresBackend.cc
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

freezermanager_enable_sanitizers(freezermanager_storage_postgres)
