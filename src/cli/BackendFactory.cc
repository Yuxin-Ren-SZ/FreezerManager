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

#include <memory>

namespace fmgr::cli {

  namespace {

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
      auto backend = std::make_unique<storage::SqliteBackend>(
          storage::SqliteBackendOptions{.database_path = options.sqlite_path.value()});
      register_all_repositories(*backend);
      backend->migrate_to_latest();
      return backend;
    }

    auto backend = std::make_unique<storage::PostgresBackend>(
        storage::PostgresBackendOptions{.connection_string = options.postgres_url.value()});
    register_all_repositories(*backend);
    backend->migrate_to_latest();
    return backend;
  }

} // namespace fmgr::cli
