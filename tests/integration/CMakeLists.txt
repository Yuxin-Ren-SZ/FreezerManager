# Integration tests.

find_package(gRPC CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)

add_executable(freezermanager_server_integration_tests
  server_integration_test.cpp
)

target_include_directories(
  freezermanager_server_integration_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}/tests
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(
  freezermanager_server_integration_tests
  PRIVATE
    FreezerManager::server_lib
    FreezerManager::storage_sqlite
    FreezerManager::auth
    gRPC::grpc++
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(freezermanager_server_integration_tests)
