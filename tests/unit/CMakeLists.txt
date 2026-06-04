find_package(GTest CONFIG REQUIRED)

add_executable(
  freezermanager_core_unit_tests
  box_types_test.cpp
  core_value_types_test.cpp
  custom_field_validator_test.cpp
  freezer_types_test.cpp
  identity_types_test.cpp
  item_types_test.cpp
  permission_catalog_test.cpp
  role_types_test.cpp
  sample_types_test.cpp
  seed_templates_test.cpp
  session_types_test.cpp
  share_request_types_test.cpp
)
target_link_libraries(
  freezermanager_core_unit_tests
  PRIVATE
    FreezerManager::core
    GTest::gtest_main
)
target_include_directories(
  freezermanager_core_unit_tests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)
target_compile_definitions(
  freezermanager_core_unit_tests
  PRIVATE
    FMGR_SEED_DATA_DIR="${CMAKE_SOURCE_DIR}/data/seed"
)

add_executable(freezermanager_storage_unit_tests storage_interface_test.cpp)
target_link_libraries(
  freezermanager_storage_unit_tests
  PRIVATE
    FreezerManager::storage
    GTest::gtest_main
)
target_include_directories(
  freezermanager_storage_unit_tests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

add_executable(
  freezermanager_sqlite_storage_unit_tests
  audit_event_test.cpp
  sqlite_backend_test.cpp
  sqlite_box_geometry_repository_test.cpp
  sqlite_box_repository_test.cpp
  sqlite_item_type_repository_test.cpp
  sqlite_sample_repository_test.cpp
  sqlite_session_repository_test.cpp
  sqlite_share_request_repository_test.cpp
)
target_link_libraries(
  freezermanager_sqlite_storage_unit_tests
  PRIVATE
    FreezerManager::storage_sqlite
    GTest::gtest_main
)
target_include_directories(
  freezermanager_sqlite_storage_unit_tests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

add_executable(
  freezermanager_auth_unit_tests
  auth_types_test.cpp
  totp_test.cpp
  local_auth_provider_test.cpp
)
target_link_libraries(
  freezermanager_auth_unit_tests
  PRIVATE
    FreezerManager::auth
    FreezerManager::storage_sqlite
    GTest::gtest_main
)
target_include_directories(
  freezermanager_auth_unit_tests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

# Domain-repository tests parameterized over both backends (SQLite + Postgres).
# Postgres parameters skip when FMGR_TEST_POSTGRES_URL is unset.
add_executable(
  freezermanager_storage_repository_tests
  identity_repository_test.cpp
  layout_repository_test.cpp
  role_repository_test.cpp
)
target_link_libraries(
  freezermanager_storage_repository_tests
  PRIVATE
    FreezerManager::storage_sqlite
    FreezerManager::storage_postgres
    GTest::gtest_main
)
target_include_directories(
  freezermanager_storage_repository_tests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

add_executable(freezermanager_rpc_unit_tests auth_middleware_test.cpp)
target_link_libraries(
  freezermanager_rpc_unit_tests
  PRIVATE
    FreezerManager::rpc
    FreezerManager::storage_sqlite
    GTest::gtest_main
)
target_include_directories(
  freezermanager_rpc_unit_tests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

include(GoogleTest)
gtest_discover_tests(freezermanager_core_unit_tests)
gtest_discover_tests(freezermanager_storage_unit_tests)
gtest_discover_tests(freezermanager_sqlite_storage_unit_tests)
gtest_discover_tests(freezermanager_auth_unit_tests)
gtest_discover_tests(freezermanager_rpc_unit_tests)
# The Postgres parameter of these tests DROPs/CREATEs the shared public schema;
# serialize against every other Postgres-touching suite with one resource lock.
gtest_discover_tests(freezermanager_storage_repository_tests
    PROPERTIES RESOURCE_LOCK fmgr_postgres_test_db)
