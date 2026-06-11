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

add_executable(freezermanager_lab_service_integration_tests
  lab_service_integration_test.cpp
)

target_include_directories(
  freezermanager_lab_service_integration_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}/tests
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(
  freezermanager_lab_service_integration_tests
  PRIVATE
    FreezerManager::server_lib
    FreezerManager::storage_sqlite
    FreezerManager::auth
    gRPC::grpc++
    GTest::gtest_main
)

add_executable(freezermanager_box_service_integration_tests
  box_service_integration_test.cpp
)

target_include_directories(
  freezermanager_box_service_integration_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}/tests
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(
  freezermanager_box_service_integration_tests
  PRIVATE
    FreezerManager::server_lib
    FreezerManager::storage_sqlite
    FreezerManager::auth
    gRPC::grpc++
    GTest::gtest_main
)

include(GoogleTest)
# These spin up an in-process gRPC server. The Conan gRPC/absl libraries are
# prebuilt WITHOUT sanitizer instrumentation, so under asan/tsan they produce
# library-internal use-after-poison / data-race reports that are not bugs in our
# code (our logic is covered under sanitizers by the unit tests). They are
# labelled "grpc_integration" so CI can exclude them from the asan/tsan runs via
# `ctest --label-exclude grpc_integration`; they still run under dev/release.
gtest_discover_tests(freezermanager_server_integration_tests
  PROPERTIES LABELS "grpc_integration")
gtest_discover_tests(freezermanager_lab_service_integration_tests
  PROPERTIES LABELS "grpc_integration")
gtest_discover_tests(freezermanager_box_service_integration_tests
  PROPERTIES LABELS "grpc_integration")
