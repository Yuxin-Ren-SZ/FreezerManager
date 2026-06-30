# Observability: structured logging, health probes, metrics (PRD §17).

find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)

add_library(freezermanager_obs STATIC Log.cc Health.cc Metrics.cc)
add_library(FreezerManager::obs ALIAS freezermanager_obs)

target_include_directories(
  freezermanager_obs
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(
  freezermanager_obs
  PUBLIC
    spdlog::spdlog
    nlohmann_json::nlohmann_json
  PRIVATE
    fmt::fmt
)
