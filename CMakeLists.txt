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

function(freezermanager_enable_sanitizers target)
  if(FREEZERMANAGER_ENABLE_ASAN)
    target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address)
  endif()

  if(FREEZERMANAGER_ENABLE_UBSAN)
    target_compile_options(${target} PRIVATE -fsanitize=undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=undefined)
  endif()

  if(FREEZERMANAGER_ENABLE_TSAN)
    target_compile_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=thread)
  endif()
endfunction()

include(CTest)

add_subdirectory(src)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
