# End-to-end smoke tests.
# Spin up a full FreezerServer on SQLite and exercise complete user journeys
# through the gRPC surface.

find_package(gRPC CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)

add_executable(
  freezermanager_e2e_smoke_tests
  e2e_smoke_test.cpp
)

target_include_directories(
  freezermanager_e2e_smoke_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}/tests
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(
  freezermanager_e2e_smoke_tests
  PRIVATE
    FreezerManager::server_lib
    FreezerManager::storage_sqlite
    FreezerManager::auth
    gRPC::grpc++
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(freezermanager_e2e_smoke_tests
  PROPERTIES LABELS "e2e")
