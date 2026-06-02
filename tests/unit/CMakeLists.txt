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

add_executable(
  freezermanager_sqlite_storage_unit_tests
  audit_event_test.cpp
  sqlite_backend_test.cpp
  sqlite_box_geometry_repository_test.cpp
  sqlite_box_repository_test.cpp
  sqlite_identity_repository_test.cpp
  sqlite_item_type_repository_test.cpp
  sqlite_layout_repository_test.cpp
  sqlite_role_repository_test.cpp
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

include(GoogleTest)
gtest_discover_tests(freezermanager_core_unit_tests)
gtest_discover_tests(freezermanager_storage_unit_tests)
gtest_discover_tests(freezermanager_sqlite_storage_unit_tests)
