find_package(GTest CONFIG REQUIRED)

add_executable(freezermanager_core_unit_tests core_value_types_test.cpp identity_types_test.cpp)
target_link_libraries(
  freezermanager_core_unit_tests
  PRIVATE
    FreezerManager::core
    GTest::gtest_main
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
  sqlite_backend_test.cpp
  sqlite_identity_repository_test.cpp
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
