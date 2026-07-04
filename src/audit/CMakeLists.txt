find_package(libsodium CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(freezermanager_audit AuditChainVerifier.cc AuditEventContent.cc CanonicalJson.cc)
add_library(FreezerManager::audit ALIAS freezermanager_audit)

target_include_directories(
  freezermanager_audit
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(
  freezermanager_audit
  PUBLIC
    FreezerManager::core
    nlohmann_json::nlohmann_json
    libsodium::libsodium
)
