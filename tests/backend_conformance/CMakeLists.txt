find_package(GTest CONFIG REQUIRED)

add_executable(freezermanager_backend_conformance_tests storage_backend_conformance_test.cpp)
target_link_libraries(
  freezermanager_backend_conformance_tests
  PRIVATE
    FreezerManager::storage
    GTest::gtest_main
)

add_executable(freezermanager_sqlite_backend_conformance_tests sqlite_backend_conformance_test.cpp)
target_link_libraries(
  freezermanager_sqlite_backend_conformance_tests
  PRIVATE
    FreezerManager::storage_sqlite
    GTest::gtest_main
)

add_executable(
  freezermanager_postgres_backend_conformance_tests
  postgres_backend_conformance_test.cpp
)
target_link_libraries(
  freezermanager_postgres_backend_conformance_tests
  PRIVATE
    FreezerManager::storage_postgres
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(freezermanager_backend_conformance_tests)
gtest_discover_tests(freezermanager_sqlite_backend_conformance_tests)
gtest_discover_tests(freezermanager_postgres_backend_conformance_tests)
