find_package(GTest CONFIG REQUIRED)

add_executable(freezermanager_backend_conformance_tests storage_backend_conformance_test.cpp)
target_link_libraries(
  freezermanager_backend_conformance_tests
  PRIVATE
    FreezerManager::storage
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(freezermanager_backend_conformance_tests)
