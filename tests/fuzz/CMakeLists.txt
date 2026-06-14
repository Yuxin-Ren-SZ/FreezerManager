# Fuzz harnesses.
# Property-based fuzz tests that exercise security-critical paths with
# randomized inputs. These run as regular GoogleTest tests (no libFuzzer
# dependency) and are deterministic-seeded from --gtest_random_seed=.

find_package(GTest CONFIG REQUIRED)

add_executable(
  freezermanager_fuzz_tests
  fuzz_ratelimit_and_validator.cpp
)
target_link_libraries(
  freezermanager_fuzz_tests
  PRIVATE
    FreezerManager::core
    GTest::gtest_main
)
target_include_directories(
  freezermanager_fuzz_tests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

include(GoogleTest)
gtest_discover_tests(freezermanager_fuzz_tests)
