# REST/JSON gateway: HTTP front door that forwards to the gRPC services over the
# server's in-process channel.

find_package(Drogon CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(freezermanager_rest STATIC
  JsonProtoMapping.cc
  RestGateway.cc
)
add_library(FreezerManager::rest ALIAS freezermanager_rest)

target_include_directories(
  freezermanager_rest
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(
  freezermanager_rest
  PUBLIC
    FreezerManager::proto
    Drogon::Drogon
    gRPC::grpc++
  PRIVATE
    nlohmann_json::nlohmann_json
)
