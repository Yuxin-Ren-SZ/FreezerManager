find_package(nlohmann_json CONFIG REQUIRED)

add_library(freezermanager_storage INTERFACE)
add_library(FreezerManager::storage ALIAS freezermanager_storage)

target_include_directories(
  freezermanager_storage
  INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(
  freezermanager_storage
  INTERFACE
    FreezerManager::core
    nlohmann_json::nlohmann_json
)
