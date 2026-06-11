// SPDX-License-Identifier: AGPL-3.0-or-later

#include "storage/sqlite/SqliteBackend.h"

#include "audit/CanonicalJson.h"
#include "core/timestamp.h"

#include <sodium.h>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fmgr::storage {
  namespace {

    constexpr std::string_view k_in_memory_path = ":memory:";

    [[nodiscard]] std::string sqlite_message(sqlite3* handle) {
      if (handle == nullptr) {
        return "sqlite handle is null";
      }
      return sqlite3_errmsg(handle);
    }

    [[noreturn]] void throw_sqlite_error(int code, sqlite3* handle, std::string_view action) {
      const auto extended_code = sqlite3_extended_errcode(handle);
      const auto effective_code = extended_code == SQLITE_OK ? code : extended_code;
      const std::string message = std::string(action) + ": " + sqlite_message(handle);
      switch (effective_code) {
      case SQLITE_CONSTRAINT_UNIQUE:
      case SQLITE_CONSTRAINT_PRIMARYKEY:
        throw UniqueViolation(message);
      case SQLITE_CONSTRAINT_FOREIGNKEY:
        throw ForeignKeyViolation(message);
      case SQLITE_BUSY:
      case SQLITE_LOCKED:
        throw Unavailable(message);
      case SQLITE_CONSTRAINT:
      case SQLITE_CONSTRAINT_CHECK:
      case SQLITE_CONSTRAINT_NOTNULL:
        throw ConstraintViolation(message);
      default:
        throw BackendError(BackendErrorCode::ConstraintViolation, message);
      }
    }

    class SqliteConnection {
    public:
      SqliteConnection(const std::string& database_path, std::chrono::milliseconds busy_timeout) {
        constexpr int flags =
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX;
        sqlite3* raw_handle = nullptr;
        const auto open_result =
            sqlite3_open_v2(database_path.c_str(), &raw_handle, flags, nullptr);
        handle_.reset(raw_handle);
        if (open_result != SQLITE_OK) {
          throw_sqlite_error(open_result, handle_.get(), "open sqlite database");
        }
        sqlite3_extended_result_codes(handle_.get(), 1);

        exec("PRAGMA foreign_keys = ON");
        exec("PRAGMA busy_timeout = " + std::to_string(busy_timeout.count()));
        if (database_path != k_in_memory_path && !database_path.starts_with("file:fmgr-memory-")) {
          exec("PRAGMA journal_mode = WAL");
        }
        require_json1();
      }

      [[nodiscard]] sqlite3* get() const {
        return handle_.get();
      }

      void exec(const std::string& sql) const {
        char* error_message = nullptr;
        const auto result =
            sqlite3_exec(handle_.get(), sql.c_str(), nullptr, nullptr, &error_message);
        if (result != SQLITE_OK) {
          std::string message;
          if (error_message != nullptr) {
            message = error_message;
            sqlite3_free(error_message);
          } else {
            message = sqlite_message(handle_.get());
          }
          throw_sqlite_error(result, handle_.get(), message);
        }
      }

    private:
      struct Deleter {
        void operator()(sqlite3* handle) const {
          if (handle != nullptr) {
            sqlite3_close(handle);
          }
        }
      };

      void require_json1() const {
        sqlite3_stmt* raw_statement = nullptr;
        const auto* const sql = "SELECT json_extract('{\"a\":1}', '$.a')";
        const auto prepare_result =
            sqlite3_prepare_v2(handle_.get(), sql, -1, &raw_statement, nullptr);
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw_statement,
                                                                             sqlite3_finalize);
        if (prepare_result != SQLITE_OK) {
          throw UnsupportedOperation("sqlite JSON1 extension is required");
        }
        const auto step_result = sqlite3_step(statement.get());
        if (step_result != SQLITE_ROW || sqlite3_column_int64(statement.get(), 0) != 1) {
          throw UnsupportedOperation("sqlite JSON1 extension is required");
        }
      }

      std::unique_ptr<sqlite3, Deleter> handle_;
    };

    [[nodiscard]] std::string default_memory_uri() {
      static std::mutex mutex;
      static std::uint64_t counter = 0;
      std::scoped_lock lock(mutex);
      ++counter;
      return "file:fmgr-memory-" + std::to_string(counter) + "?mode=memory&cache=shared";
    }

    [[nodiscard]] std::vector<SqliteMigration> default_migrations() {
      return {
          SqliteMigration{
              .version = 1,
              .name = "0001_init",
              .up_sql = R"sql(
CREATE TABLE IF NOT EXISTS audit_events (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  entity_name TEXT NOT NULL,
  entity_id TEXT NOT NULL,
  actor_user_id TEXT NOT NULL,
  actor_session_id TEXT NOT NULL,
  request_id TEXT NOT NULL,
  reason TEXT NOT NULL
);
)sql",
          },
          SqliteMigration{
              .version = 2,
              .name = "0002_identity",
              .up_sql = R"sql(
CREATE TABLE IF NOT EXISTS labs (
  id TEXT PRIMARY KEY,
  name TEXT NOT NULL CHECK (length(name) > 0),
  contact TEXT NOT NULL,
  created_at_micros INTEGER NOT NULL,
  settings_json TEXT NOT NULL CHECK (json_valid(settings_json)),
  is_phi_enabled INTEGER NOT NULL CHECK (is_phi_enabled IN (0, 1)),
  archived_at_micros INTEGER
);

CREATE TABLE IF NOT EXISTS users (
  id TEXT PRIMARY KEY,
  primary_email TEXT NOT NULL CHECK (length(primary_email) > 2),
  display_name TEXT NOT NULL CHECK (length(display_name) > 0),
  status TEXT NOT NULL CHECK (status IN ('active', 'disabled')),
  created_at_micros INTEGER NOT NULL,
  auth_bindings_json TEXT NOT NULL CHECK (json_valid(auth_bindings_json)),
  totp_secret_enc TEXT,
  default_lab_id TEXT REFERENCES labs(id) DEFERRABLE INITIALLY DEFERRED
);

CREATE UNIQUE INDEX IF NOT EXISTS users_primary_email_lower_unique
  ON users(lower(primary_email));

CREATE TABLE IF NOT EXISTS lab_memberships (
  user_id TEXT NOT NULL REFERENCES users(id) DEFERRABLE INITIALLY DEFERRED,
  lab_id TEXT NOT NULL REFERENCES labs(id) DEFERRABLE INITIALLY DEFERRED,
  scope_filters_json TEXT NOT NULL CHECK (json_valid(scope_filters_json)),
  invited_by TEXT REFERENCES users(id) DEFERRABLE INITIALLY DEFERRED,
  joined_at_micros INTEGER NOT NULL,
  revoked_at_micros INTEGER,
  PRIMARY KEY (user_id, lab_id)
);
)sql",
          },
          SqliteMigration{
              .version = 3,
              .name = "0003_roles",
              .up_sql = R"sql(
CREATE TABLE IF NOT EXISTS permissions (
  key TEXT PRIMARY KEY,
  description TEXT NOT NULL CHECK (length(description) > 0)
);

CREATE TABLE IF NOT EXISTS roles (
  id TEXT PRIMARY KEY,
  lab_id TEXT REFERENCES labs(id) DEFERRABLE INITIALLY DEFERRED,
  kind TEXT NOT NULL CHECK (kind IN
    ('system_admin', 'lab_admin', 'member', 'read_only', 'api_client', 'custom')),
  name TEXT NOT NULL CHECK (length(name) > 0),
  description TEXT NOT NULL,
  is_builtin INTEGER NOT NULL CHECK (is_builtin IN (0, 1)),
  created_at_micros INTEGER NOT NULL,
  archived_at_micros INTEGER
);

CREATE UNIQUE INDEX IF NOT EXISTS roles_builtin_name_unique
  ON roles(name) WHERE lab_id IS NULL;
CREATE UNIQUE INDEX IF NOT EXISTS roles_lab_name_unique
  ON roles(lab_id, name) WHERE lab_id IS NOT NULL;

CREATE TABLE IF NOT EXISTS role_permissions (
  role_id TEXT NOT NULL REFERENCES roles(id) DEFERRABLE INITIALLY DEFERRED,
  permission_key TEXT NOT NULL REFERENCES permissions(key) DEFERRABLE INITIALLY DEFERRED,
  PRIMARY KEY (role_id, permission_key)
);

ALTER TABLE lab_memberships
  ADD COLUMN role_id TEXT REFERENCES roles(id) DEFERRABLE INITIALLY DEFERRED;

INSERT INTO permissions (key, description) VALUES
  ('sample.read', 'Read sample records.'),
  ('sample.write', 'Create or modify samples.'),
  ('sample.checkout', 'Check samples in or out.'),
  ('sample.delete_soft', 'Tombstone a sample.'),
  ('sample.delete_hard', 'Permanently delete tombstoned sample (SystemAdmin only).'),
  ('box.configure', 'Define or modify boxes and box types.'),
  ('freezer.configure', 'Configure freezers and storage layout.'),
  ('custom_field.define', 'Define custom field schemas.'),
  ('item_type.define', 'Define item-type taxonomy.'),
  ('user.invite', 'Invite or revoke lab members.'),
  ('user.manage_roles', 'Assign roles to lab members.'),
  ('audit.read', 'Browse audit log entries.'),
  ('audit.export', 'Export signed audit log files.'),
  ('backup.run', 'Trigger a backup or restore-drill.'),
  ('share.request', 'Open a cross-lab share request.'),
  ('share.approve', 'Approve or reject a share request.'),
  ('phi.read', 'Read PHI-tagged custom fields.'),
  ('lab.configure', 'Modify lab settings.'),
  ('lab.enable_phi', 'Toggle PHI mode on a lab.'),
  ('key.rotate', 'Rotate cryptographic keys.'),
  ('session.revoke', 'Revoke another user''s sessions.');

INSERT INTO roles (id, lab_id, kind, name, description, is_builtin, created_at_micros) VALUES
  ('00000000-0000-0000-0000-000000000001', NULL, 'system_admin', 'SystemAdmin',
   'Deployment-wide administrator (PHI access requires separate grant).', 1, 0),
  ('00000000-0000-0000-0000-000000000002', NULL, 'lab_admin', 'LabAdmin',
   'Full administrative control of one lab.', 1, 0),
  ('00000000-0000-0000-0000-000000000003', NULL, 'member', 'Member',
   'Read+write samples within scoped freezers/projects.', 1, 0),
  ('00000000-0000-0000-0000-000000000004', NULL, 'read_only', 'ReadOnly',
   'Read-only access; cannot check out.', 1, 0),
  ('00000000-0000-0000-0000-000000000005', NULL, 'api_client', 'ApiClient',
   'Conservative machine-client default; expand via custom roles.', 1, 0);

INSERT INTO role_permissions (role_id, permission_key) VALUES
  ('00000000-0000-0000-0000-000000000001', 'sample.read'),
  ('00000000-0000-0000-0000-000000000001', 'sample.write'),
  ('00000000-0000-0000-0000-000000000001', 'sample.checkout'),
  ('00000000-0000-0000-0000-000000000001', 'sample.delete_soft'),
  ('00000000-0000-0000-0000-000000000001', 'sample.delete_hard'),
  ('00000000-0000-0000-0000-000000000001', 'box.configure'),
  ('00000000-0000-0000-0000-000000000001', 'freezer.configure'),
  ('00000000-0000-0000-0000-000000000001', 'custom_field.define'),
  ('00000000-0000-0000-0000-000000000001', 'item_type.define'),
  ('00000000-0000-0000-0000-000000000001', 'user.invite'),
  ('00000000-0000-0000-0000-000000000001', 'user.manage_roles'),
  ('00000000-0000-0000-0000-000000000001', 'audit.read'),
  ('00000000-0000-0000-0000-000000000001', 'audit.export'),
  ('00000000-0000-0000-0000-000000000001', 'backup.run'),
  ('00000000-0000-0000-0000-000000000001', 'share.request'),
  ('00000000-0000-0000-0000-000000000001', 'share.approve'),
  ('00000000-0000-0000-0000-000000000001', 'lab.configure'),
  ('00000000-0000-0000-0000-000000000001', 'lab.enable_phi'),
  ('00000000-0000-0000-0000-000000000001', 'key.rotate'),
  ('00000000-0000-0000-0000-000000000001', 'session.revoke'),
  ('00000000-0000-0000-0000-000000000002', 'sample.read'),
  ('00000000-0000-0000-0000-000000000002', 'sample.write'),
  ('00000000-0000-0000-0000-000000000002', 'sample.checkout'),
  ('00000000-0000-0000-0000-000000000002', 'sample.delete_soft'),
  ('00000000-0000-0000-0000-000000000002', 'box.configure'),
  ('00000000-0000-0000-0000-000000000002', 'freezer.configure'),
  ('00000000-0000-0000-0000-000000000002', 'custom_field.define'),
  ('00000000-0000-0000-0000-000000000002', 'item_type.define'),
  ('00000000-0000-0000-0000-000000000002', 'user.invite'),
  ('00000000-0000-0000-0000-000000000002', 'user.manage_roles'),
  ('00000000-0000-0000-0000-000000000002', 'audit.read'),
  ('00000000-0000-0000-0000-000000000002', 'audit.export'),
  ('00000000-0000-0000-0000-000000000002', 'share.request'),
  ('00000000-0000-0000-0000-000000000002', 'share.approve'),
  ('00000000-0000-0000-0000-000000000002', 'lab.configure'),
  ('00000000-0000-0000-0000-000000000003', 'sample.read'),
  ('00000000-0000-0000-0000-000000000003', 'sample.write'),
  ('00000000-0000-0000-0000-000000000003', 'sample.checkout'),
  ('00000000-0000-0000-0000-000000000003', 'sample.delete_soft'),
  ('00000000-0000-0000-0000-000000000003', 'share.request'),
  ('00000000-0000-0000-0000-000000000004', 'sample.read'),
  ('00000000-0000-0000-0000-000000000004', 'audit.read'),
  ('00000000-0000-0000-0000-000000000005', 'sample.read'),
  ('00000000-0000-0000-0000-000000000005', 'sample.checkout');
)sql",
          },
          SqliteMigration{
              .version = 4,
              .name = "0004_layout",
              .up_sql = R"sql(
CREATE TABLE IF NOT EXISTS storage_containers (
  id TEXT PRIMARY KEY,
  lab_id TEXT NOT NULL REFERENCES labs(id) DEFERRABLE INITIALLY DEFERRED,
  parent_id TEXT REFERENCES storage_containers(id) DEFERRABLE INITIALLY DEFERRED,
  kind TEXT NOT NULL CHECK (kind IN
    ('compartment', 'shelf', 'rack', 'drawer', 'custom')),
  name TEXT NOT NULL CHECK (length(name) > 0),
  label TEXT NOT NULL,
  ordering_index INTEGER NOT NULL DEFAULT 0,
  capacity_hint_json TEXT NOT NULL CHECK (json_valid(capacity_hint_json)),
  created_at_micros INTEGER NOT NULL,
  archived_at_micros INTEGER,
  CHECK (id <> parent_id)
);

CREATE INDEX IF NOT EXISTS storage_containers_parent_idx
  ON storage_containers(parent_id);
CREATE INDEX IF NOT EXISTS storage_containers_lab_idx
  ON storage_containers(lab_id);

CREATE TABLE IF NOT EXISTS freezers (
  id TEXT PRIMARY KEY,
  lab_id TEXT NOT NULL REFERENCES labs(id) DEFERRABLE INITIALLY DEFERRED,
  name TEXT NOT NULL CHECK (length(name) > 0),
  location TEXT NOT NULL,
  model TEXT NOT NULL,
  temp_target_c REAL,
  layout_root_id TEXT NOT NULL
    REFERENCES storage_containers(id) DEFERRABLE INITIALLY DEFERRED,
  created_at_micros INTEGER NOT NULL,
  archived_at_micros INTEGER
);

CREATE INDEX IF NOT EXISTS freezers_lab_id_idx ON freezers(lab_id);
CREATE UNIQUE INDEX IF NOT EXISTS freezers_lab_name_unique
  ON freezers(lab_id, name) WHERE archived_at_micros IS NULL;
)sql",
          },
          SqliteMigration{
              .version = 5,
              .name = "0005_box_types",
              .up_sql = R"sql(
CREATE TABLE IF NOT EXISTS container_types (
  id TEXT PRIMARY KEY,
  lab_id TEXT NOT NULL REFERENCES labs(id) DEFERRABLE INITIALLY DEFERRED,
  name TEXT NOT NULL CHECK (length(name) > 0),
  size_class TEXT NOT NULL CHECK (length(size_class) > 0),
  outer_dimensions_json TEXT NOT NULL CHECK (json_valid(outer_dimensions_json)),
  material TEXT NOT NULL,
  supplier_sku TEXT NOT NULL,
  created_at_micros INTEGER NOT NULL,
  archived_at_micros INTEGER
);

CREATE INDEX IF NOT EXISTS container_types_lab_idx
  ON container_types(lab_id);
CREATE INDEX IF NOT EXISTS container_types_lab_size_class_idx
  ON container_types(lab_id, size_class);

CREATE TABLE IF NOT EXISTS box_types (
  id TEXT PRIMARY KEY,
  lab_id TEXT NOT NULL REFERENCES labs(id) DEFERRABLE INITIALLY DEFERRED,
  name TEXT NOT NULL CHECK (length(name) > 0),
  manufacturer TEXT NOT NULL,
  sku TEXT NOT NULL,
  created_at_micros INTEGER NOT NULL,
  archived_at_micros INTEGER
);

CREATE INDEX IF NOT EXISTS box_types_lab_idx
  ON box_types(lab_id);
CREATE UNIQUE INDEX IF NOT EXISTS box_types_lab_name_unique
  ON box_types(lab_id, name) WHERE archived_at_micros IS NULL;

CREATE TABLE IF NOT EXISTS box_type_positions (
  box_type_id TEXT NOT NULL
    REFERENCES box_types(id) ON DELETE CASCADE DEFERRABLE INITIALLY DEFERRED,
  label TEXT NOT NULL CHECK (length(label) > 0),
  row_index INTEGER NOT NULL CHECK (row_index >= 0),
  col_index INTEGER NOT NULL CHECK (col_index >= 0),
  z_index INTEGER CHECK (z_index IS NULL OR z_index >= 0),
  PRIMARY KEY (box_type_id, label)
);

CREATE TABLE IF NOT EXISTS box_type_position_accepts (
  box_type_id TEXT NOT NULL,
  position_label TEXT NOT NULL,
  size_class TEXT NOT NULL CHECK (length(size_class) > 0),
  PRIMARY KEY (box_type_id, position_label, size_class),
  FOREIGN KEY (box_type_id, position_label)
    REFERENCES box_type_positions(box_type_id, label)
    ON DELETE CASCADE DEFERRABLE INITIALLY DEFERRED
);
)sql",
          },
          SqliteMigration{
              .version = 6,
              .name = "0006_boxes",
              .up_sql = R"sql(
CREATE TABLE IF NOT EXISTS boxes (
  id                   TEXT    PRIMARY KEY,
  lab_id               TEXT    NOT NULL REFERENCES labs(id)
                                 DEFERRABLE INITIALLY DEFERRED,
  box_type_id          TEXT    NOT NULL REFERENCES box_types(id)
                                 DEFERRABLE INITIALLY DEFERRED,
  storage_container_id TEXT    NOT NULL REFERENCES storage_containers(id)
                                 DEFERRABLE INITIALLY DEFERRED,
  label                TEXT    NOT NULL CHECK (length(label) > 0),
  serial               TEXT,
  barcode              TEXT,
  created_at_micros    INTEGER NOT NULL,
  archived_at_micros   INTEGER
);

CREATE INDEX IF NOT EXISTS boxes_lab_idx
  ON boxes(lab_id);
CREATE INDEX IF NOT EXISTS boxes_storage_container_idx
  ON boxes(storage_container_id);
CREATE INDEX IF NOT EXISTS boxes_box_type_idx
  ON boxes(box_type_id);
CREATE UNIQUE INDEX IF NOT EXISTS boxes_lab_label_unique
  ON boxes(lab_id, label) WHERE archived_at_micros IS NULL;
)sql",
          },
          SqliteMigration{
              .version = 7,
              .name = "0007_item_types",
              .up_sql = R"sql(
CREATE TABLE IF NOT EXISTS item_types (
  id                 TEXT    PRIMARY KEY,
  lab_id             TEXT    NOT NULL REFERENCES labs(id)       DEFERRABLE INITIALLY DEFERRED,
  parent_id          TEXT             REFERENCES item_types(id) DEFERRABLE INITIALLY DEFERRED,
  name               TEXT    NOT NULL CHECK (length(name) > 0),
  created_at_micros  INTEGER NOT NULL,
  archived_at_micros INTEGER,
  CHECK (id <> parent_id)
);

CREATE INDEX IF NOT EXISTS item_types_lab_idx    ON item_types(lab_id);
CREATE INDEX IF NOT EXISTS item_types_parent_idx ON item_types(parent_id);
CREATE UNIQUE INDEX IF NOT EXISTS item_types_lab_name_unique
  ON item_types(lab_id, name) WHERE archived_at_micros IS NULL;

CREATE TABLE IF NOT EXISTS custom_field_definitions (
  id                 TEXT    PRIMARY KEY,
  lab_id             TEXT    NOT NULL REFERENCES labs(id)       DEFERRABLE INITIALLY DEFERRED,
  scope_kind         TEXT    NOT NULL
    CHECK (scope_kind IN ('sample', 'box', 'freezer', 'container')),
  item_type_id       TEXT             REFERENCES item_types(id) DEFERRABLE INITIALLY DEFERRED,
  key                TEXT    NOT NULL CHECK (length(key) > 0),
  label              TEXT    NOT NULL CHECK (length(label) > 0),
  data_type          TEXT    NOT NULL,
  required           INTEGER NOT NULL DEFAULT 0 CHECK (required IN (0, 1)),
  validation_json    TEXT    NOT NULL DEFAULT '{}',
  indexed            INTEGER NOT NULL DEFAULT 0 CHECK (indexed IN (0, 1)),
  is_phi             INTEGER NOT NULL DEFAULT 0 CHECK (is_phi IN (0, 1)),
  created_at_micros  INTEGER NOT NULL,
  archived_at_micros INTEGER
);

CREATE INDEX IF NOT EXISTS cfd_lab_idx  ON custom_field_definitions(lab_id);
CREATE INDEX IF NOT EXISTS cfd_type_idx ON custom_field_definitions(item_type_id);

CREATE UNIQUE INDEX IF NOT EXISTS cfd_lab_scope_type_key_unique
  ON custom_field_definitions(lab_id, scope_kind, COALESCE(item_type_id, ''), key)
  WHERE archived_at_micros IS NULL;
)sql",
          },
          SqliteMigration{
              .version = 8,
              .name = "0008_samples",
              .up_sql = R"sql(
CREATE TABLE IF NOT EXISTS projects (
  id                 TEXT    PRIMARY KEY,
  lab_id             TEXT    NOT NULL REFERENCES labs(id)  DEFERRABLE INITIALLY DEFERRED,
  name               TEXT    NOT NULL CHECK (length(name) > 0),
  owner_user_id      TEXT    NOT NULL REFERENCES users(id) DEFERRABLE INITIALLY DEFERRED,
  created_at_micros  INTEGER NOT NULL,
  archived_at_micros INTEGER
);

CREATE INDEX  IF NOT EXISTS projects_lab_idx ON projects(lab_id);
CREATE UNIQUE INDEX IF NOT EXISTS projects_lab_name_unique
  ON projects(lab_id, name) WHERE archived_at_micros IS NULL;

CREATE TABLE IF NOT EXISTS samples (
  id                      TEXT    PRIMARY KEY,
  lab_id                  TEXT    NOT NULL REFERENCES labs(id)            DEFERRABLE INITIALLY DEFERRED,
  item_type_id            TEXT    NOT NULL REFERENCES item_types(id)      DEFERRABLE INITIALLY DEFERRED,
  name                    TEXT    NOT NULL CHECK (length(name) > 0),
  barcode                 TEXT,
  container_type_id       TEXT             REFERENCES container_types(id) DEFERRABLE INITIALLY DEFERRED,
  box_id                  TEXT             REFERENCES boxes(id)           DEFERRABLE INITIALLY DEFERRED,
  position_label          TEXT,
  volume_value            INTEGER,
  volume_unit             TEXT,
  mass_value              INTEGER,
  mass_unit               TEXT,
  status                  TEXT    NOT NULL DEFAULT 'active'
    CHECK (status IN ('active', 'checked_out', 'depleted', 'destroyed', 'tombstoned')),
  parent_sample_id        TEXT             REFERENCES samples(id)         DEFERRABLE INITIALLY DEFERRED,
  created_by              TEXT    NOT NULL,
  created_at_micros       INTEGER NOT NULL,
  last_modified_by        TEXT    NOT NULL,
  last_modified_at_micros INTEGER NOT NULL,
  custom_fields_json      TEXT    NOT NULL DEFAULT '{}',
  phi_fields_enc_json     TEXT    NOT NULL DEFAULT '{}',
  CHECK ((box_id IS NULL) = (position_label IS NULL))
);

CREATE INDEX IF NOT EXISTS samples_lab_idx    ON samples(lab_id);
CREATE INDEX IF NOT EXISTS samples_box_idx    ON samples(box_id);
CREATE INDEX IF NOT EXISTS samples_parent_idx ON samples(parent_sample_id);
CREATE INDEX IF NOT EXISTS samples_status_idx ON samples(status);

CREATE UNIQUE INDEX IF NOT EXISTS samples_position_unique
  ON samples(box_id, position_label)
  WHERE status IN ('active', 'checked_out') AND box_id IS NOT NULL;

CREATE TABLE IF NOT EXISTS sample_projects (
  sample_id  TEXT NOT NULL REFERENCES samples(id)  DEFERRABLE INITIALLY DEFERRED,
  project_id TEXT NOT NULL REFERENCES projects(id) DEFERRABLE INITIALLY DEFERRED,
  PRIMARY KEY (sample_id, project_id)
);

CREATE TABLE IF NOT EXISTS checkout_events (
  id             TEXT    PRIMARY KEY,
  sample_id      TEXT    NOT NULL REFERENCES samples(id) DEFERRABLE INITIALLY DEFERRED,
  lab_id         TEXT    NOT NULL REFERENCES labs(id)    DEFERRABLE INITIALLY DEFERRED,
  user_id        TEXT    NOT NULL REFERENCES users(id)   DEFERRABLE INITIALLY DEFERRED,
  action         TEXT    NOT NULL CHECK (action IN ('out', 'in', 'destroy')),
  reason         TEXT,
  at_micros      INTEGER NOT NULL,
  volume_delta   INTEGER,
  volume_unit    TEXT,
  location_after TEXT
);

CREATE INDEX IF NOT EXISTS checkout_events_sample_idx ON checkout_events(sample_id);
CREATE INDEX IF NOT EXISTS checkout_events_lab_idx    ON checkout_events(lab_id);
CREATE INDEX IF NOT EXISTS checkout_events_user_idx   ON checkout_events(user_id);
)sql",
          },
          {
              .version = 9,
              .name = "0009_share_requests",
              .up_sql = R"sql(
CREATE TABLE IF NOT EXISTS share_requests (
  id                TEXT    PRIMARY KEY,
  source_lab_id     TEXT    NOT NULL REFERENCES labs(id)  DEFERRABLE INITIALLY DEFERRED,
  target_lab_id     TEXT    NOT NULL REFERENCES labs(id)  DEFERRABLE INITIALLY DEFERRED,
  requested_by      TEXT    NOT NULL REFERENCES users(id) DEFERRABLE INITIALLY DEFERRED,
  scope_json        TEXT    NOT NULL DEFAULT '{}',
  status            TEXT    NOT NULL DEFAULT 'pending'
    CHECK (status IN ('pending', 'approved', 'rejected', 'revoked')),
  created_at_micros  INTEGER NOT NULL,
  decided_at_micros  INTEGER,
  CHECK (source_lab_id != target_lab_id)
);

CREATE INDEX IF NOT EXISTS share_requests_source_lab_idx ON share_requests(source_lab_id);
CREATE INDEX IF NOT EXISTS share_requests_target_lab_idx ON share_requests(target_lab_id);
CREATE INDEX IF NOT EXISTS share_requests_status_idx     ON share_requests(status);

CREATE TABLE IF NOT EXISTS share_request_approvals (
  share_request_id  TEXT    NOT NULL REFERENCES share_requests(id) DEFERRABLE INITIALLY DEFERRED,
  approver_role     TEXT    NOT NULL
    CHECK (approver_role IN ('source_admin', 'target_admin', 'system_admin')),
  approver_user_id  TEXT    NOT NULL REFERENCES users(id) DEFERRABLE INITIALLY DEFERRED,
  decided_at_micros INTEGER NOT NULL,
  note              TEXT,
  PRIMARY KEY (share_request_id, approver_role)
);

CREATE INDEX IF NOT EXISTS share_request_approvals_request_idx
  ON share_request_approvals(share_request_id);
)sql",
          },
          {
              .version = 11,
              .name = "0011_audit_events",
              .up_sql = R"sql(
-- Replace the simple audit_events table introduced in 0001_init with the
-- full hash-chained schema required by PRD §7.3.
-- DROP is safe here because the 0001 table was a temporary bootstrap placeholder
-- with no production data.
DROP TABLE IF EXISTS audit_events;

CREATE TABLE audit_events (
  id               TEXT    PRIMARY KEY,
  at_micros        INTEGER NOT NULL,
  actor_user_id    TEXT    NOT NULL,
  actor_session_id TEXT    NOT NULL,
  lab_id           TEXT,
  action           TEXT    NOT NULL,
  entity_kind      TEXT    NOT NULL,
  entity_id        TEXT,
  before_json      TEXT    NOT NULL DEFAULT '{}',
  after_json       TEXT    NOT NULL DEFAULT '{}',
  request_id       TEXT    NOT NULL,
  prev_hash        TEXT    NOT NULL,
  this_hash        TEXT    NOT NULL UNIQUE
);

CREATE INDEX IF NOT EXISTS audit_events_at_idx          ON audit_events(at_micros);
CREATE INDEX IF NOT EXISTS audit_events_actor_idx       ON audit_events(actor_user_id);
CREATE INDEX IF NOT EXISTS audit_events_entity_kind_idx ON audit_events(entity_kind);
CREATE INDEX IF NOT EXISTS audit_events_lab_idx         ON audit_events(lab_id);

-- Immutability triggers: the hash chain must never be altered.
CREATE TRIGGER IF NOT EXISTS audit_events_no_update
  BEFORE UPDATE ON audit_events
  FOR EACH ROW
  BEGIN
    SELECT RAISE(ABORT, 'audit_events is append-only: updates are not permitted');
  END;

CREATE TRIGGER IF NOT EXISTS audit_events_no_delete
  BEFORE DELETE ON audit_events
  FOR EACH ROW
  BEGIN
    SELECT RAISE(ABORT, 'audit_events is append-only: deletes are not permitted');
  END;
)sql",
          },
          {
              .version = 10,
              .name = "0010_sessions",
              .up_sql = R"sql(
-- Server-side sessions and API tokens (D9).
-- token_prefix is the plaintext lookup key; token_hash is the Argon2id digest.
-- The auth layer generates the full random token, hashes it, and stores only
-- the hash.  No ON DELETE CASCADE: tombstones are set via revoked_at_micros.

CREATE TABLE IF NOT EXISTS sessions (
  id                    TEXT    PRIMARY KEY,
  user_id               TEXT    NOT NULL REFERENCES users(id) DEFERRABLE INITIALLY DEFERRED,
  token_hash            TEXT    NOT NULL CHECK (length(token_hash) > 0),
  token_prefix          TEXT    NOT NULL CHECK (length(token_prefix) > 0),
  created_at_micros     INTEGER NOT NULL,
  last_seen_at_micros   INTEGER NOT NULL,
  ip                    TEXT,
  user_agent            TEXT,
  revoked_at_micros     INTEGER
);

-- Only one active session may use a given prefix at a time.
CREATE UNIQUE INDEX IF NOT EXISTS sessions_prefix_active_unique
  ON sessions(token_prefix) WHERE revoked_at_micros IS NULL;
CREATE INDEX IF NOT EXISTS sessions_user_id_idx
  ON sessions(user_id);

CREATE TABLE IF NOT EXISTS api_tokens (
  id                    TEXT    PRIMARY KEY,
  user_id               TEXT    NOT NULL REFERENCES users(id)  DEFERRABLE INITIALLY DEFERRED,
  lab_id                TEXT             REFERENCES labs(id)   DEFERRABLE INITIALLY DEFERRED,
  name                  TEXT    NOT NULL CHECK (length(name) > 0),
  scope_json            TEXT    NOT NULL DEFAULT '[]' CHECK (json_valid(scope_json)),
  token_hash            TEXT    NOT NULL CHECK (length(token_hash) > 0),
  token_prefix          TEXT    NOT NULL CHECK (length(token_prefix) > 0),
  created_at_micros     INTEGER NOT NULL,
  expires_at_micros     INTEGER,
  revoked_at_micros     INTEGER
);

CREATE UNIQUE INDEX IF NOT EXISTS api_tokens_prefix_active_unique
  ON api_tokens(token_prefix) WHERE revoked_at_micros IS NULL;
CREATE INDEX IF NOT EXISTS api_tokens_user_id_idx ON api_tokens(user_id);
CREATE INDEX IF NOT EXISTS api_tokens_lab_id_idx  ON api_tokens(lab_id);
)sql",
          },
          {
              .version = 12,
              .name = "0012_sessions_mfa",
              .up_sql = R"sql(
-- E2: Add MFA completion tracking to sessions.
-- Existing rows default to 1 (mfa_complete = true) so that previously
-- created sessions remain valid after the migration.
ALTER TABLE sessions ADD COLUMN mfa_complete INTEGER NOT NULL DEFAULT 1;
)sql",
          },
          {
              .version = 13,
              .name = "0013_authz_version",
              .up_sql = R"sql(
-- Authorization epoch for cache invalidation. Bumped in-transaction whenever a
-- user's effective permissions change (membership/role/grant). Auth providers
-- key their resolved-permission cache on this value so a downgrade takes effect
-- on the next request rather than waiting for the cache TTL. Existing rows start
-- at 0; any later authz change increments it.
ALTER TABLE users ADD COLUMN authz_version INTEGER NOT NULL DEFAULT 0;
)sql",
          },
          {
              .version = 14,
              .name = "0014_lab_provision",
              .up_sql = R"sql(
-- Global-only permission for deployment-level lab provisioning (LabService/CreateLab).
-- Creating a brand-new lab cannot be gated by a per-lab permission because no
-- membership exists yet; it is a SystemAdmin deployment action. Granted to the
-- built-in SystemAdmin role only.
INSERT INTO permissions (key, description) VALUES
  ('lab.provision', 'Provision (create) new labs (SystemAdmin only).');
INSERT INTO role_permissions (role_id, permission_key) VALUES
  ('00000000-0000-0000-0000-000000000001', 'lab.provision');
)sql",
          },
      };
    }

    void bind_text(sqlite3_stmt* statement, int index, const std::string& value) {
      const auto result = sqlite3_bind_text(statement, index, value.c_str(),
                                            static_cast<int>(value.size()), SQLITE_TRANSIENT);
      if (result != SQLITE_OK) {
        throw ConstraintViolation("failed to bind sqlite text parameter");
      }
    }

    void bind_int64(sqlite3_stmt* statement, int index, std::int64_t value) {
      const auto result = sqlite3_bind_int64(statement, index, value);
      if (result != SQLITE_OK) {
        throw ConstraintViolation("failed to bind sqlite integer parameter");
      }
    }

    class PreparedStatement {
    public:
      PreparedStatement(sqlite3* handle, const std::string& sql) : handle_(handle) {
        const auto result = sqlite3_prepare_v2(handle_, sql.c_str(), -1, &statement_, nullptr);
        if (result != SQLITE_OK) {
          throw_sqlite_error(result, handle_, "prepare sqlite statement");
        }
      }

      ~PreparedStatement() {
        sqlite3_finalize(statement_);
      }

      PreparedStatement(const PreparedStatement&) = delete;
      PreparedStatement& operator=(const PreparedStatement&) = delete;

      [[nodiscard]] sqlite3_stmt* get() const {
        return statement_;
      }

      [[nodiscard]] bool step_row() const {
        const auto result = sqlite3_step(statement_);
        if (result == SQLITE_ROW) {
          return true;
        }
        if (result == SQLITE_DONE) {
          return false;
        }
        throw_sqlite_error(result, handle_, "step sqlite statement");
      }

      void step_done() const {
        const auto result = sqlite3_step(statement_);
        if (result != SQLITE_DONE) {
          throw_sqlite_error(result, handle_, "execute sqlite statement");
        }
      }

    private:
      sqlite3* handle_;
      sqlite3_stmt* statement_{nullptr};
    };

    // ---- Audit chain helpers ----

    [[nodiscard]] std::string generate_random_uuid() {
      if (sodium_init() < 0) {
        throw std::runtime_error("libsodium initialisation failed");
      }
      std::array<unsigned char, 16> bytes{};
      randombytes_buf(bytes.data(), bytes.size());
      bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U); // version 4
      bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U); // variant
      // UUID text: 32 hex chars + 4 hyphens + NUL = 37 bytes.
      std::array<char, 37> buf{};
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
      std::snprintf(buf.data(), buf.size(),
                    "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                    bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                    bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14],
                    bytes[15]);
      return {buf.data()};
    }

    [[nodiscard]] std::string fetch_last_audit_hash(sqlite3* handle) {
      sqlite3_stmt* raw_stmt = nullptr;
      const auto prepare_result = sqlite3_prepare_v2(
          handle, "SELECT this_hash FROM audit_events ORDER BY rowid DESC LIMIT 1", -1, &raw_stmt,
          nullptr);
      if (prepare_result != SQLITE_OK) {
        return std::string(audit::zero_hash());
      }
      std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(raw_stmt, sqlite3_finalize);
      if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        return std::string(audit::zero_hash());
      }
      const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
      return (text == nullptr) ? std::string(audit::zero_hash()) : std::string(text);
    }

    // BLAKE2b-256 hex digest of `data` via libsodium. Stable across platforms/compilers.
    [[nodiscard]] std::string blake2b_hex(const std::string& data) {
      if (sodium_init() < 0) {
        throw std::runtime_error("libsodium initialisation failed");
      }
      std::array<unsigned char, crypto_generichash_BYTES> hash{};
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      crypto_generichash(hash.data(), hash.size(),
                         reinterpret_cast<const unsigned char*>(data.data()), data.size(), nullptr,
                         0);
      std::string hex;
      hex.reserve(hash.size() * 2);
      static constexpr std::array<char, 16> k_nibbles = {'0', '1', '2', '3', '4', '5', '6', '7',
                                                         '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
      for (const unsigned char byte : hash) {
        hex += k_nibbles[byte >> 4U];
        hex += k_nibbles[byte & 0x0fU];
      }
      return hex;
    }

    [[nodiscard]] std::string checksum(const SqliteMigration& migration) {
      return blake2b_hex(std::to_string(migration.version) + ":" + migration.name + ":" +
                         migration.up_sql);
    }

    [[nodiscard]] std::int64_t now_unix_seconds() {
      return static_cast<std::int64_t>(std::time(nullptr));
    }

  } // namespace

  struct SqliteBackendState {
    explicit SqliteBackendState(SqliteBackendOptions input_options)
        : options(std::move(input_options)) {
      if (options.database_path.empty()) {
        options.database_path = k_in_memory_path;
      }
      if (options.database_path == k_in_memory_path) {
        options.database_path = default_memory_uri();
      }
      if (options.migrations.empty()) {
        options.migrations = default_migrations();
      }
      anchor = std::make_unique<SqliteConnection>(options.database_path, options.busy_timeout);
    }

    [[nodiscard]] std::unique_ptr<SqliteConnection> open_connection() const {
      return std::make_unique<SqliteConnection>(options.database_path, options.busy_timeout);
    }

    SqliteBackendOptions options;
    std::unique_ptr<SqliteConnection> anchor;
    mutable std::mutex mutex;
    bool fail_next_audit_append{false};
  };

  namespace {

    void exec(sqlite3* handle, const std::string& sql) {
      char* error_message = nullptr;
      const auto result = sqlite3_exec(handle, sql.c_str(), nullptr, nullptr, &error_message);
      if (result != SQLITE_OK) {
        std::string message;
        if (error_message != nullptr) {
          message = error_message;
          sqlite3_free(error_message);
        } else {
          message = sqlite_message(handle);
        }
        throw_sqlite_error(result, handle, message);
      }
    }

    void rollback_no_throw(sqlite3* handle) noexcept {
      sqlite3_exec(handle, "ROLLBACK", nullptr, nullptr, nullptr);
    }

    void ensure_metadata_tables(sqlite3* handle) {
      exec(handle, R"sql(
CREATE TABLE IF NOT EXISTS schema_migrations (
  version INTEGER PRIMARY KEY,
  name TEXT NOT NULL,
  checksum TEXT NOT NULL,
  applied_at_unix_seconds INTEGER NOT NULL
);
)sql");
      // Hash-chained, append-only audit log (PRD §7.3).
      // Migration 0001 contained a simple placeholder; 0011 promotes to this schema.
      // ensure_metadata_tables() creates it here so backends using custom migrations
      // (e.g. conformance tests) also get the correct schema.
      exec(handle, R"sql(
CREATE TABLE IF NOT EXISTS audit_events (
  id               TEXT    PRIMARY KEY,
  at_micros        INTEGER NOT NULL,
  actor_user_id    TEXT    NOT NULL,
  actor_session_id TEXT    NOT NULL,
  lab_id           TEXT,
  action           TEXT    NOT NULL,
  entity_kind      TEXT    NOT NULL,
  entity_id        TEXT,
  before_json      TEXT    NOT NULL DEFAULT '{}',
  after_json       TEXT    NOT NULL DEFAULT '{}',
  request_id       TEXT    NOT NULL,
  prev_hash        TEXT    NOT NULL,
  this_hash        TEXT    NOT NULL UNIQUE
);
)sql");
      exec(handle, R"sql(
CREATE TRIGGER IF NOT EXISTS audit_events_no_update
  BEFORE UPDATE ON audit_events
  FOR EACH ROW
  BEGIN
    SELECT RAISE(ABORT, 'audit_events is append-only: updates are not permitted');
  END;
)sql");
      exec(handle, R"sql(
CREATE TRIGGER IF NOT EXISTS audit_events_no_delete
  BEFORE DELETE ON audit_events
  FOR EACH ROW
  BEGIN
    SELECT RAISE(ABORT, 'audit_events is append-only: deletes are not permitted');
  END;
)sql");
    }

    [[nodiscard]] std::optional<std::string> applied_checksum(sqlite3* handle, int version) {
      PreparedStatement statement(handle,
                                  "SELECT checksum FROM schema_migrations WHERE version = ?");
      bind_int64(statement.get(), 1, version);
      if (!statement.step_row()) {
        return std::nullopt;
      }
      const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 0));
      return std::string(text == nullptr ? "" : text);
    }

    [[nodiscard]] SchemaVersion current_schema_version(sqlite3* handle) {
      ensure_metadata_tables(handle);
      PreparedStatement statement(handle,
                                  "SELECT COALESCE(MAX(version), 0) FROM schema_migrations");
      if (!statement.step_row()) {
        return SchemaVersion{0};
      }
      return SchemaVersion{sqlite3_column_int(statement.get(), 0)};
    }

  } // namespace

  class SqliteTransaction::Impl {
  public:
    Impl(std::shared_ptr<SqliteBackendState> input_state, IsolationLevel input_isolation_level)
        : state(std::move(input_state)), isolation_level(input_isolation_level),
          connection(state->open_connection()) {}

    std::shared_ptr<SqliteBackendState> state;
    IsolationLevel isolation_level;
    std::unique_ptr<SqliteConnection> connection;
    std::vector<std::function<void(sqlite3*)>> commit_hooks;

    struct AuditMutation {
      std::string entity_kind; // was entity_name; maps to entity_kind column
      std::string entity_id;
      std::string action; // e.g. "mutation", "insert", "update", "soft_delete"
      MutationContext context;
      AuditSnapshot snapshot; // repository-derived before/after state
    };
    std::vector<AuditMutation> audit_mutations;
    bool completed{false};
  };

  SqliteTransaction::SqliteTransaction(std::shared_ptr<SqliteBackendState> state,
                                       IsolationLevel isolation_level)
      : impl_(std::make_unique<Impl>(std::move(state), isolation_level)) {}

  SqliteTransaction::~SqliteTransaction() = default;

  sqlite3* SqliteTransaction::handle() const {
    return impl_->connection->get();
  }

  IsolationLevel SqliteTransaction::isolation_level() const {
    return impl_->isolation_level;
  }

  void SqliteTransaction::add_commit_hook(std::function<void(sqlite3*)> hook) {
    impl_->commit_hooks.push_back(std::move(hook));
  }

  void SqliteTransaction::note_mutation(std::string entity_kind, std::string entity_id,
                                        const MutationContext& context, std::string action,
                                        AuditSnapshot snapshot) {
    impl_->audit_mutations.push_back(SqliteTransaction::Impl::AuditMutation{
        .entity_kind = std::move(entity_kind),
        .entity_id = std::move(entity_id),
        .action = std::move(action),
        .context = context,
        .snapshot = std::move(snapshot),
    });
  }

  void SqliteTransaction::commit() {
    if (impl_->completed) {
      throw ConstraintViolation("sqlite transaction already completed");
    }

    sqlite3* database = handle();
    bool began = false;
    try {
      exec(database, "BEGIN IMMEDIATE");
      began = true;
      for (const auto& hook : impl_->commit_hooks) {
        hook(database);
      }

      {
        std::scoped_lock lock(impl_->state->mutex);
        if (impl_->state->fail_next_audit_append && !impl_->audit_mutations.empty()) {
          impl_->state->fail_next_audit_append = false;
          throw ConstraintViolation("audit append failed");
        }
      }

      if (!impl_->audit_mutations.empty()) {
        // Initialise libsodium (idempotent).
        if (sodium_init() < 0) {
          throw ConstraintViolation("libsodium initialisation failed");
        }

        // Chain starts from the last committed audit row (or zero-hash).
        std::string prev_hash = fetch_last_audit_hash(database);

        const auto now_micros =
            static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());

        PreparedStatement audit_stmt(database,
                                     "INSERT INTO audit_events "
                                     "(id, at_micros, actor_user_id, actor_session_id, lab_id, "
                                     "action, entity_kind, entity_id, before_json, after_json, "
                                     "request_id, prev_hash, this_hash) "
                                     "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");

        for (const auto& mutation : impl_->audit_mutations) {
          const auto event_id = generate_random_uuid();

          const std::string before_str =
              mutation.snapshot.before.has_value() ? mutation.snapshot.before->dump() : "{}";
          const std::string after_str =
              mutation.snapshot.after.has_value() ? mutation.snapshot.after->dump() : "{}";

          // Build the content JSON (alphabetically sorted; nlohmann uses std::map).
          const nlohmann::json lab_id_val = mutation.context.lab_id.has_value()
                                                ? nlohmann::json(*mutation.context.lab_id)
                                                : nlohmann::json(nullptr);
          const nlohmann::json content = {
              {"action", mutation.action},
              {"actor_session_id", mutation.context.actor_session_id},
              {"actor_user_id", mutation.context.actor_user_id.to_string()},
              {"after_json", after_str},
              {"at", now_micros},
              {"before_json", before_str},
              {"entity_id", mutation.entity_id},
              {"entity_kind", mutation.entity_kind},
              {"id", event_id},
              {"lab_id", lab_id_val},
              {"request_id", mutation.context.request_id},
          };
          const auto content_json = audit::canonical_json(content);
          const auto this_hash = audit::compute_audit_hash(prev_hash, content_json);

          sqlite3_reset(audit_stmt.get());
          sqlite3_clear_bindings(audit_stmt.get());
          bind_text(audit_stmt.get(), 1, event_id);
          bind_int64(audit_stmt.get(), 2, now_micros);
          bind_text(audit_stmt.get(), 3, mutation.context.actor_user_id.to_string());
          bind_text(audit_stmt.get(), 4, mutation.context.actor_session_id);
          if (mutation.context.lab_id.has_value()) {
            bind_text(audit_stmt.get(), 5, *mutation.context.lab_id);
          } else {
            sqlite3_bind_null(audit_stmt.get(), 5);
          }
          bind_text(audit_stmt.get(), 6, mutation.action);
          bind_text(audit_stmt.get(), 7, mutation.entity_kind);
          bind_text(audit_stmt.get(), 8, mutation.entity_id);
          bind_text(audit_stmt.get(), 9, before_str);
          bind_text(audit_stmt.get(), 10, after_str);
          bind_text(audit_stmt.get(), 11, mutation.context.request_id);
          bind_text(audit_stmt.get(), 12, prev_hash);
          bind_text(audit_stmt.get(), 13, this_hash);
          audit_stmt.step_done();

          prev_hash = this_hash;
        }
      }

      exec(database, "COMMIT");
      impl_->completed = true;
    } catch (...) {
      if (began) {
        rollback_no_throw(database);
      }
      impl_->completed = true;
      throw;
    }
  }

  void SqliteTransaction::rollback() {
    impl_->completed = true;
  }

  SqliteBackend::SqliteBackend(SqliteBackendOptions options)
      : state_(std::make_shared<SqliteBackendState>(std::move(options))) {}

  SqliteBackend::~SqliteBackend() = default;
  SqliteBackend::SqliteBackend(SqliteBackend&&) noexcept = default;
  SqliteBackend& SqliteBackend::operator=(SqliteBackend&&) noexcept = default;

  void SqliteBackend::migrate_to_latest() {
    auto connection = state_->open_connection();
    sqlite3* database = connection->get();
    bool began = false;
    try {
      exec(database, "BEGIN IMMEDIATE");
      began = true;
      ensure_metadata_tables(database);

      auto migrations = state_->options.migrations;
      std::ranges::sort(migrations, {}, &SqliteMigration::version);
      int previous_version = 0;
      for (const auto& migration : migrations) {
        if (migration.version <= previous_version) {
          throw MigrationFailure("sqlite migration versions must be strictly increasing");
        }
        previous_version = migration.version;

        const auto expected_checksum = checksum(migration);
        const auto existing_checksum = applied_checksum(database, migration.version);
        if (existing_checksum.has_value()) {
          if (existing_checksum.value() != expected_checksum) {
            throw MigrationFailure("sqlite migration checksum changed after application");
          }
          continue;
        }

        exec(database, migration.up_sql);
        PreparedStatement insert_migration(
            database, "INSERT INTO schema_migrations "
                      "(version, name, checksum, applied_at_unix_seconds) VALUES (?, ?, ?, ?)");
        bind_int64(insert_migration.get(), 1, migration.version);
        bind_text(insert_migration.get(), 2, migration.name);
        bind_text(insert_migration.get(), 3, expected_checksum);
        bind_int64(insert_migration.get(), 4, now_unix_seconds());
        insert_migration.step_done();
      }

      exec(database, "COMMIT");
    } catch (const BackendError&) {
      if (began) {
        rollback_no_throw(database);
      }
      throw;
    } catch (const std::exception& error) {
      if (began) {
        rollback_no_throw(database);
      }
      throw MigrationFailure(error.what());
    }
  }

  SchemaVersion SqliteBackend::current_version() const {
    auto connection = state_->open_connection();
    return current_schema_version(connection->get());
  }

  std::unique_ptr<ITransaction> SqliteBackend::begin(IsolationLevel isolation_level) {
    auto transaction =
        std::unique_ptr<SqliteTransaction>(new SqliteTransaction(state_, isolation_level));
    for (const auto& [unused_type, factory] : repository_factories_) {
      (void)unused_type;
      factory(*transaction);
    }
    return transaction;
  }

  Capabilities SqliteBackend::caps() const {
    Capabilities capabilities;
    capabilities.json_path_equality = true;
    capabilities.json_path_indexes = false;
    capabilities.native_uuid = false;
    return capabilities;
  }

  void SqliteBackend::fail_next_audit_append_for_tests() {
    std::scoped_lock lock(state_->mutex);
    state_->fail_next_audit_append = true;
  }

  std::size_t SqliteBackend::audit_event_count_for_tests() const {
    auto connection = state_->open_connection();
    ensure_metadata_tables(connection->get());
    PreparedStatement statement(connection->get(), "SELECT COUNT(*) FROM audit_events");
    if (!statement.step_row()) {
      return 0;
    }
    return static_cast<std::size_t>(sqlite3_column_int64(statement.get(), 0));
  }

  void SqliteBackend::downgrade_to_zero_for_tests() {
    auto connection = state_->open_connection();
    sqlite3* database = connection->get();
    bool began = false;
    try {
      exec(database, "BEGIN IMMEDIATE");
      began = true;
      ensure_metadata_tables(database);
      exec(database, "DELETE FROM schema_migrations");
      exec(database, "COMMIT");
    } catch (...) {
      if (began) {
        rollback_no_throw(database);
      }
      throw;
    }
  }

} // namespace fmgr::storage
