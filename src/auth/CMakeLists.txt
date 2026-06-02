# Authentication provider interfaces and implementations.

find_package(libsodium REQUIRED)
find_package(OpenSSL REQUIRED)

add_library(
  freezermanager_auth
  STATIC
    LocalAuthProvider.cc
    Totp.cc
)
add_library(FreezerManager::auth ALIAS freezermanager_auth)

target_include_directories(
  freezermanager_auth
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(
  freezermanager_auth
  PUBLIC
    FreezerManager::core
    FreezerManager::storage
  PRIVATE
    libsodium::libsodium
    OpenSSL::SSL
    OpenSSL::Crypto
)
