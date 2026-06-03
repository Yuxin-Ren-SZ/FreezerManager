# gRPC services and REST gateway wiring.

add_library(freezermanager_rpc STATIC AuthMiddleware.cc)
add_library(FreezerManager::rpc ALIAS freezermanager_rpc)

target_include_directories(
  freezermanager_rpc
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(
  freezermanager_rpc
  PUBLIC
    FreezerManager::core
    FreezerManager::auth
    FreezerManager::storage
)
