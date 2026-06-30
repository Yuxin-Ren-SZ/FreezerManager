find_package(GTest CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(
  freezermanager_core_unit_tests
  box_types_test.cpp
  core_value_types_test.cpp
  custom_field_validator_test.cpp
  freezer_types_test.cpp
  identity_types_test.cpp
  json_helpers_test.cpp
  item_types_test.cpp
  permission_catalog_test.cpp
  role_types_test.cpp
  sample_types_test.cpp
  seed_templates_test.cpp
  session_types_test.cpp
  share_request_types_test.cpp
)
target_link_libraries(
  freezermanager_core_unit_tests
  PRIVATE
    FreezerManager::core
    GTest::gtest_main
)
target_include_directories(
  freezermanager_core_unit_tests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)
target_compile_definitions(
  freezermanager_core_unit_tests
  PRIVATE
    FMGR_SEED_DATA_DIR="${CMAKE_SOURCE_DIR}/data/seed"
)

add_executable(freezermanager_storage_unit_tests storage_interface_test.cpp)
target_link_libraries(
  freezermanager_storage_unit_tests
  PRIVATE
    FreezerManager::storage
    GTest::gtest_main
)
target_include_directories(
  freezermanager_storage_unit_tests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

add_executable(
  freezermanager_sqlite_storage_unit_tests
  audit_event_test.cpp
  sqlite_backend_test.cpp
)
target_link_libraries(
  freezermanager_sqlite_storage_unit_tests
  PRIVATE
    FreezerManager::storage_sqlite
    GTest::gtest_main
)
target_include_directories(
  freezermanager_sqlite_storage_unit_tests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

add_executable(
  freezermanager_auth_unit_tests
  auth_types_test.cpp
  totp_test.cpp
  local_auth_provider_test.cpp
)
target_link_libraries(
  freezermanager_auth_unit_tests
  PRIVATE
    FreezerManager::auth
    FreezerManager::storage_sqlite
    GTest::gtest_main
)
target_include_directories(
  freezermanager_auth_unit_tests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

# Domain-repository tests parameterized over both backends (SQLite + Postgres).
# Postgres parameters skip when FMGR_TEST_POSTGRES_URL is unset.
add_executable(
  freezermanager_storage_repository_tests
  audit_repository_test.cpp
  box_geometry_repository_test.cpp
  box_repository_test.cpp
  identity_repository_test.cpp
  item_type_repository_test.cpp
  layout_repository_test.cpp
  sample_repository_test.cpp
  session_repository_test.cpp
  share_request_repository_test.cpp
  role_repository_test.cpp
)
target_link_libraries(
  freezermanager_storage_repository_tests
  PRIVATE
    FreezerManager::storage_sqlite
    FreezerManager::storage_postgres
    GTest::gtest_main
)
target_include_directories(
  freezermanager_storage_repository_tests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

add_executable(freezermanager_rpc_unit_tests auth_middleware_test.cpp rate_limiter_test.cpp)
target_link_libraries(
  freezermanager_rpc_unit_tests
  PRIVATE
    FreezerManager::rpc
    FreezerManager::storage_sqlite
    GTest::gtest_main
)
target_include_directories(
  freezermanager_rpc_unit_tests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

add_executable(freezermanager_audit_unit_tests audit_chain_verifier_test.cpp)
target_link_libraries(
  freezermanager_audit_unit_tests
  PRIVATE
    FreezerManager::audit
    GTest::gtest_main
)
target_include_directories(
  freezermanager_audit_unit_tests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

add_executable(freezermanager_cli_unit_tests cli_test.cpp cli_user_set_password_test.cpp)
target_link_libraries(
  freezermanager_cli_unit_tests
  PRIVATE
    FreezerManager::cli
    FreezerManager::auth
    FreezerManager::storage_sqlite
    GTest::gtest_main
)
target_include_directories(
  freezermanager_cli_unit_tests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

add_executable(freezermanager_custom_field_resolver_tests custom_field_resolver_test.cpp)
target_link_libraries(
  freezermanager_custom_field_resolver_tests
  PRIVATE
    FreezerManager::storage_sqlite
    GTest::gtest_main
)
target_include_directories(
  freezermanager_custom_field_resolver_tests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

add_executable(freezermanager_error_translation_tests error_translation_test.cpp)
target_link_libraries(
  freezermanager_error_translation_tests
  PRIVATE
    FreezerManager::server_lib
    FreezerManager::storage
    GTest::gtest_main
)
target_include_directories(
  freezermanager_error_translation_tests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

add_executable(freezermanager_obs_unit_tests obs_log_test.cpp)
target_link_libraries(
  freezermanager_obs_unit_tests
  PRIVATE
    FreezerManager::obs
    GTest::gtest_main
)
target_include_directories(
  freezermanager_obs_unit_tests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

add_executable(freezermanager_transaction_resilience_tests transaction_resilience_test.cpp)
target_link_libraries(
  freezermanager_transaction_resilience_tests
  PRIVATE
    FreezerManager::storage_sqlite
    GTest::gtest_main
)
target_include_directories(
  freezermanager_transaction_resilience_tests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

add_executable(freezermanager_rest_unit_tests
  json_proto_mapping_test.cpp
  rest_error_translation_test.cpp
)
target_link_libraries(
  freezermanager_rest_unit_tests
  PRIVATE
    FreezerManager::rest
    nlohmann_json::nlohmann_json
    GTest::gtest_main
)
target_include_directories(
  freezermanager_rest_unit_tests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

add_executable(freezermanager_kms_unit_tests kms_test.cpp os_keyring_test.cpp)
target_link_libraries(
  freezermanager_kms_unit_tests
  PRIVATE
    FreezerManager::kms
    GTest::gtest_main
)
target_include_directories(
  freezermanager_kms_unit_tests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

add_executable(freezermanager_crypto_unit_tests field_cipher_test.cpp file_cipher_test.cpp)
target_link_libraries(
  freezermanager_crypto_unit_tests
  PRIVATE
    FreezerManager::crypto
    FreezerManager::kms
    nlohmann_json::nlohmann_json
    GTest::gtest_main
)
target_include_directories(
  freezermanager_crypto_unit_tests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

add_executable(freezermanager_backup_unit_tests
  backup_filename_test.cpp
  retention_policy_test.cpp
  backup_runner_test.cpp
)
target_link_libraries(
  freezermanager_backup_unit_tests
  PRIVATE
    FreezerManager::backup
    FreezerManager::storage_sqlite
    FreezerManager::kms
    GTest::gtest_main
)
target_include_directories(
  freezermanager_backup_unit_tests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

add_executable(freezermanager_backup_scheduler_tests backup_scheduler_test.cpp)
target_link_libraries(
  freezermanager_backup_scheduler_tests
  PRIVATE
    FreezerManager::server_lib
    FreezerManager::storage_sqlite
    FreezerManager::kms
    GTest::gtest_main
)
target_include_directories(
  freezermanager_backup_scheduler_tests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

# Qt desktop-client unit tests. Only built when the Qt client itself was built
# (the FreezerManager::qt_lib alias exists). Headless: no QApplication, no
# DISPLAY — ConfigManager uses an isolated INI file and GrpcChannel connects
# lazily, so neither needs a GUI or a live server.
if(TARGET FreezerManager::qt_lib)
  add_executable(
    freezermanager_qt_unit_tests
    qt_auth_service_client_test.cpp
    qt_barcode_scan_controller_test.cpp
    qt_box_grid_model_test.cpp
    qt_box_service_client_test.cpp
    qt_config_manager_test.cpp
    qt_grpc_channel_test.cpp
    qt_lab_service_client_test.cpp
    qt_lab_tree_model_test.cpp
    qt_sample_service_client_test.cpp
    qt_sample_table_model_test.cpp
    qt_sample_watch_subscriber_test.cpp
    qt_session_manager_test.cpp
  )
  target_link_libraries(
    freezermanager_qt_unit_tests
    PRIVATE
      FreezerManager::qt_lib
      GTest::gtest_main
  )
  target_include_directories(
    freezermanager_qt_unit_tests
    PRIVATE
      ${CMAKE_SOURCE_DIR}/tests
  )
endif()

include(GoogleTest)
gtest_discover_tests(freezermanager_core_unit_tests)
gtest_discover_tests(freezermanager_backup_unit_tests)
gtest_discover_tests(freezermanager_backup_scheduler_tests)
if(TARGET freezermanager_qt_unit_tests)
  gtest_discover_tests(freezermanager_qt_unit_tests)
endif()
gtest_discover_tests(freezermanager_kms_unit_tests)
gtest_discover_tests(freezermanager_crypto_unit_tests)
gtest_discover_tests(freezermanager_rest_unit_tests)
gtest_discover_tests(freezermanager_storage_unit_tests)
gtest_discover_tests(freezermanager_sqlite_storage_unit_tests)
gtest_discover_tests(freezermanager_auth_unit_tests)
gtest_discover_tests(freezermanager_rpc_unit_tests)
gtest_discover_tests(freezermanager_audit_unit_tests)
gtest_discover_tests(freezermanager_custom_field_resolver_tests)
gtest_discover_tests(freezermanager_error_translation_tests
  PROPERTIES LABELS "grpc_integration")
gtest_discover_tests(freezermanager_obs_unit_tests)
gtest_discover_tests(freezermanager_transaction_resilience_tests)
# The Postgres parameter of these tests DROPs/CREATEs the shared public schema;
# serialize against every other Postgres-touching suite with one resource lock.
gtest_discover_tests(freezermanager_storage_repository_tests
    PROPERTIES RESOURCE_LOCK fmgr_postgres_test_db)
# The Postgres parameter of the CLI tests also DROP/CREATEs the shared public
# schema; share the same lock so it never races the repository suite.
gtest_discover_tests(freezermanager_cli_unit_tests
    PROPERTIES RESOURCE_LOCK fmgr_postgres_test_db)
