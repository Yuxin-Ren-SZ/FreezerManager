find_package(nlohmann_json CONFIG REQUIRED)
find_package(libsodium REQUIRED)

add_library(freezermanager_core INTERFACE)
add_library(FreezerManager::core ALIAS freezermanager_core)

target_include_directories(
  freezermanager_core
  INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# libsodium is linked publicly because uuid.h's generate_uuid_v4() calls
# randombytes_buf() inline, so every consumer of the header needs the symbol.
target_link_libraries(
  freezermanager_core
  INTERFACE
    nlohmann_json::nlohmann_json
    libsodium::libsodium
)
