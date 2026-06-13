cmake_minimum_required(VERSION 3.25)

project(
  FreezerManager
  VERSION 0.1.0
  DESCRIPTION "Self-hostable freezer and biospecimen management system"
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FREEZERMANAGER_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(FREEZERMANAGER_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(FREEZERMANAGER_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(FREEZERMANAGER_ENABLE_COVERAGE "Enable code coverage instrumentation (--coverage)" OFF)
option(FREEZERMANAGER_BUILD_BENCHMARKS "Build performance benchmarks (Google Benchmark)" OFF)

if(FREEZERMANAGER_ENABLE_COVERAGE)
  add_compile_options(--coverage)
  add_link_options(--coverage)

  find_program(GCOVR_EXECUTABLE gcovr)
  if(GCOVR_EXECUTABLE)
    set(GCOV_OUTPUT_DIR "${CMAKE_BINARY_DIR}/coverage")

    # Summary report (text).  Runs gcovr from the build directory so that
    # gcov's intermediate *.gcov files land under the build tree instead of
    # polluting the source root.
    add_custom_target(coverage
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${GCOV_OUTPUT_DIR}"
      COMMAND "${GCOVR_EXECUTABLE}"
              -r "${CMAKE_SOURCE_DIR}"
              --object-directory "${CMAKE_BINARY_DIR}"
              --exclude 'tests/'
              --exclude 'out/'
              --exclude '.*proto_generated.*'
              --merge-mode-functions separate
              --print-summary
      WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
      COMMENT "Running gcovr line-coverage summary"
    )

    # HTML report.  Open coverage/index.html in a browser after the build.
    add_custom_target(coverage-html
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${GCOV_OUTPUT_DIR}"
      COMMAND "${GCOVR_EXECUTABLE}"
              -r "${CMAKE_SOURCE_DIR}"
              --object-directory "${CMAKE_BINARY_DIR}"
              --exclude 'tests/'
              --exclude 'out/'
              --exclude '.*proto_generated.*'
              --merge-mode-functions separate
              --html --html-details
              -o "${GCOV_OUTPUT_DIR}/index.html"
      WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
      COMMENT "Generating gcovr HTML coverage report → ${GCOV_OUTPUT_DIR}/index.html"
    )

    # Clean stray *.gcov files that may have leaked into the source tree from
    # ad-hoc gcov / gcovr invocations.
    add_custom_target(coverage-clean
      COMMAND find "${CMAKE_SOURCE_DIR}" -name '*.gcov' -delete
      COMMAND find "${CMAKE_BINARY_DIR}" -name '*.gcda' -delete
      COMMAND find "${CMAKE_BINARY_DIR}" -name '*.gcov' -delete
      COMMENT "Removing stray *.gcov / *.gcda files"
    )
  else()
    message(WARNING "gcovr not found — 'coverage' / 'coverage-html' / 'coverage-clean' targets unavailable")
  endif()
endif()

# Sanitizers are applied globally (compile + link) so every project library and
# every executable that links them stays consistent. Per-target opt-in let
# instrumented libs link into uninstrumented test exes -> undefined __tsan_*/etc.
if(FREEZERMANAGER_ENABLE_ASAN)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
endif()

if(FREEZERMANAGER_ENABLE_UBSAN)
  add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=undefined)
endif()

if(FREEZERMANAGER_ENABLE_TSAN)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()

include(CTest)

add_subdirectory(src)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
