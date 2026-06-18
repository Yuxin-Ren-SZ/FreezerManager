# Field-level cryptography (envelope encryption of PHI fields).

find_package(libsodium REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(
  freezermanager_crypto
  STATIC
    FieldCipher.cc
)
add_library(FreezerManager::crypto ALIAS freezermanager_crypto)

target_include_directories(
  freezermanager_crypto
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(
  freezermanager_crypto
  PUBLIC
    FreezerManager::kms
    nlohmann_json::nlohmann_json
  PRIVATE
    libsodium::libsodium
)
