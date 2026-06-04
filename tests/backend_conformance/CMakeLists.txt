find_package(GTest CONFIG REQUIRED)

add_executable(freezermanager_backend_conformance_tests storage_backend_conformance_test.cpp)
target_link_libraries(
  freezermanager_backend_conformance_tests
  PRIVATE
    FreezerManager::storage
    GTest::gtest_main
)
target_include_directories(
  freezermanager_backend_conformance_tests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

add_executable(freezermanager_sqlite_backend_conformance_tests sqlite_backend_conformance_test.cpp)
target_link_libraries(
  freezermanager_sqlite_backend_conformance_tests
  PRIVATE
    FreezerManager::storage_sqlite
    GTest::gtest_main
)
target_include_directories(
  freezermanager_sqlite_backend_conformance_tests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

add_executable(
  freezermanager_postgres_backend_conformance_tests
  postgres_backend_conformance_test.cpp
)
target_link_libraries(
  freezermanager_postgres_backend_conformance_tests
  PRIVATE
    FreezerManager::storage_postgres
    FreezerManager::rpc
    GTest::gtest_main
)
target_include_directories(
  freezermanager_postgres_backend_conformance_tests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

include(GoogleTest)
gtest_discover_tests(freezermanager_backend_conformance_tests)
gtest_discover_tests(freezermanager_sqlite_backend_conformance_tests)
# All Postgres conformance tests share one DB; serialize them with a resource lock
# so ctest -j N doesn't run two fixtures that DROP SCHEMA in parallel.
gtest_discover_tests(freezermanager_postgres_backend_conformance_tests
    PROPERTIES RESOURCE_LOCK fmgr_postgres_test_db)
