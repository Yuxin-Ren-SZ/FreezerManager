// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cli/BackendFactory.h"

#include "storage/postgres/AuditRepositories.h"
#include "storage/postgres/BoxGeometryRepositories.h"
#include "storage/postgres/IdentityRepositories.h"
#include "storage/postgres/ItemTypeRepositories.h"
#include "storage/postgres/LayoutRepositories.h"
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
#include "storage/sqlite/RoleRepositories.h"
#include "storage/sqlite/SampleRepositories.h"
#include "storage/sqlite/SessionRepositories.h"
#include "storage/sqlite/ShareRequestRepositories.h"
#include "storage/sqlite/SqliteBackend.h"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

namespace fmgr::cli {

  namespace {

    // Reject a SQLite path that escapes `data_dir` via `..` or symlink traversal
    // (review F-10 / security-audit M-3). weakly_canonical resolves `..` without
    // requiring the file to exist yet (the DB may be created on first open).
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void verify_under_data_dir(const std::string& sqlite_path, const std::string& data_dir) {
      namespace fs = std::filesystem;
      std::error_code error;
      const fs::path base = fs::weakly_canonical(fs::path(data_dir), error);
      const fs::path target = fs::weakly_canonical(fs::path(sqlite_path), error);
      if (error) {
        throw BackendOptionError("cannot resolve SQLite path against the data directory");
      }
      // lexically_relative yields a path starting with ".." when target is not
      // under base; an empty result means unrelated roots.
      const fs::path relative = target.lexically_relative(base);
      if (relative.empty() || *relative.begin() == "..") {
        throw BackendOptionError("SQLite path escapes the configured data directory");
      }
    }

    template <typename Backend> void register_all_repositories(Backend& backend) {
      storage::register_identity_repositories(backend);
      storage::register_role_repositories(backend);
      storage::register_layout_repositories(backend);
      storage::register_box_geometry_repositories(backend);
      storage::register_box_repositories(backend);
      storage::register_item_type_repositories(backend);
      storage::register_sample_repositories(backend);
      storage::register_session_repositories(backend);
      storage::register_share_request_repositories(backend);
      storage::register_audit_repositories(backend);
    }

  } // namespace

  std::unique_ptr<storage::IStorageBackend> open_backend(const BackendOptions& options) {
    if (options.sqlite_path.has_value() == options.postgres_url.has_value()) {
      throw BackendOptionError(
          "exactly one of --sqlite <path> or --postgres <url> must be provided");
    }

    if (options.sqlite_path.has_value()) {
      if (options.data_dir.has_value()) {
        verify_under_data_dir(options.sqlite_path.value(), options.data_dir.value());
      }
      auto backend = std::make_unique<storage::SqliteBackend>(
          storage::SqliteBackendOptions{.database_path = options.sqlite_path.value()});
      register_all_repositories(*backend);
      backend->migrate_to_latest();
      return backend;
    }

    // Prefer a password supplied out-of-band via FMGR_PG_PASSWORD over one
    // embedded in the URL (review F-12). Bridge it to libpq's standard
    // PGPASSWORD, which PostgresBackend (via libpqxx/libpq) reads automatically.
    // open_backend runs once at startup on the main thread, so setenv is safe.
    if (const char* pg_password = std::getenv("FMGR_PG_PASSWORD"); // NOLINT(concurrency-mt-unsafe)
        pg_password != nullptr && *pg_password != '\0') {
      ::setenv("PGPASSWORD", pg_password, /*overwrite=*/1); // NOLINT(concurrency-mt-unsafe)
    }

    auto backend = std::make_unique<storage::PostgresBackend>(
        storage::PostgresBackendOptions{.connection_string = options.postgres_url.value()});
    register_all_repositories(*backend);
    backend->migrate_to_latest();
    return backend;
  }

} // namespace fmgr::cli
