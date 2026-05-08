find_package(nlohmann_json CONFIG REQUIRED)

add_library(freezermanager_core INTERFACE)
add_library(FreezerManager::core ALIAS freezermanager_core)

target_include_directories(
  freezermanager_core
  INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(freezermanager_core INTERFACE nlohmann_json::nlohmann_json)
