# Property tests.

find_package(GTest CONFIG REQUIRED)
find_package(rapidcheck CONFIG REQUIRED)

add_executable(freezermanager_audit_property_tests audit_chain_property_test.cpp)
target_link_libraries(
  freezermanager_audit_property_tests
  PRIVATE
    FreezerManager::audit
    rapidcheck::rapidcheck
    GTest::gtest_main
)
target_include_directories(
  freezermanager_audit_property_tests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

add_executable(freezermanager_box_geometry_property_tests box_geometry_property_test.cpp)
target_link_libraries(
  freezermanager_box_geometry_property_tests
  PRIVATE
    FreezerManager::storage_sqlite
    rapidcheck::rapidcheck
    GTest::gtest_main
)
target_include_directories(
  freezermanager_box_geometry_property_tests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

add_executable(freezermanager_retention_property_tests retention_policy_property_test.cpp)
target_link_libraries(
  freezermanager_retention_property_tests
  PRIVATE
    FreezerManager::backup
    rapidcheck::rapidcheck
    GTest::gtest_main
)
target_include_directories(
  freezermanager_retention_property_tests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

include(GoogleTest)
gtest_discover_tests(freezermanager_audit_property_tests)
gtest_discover_tests(freezermanager_box_geometry_property_tests)
gtest_discover_tests(freezermanager_retention_property_tests)
