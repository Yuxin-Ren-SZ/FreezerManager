# Key-management provider interfaces and implementations.

find_package(libsodium REQUIRED)

add_library(
  freezermanager_kms
  STATIC
    EnvVarKms.cc
)
add_library(FreezerManager::kms ALIAS freezermanager_kms)

target_include_directories(
  freezermanager_kms
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(
  freezermanager_kms
  PRIVATE
    libsodium::libsodium
)
