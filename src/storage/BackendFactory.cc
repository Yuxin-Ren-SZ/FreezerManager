// SPDX-License-Identifier: AGPL-3.0-or-later

#include "storage/BackendFactory.h"

#include "storage/postgres/AuditRepositories.h"
#include "storage/postgres/BoxGeometryRepositories.h"
#include "storage/postgres/IdentityRepositories.h"
#include "storage/postgres/ItemTypeRepositories.h"
#include "storage/postgres/LayoutRepositories.h"
#include "storage/postgres/LoginAttemptRepositories.h"
#include "storage/postgres/PostgresBackend.h"
#include "storage/postgres/RoleRepositories.h"
#include "storage/postgres/SampleRepositories.h"
#include "storage/postgres/SessionRepositories.h"
#include "storage/postgres/ShareRequestRepositories.h"
#include "storage/sqlite/AuditRepositories.h"
#include "storage/sqlite/BoxGeometryRepositories.h"
#include "storage/sqlite/IdentityRepositories.h"
#include "storage/sqlite/ItemTypeRepositories.h"
#include "storage/sqlite/LayoutRepositories.h"
#include "storage/sqlite/LoginAttemptRepositories.h"
#include "storage/sqlite/RoleRepositories.h"
#include "storage/sqlite/SampleRepositories.h"
#include "storage/sqlite/SessionRepositories.h"
#include "storage/sqlite/ShareRequestRepositories.h"
#include "storage/sqlite/SqliteBackend.h"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

namespace fmgr::storage {
  namespace {

    void verify_under_data_dir(const std::string& sqlite_path, const std::string& data_dir) {
      namespace fs = std::filesystem;
      std::error_code error;
      const fs::path base = fs::weakly_canonical(fs::path(data_dir), error);
      const fs::path target = fs::weakly_canonical(fs::path(sqlite_path), error);
      if (error) {
        throw BackendOptionError("cannot resolve SQLite path against the data directory");
      }
      const fs::path relative = target.lexically_relative(base);
      if (relative.empty() || *relative.begin() == "..") {
        throw BackendOptionError("SQLite path escapes the configured data directory");
      }
    }

    void register_all_repositories(SqliteBackend& backend) {
      register_identity_repositories(backend);
      register_role_repositories(backend);
      register_layout_repositories(backend);
      register_box_geometry_repositories(backend);
      register_box_repositories(backend);
      register_item_type_repositories(backend);
      register_sample_repositories(backend);
      register_session_repositories(backend);
      register_login_attempt_repositories(backend);
      register_share_request_repositories(backend);
      register_audit_repositories(backend);
    }

    void register_all_repositories(PostgresBackend& backend) {
      register_identity_repositories(backend);
      register_role_repositories(backend);
      register_layout_repositories(backend);
      register_box_geometry_repositories(backend);
      register_box_repositories(backend);
      register_item_type_repositories(backend);
      register_sample_repositories(backend);
      register_session_repositories(backend);
      register_login_attempt_repositories(backend);
      register_share_request_repositories(backend);
      register_audit_repositories(backend);
    }

  } // namespace

  std::unique_ptr<IStorageBackend> open_backend(const BackendOptions& options) {
    if (options.sqlite_path.has_value() == options.postgres_url.has_value()) {
      throw BackendOptionError("exactly one of SQLite path or PostgreSQL URL must be provided");
    }

    if (options.sqlite_path.has_value()) {
      if (options.data_dir.has_value()) {
        verify_under_data_dir(options.sqlite_path.value(), options.data_dir.value());
      }
      auto backend = std::make_unique<SqliteBackend>(
          SqliteBackendOptions{.database_path = options.sqlite_path.value()});
      register_all_repositories(*backend);
      backend->migrate_to_latest();
      return backend;
    }

    if (const char* pg_password = std::getenv("FMGR_PG_PASSWORD");
        pg_password != nullptr && *pg_password != '\0') {
      ::setenv("PGPASSWORD", pg_password, /*overwrite=*/1); // NOLINT(concurrency-mt-unsafe)
    }

    auto backend = std::make_unique<PostgresBackend>(
        PostgresBackendOptions{.connection_string = options.postgres_url.value()});
    register_all_repositories(*backend);
    backend->migrate_to_latest();
    return backend;
  }

} // namespace fmgr::storage
