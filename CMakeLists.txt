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
