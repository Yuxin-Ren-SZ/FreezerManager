// SPDX-License-Identifier: AGPL-3.0-or-later

#include "storage/postgres/PostgresBackend.h"

#include "audit/CanonicalJson.h"
#include "core/timestamp.h"

#include <pqxx/pqxx>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace fmgr::storage {
  namespace {

    // ---- Error mapping ----

    [[noreturn]] void throw_pqxx_error(const pqxx::sql_error& error) {
      const std::string_view state = error.sqlstate();
      if (state == "23505") { throw UniqueViolation(error.what()); }
      if (state == "23503") { throw ForeignKeyViolation(error.what()); }
      if (state == "23514" || state == "23502" || state == "23000") { throw ConstraintViolation(error.what()); }
      if (state == "40001" || state == "40P01") { throw SerializationFailure(error.what()); }
      // Connection-class errors (08xxx) and admin shutdown (57P01) → Unavailable.
      if (state.size() >= 2 && state.substr(0, 2) == "08") { throw Unavailable(error.what()); }
      if (state == "57P01") { throw Unavailable(error.what()); }
      // Unrecognised SQLSTATE — fall back to ConstraintViolation (safest for data-layer errors).
      throw ConstraintViolation(error.what());
    }

    // ---- UUID generation ----

    [[nodiscard]] std::string generate_random_uuid() {
      if (sodium_init() < 0) {
        throw std::runtime_error("libsodium initialisation failed");
      }
      std::array<unsigned char, 16> bytes{};
      randombytes_buf(bytes.data(), bytes.size());
      bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
      bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);
      std::array<char, 37> buf{};
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
      std::snprintf(buf.data(), buf.size(),
                    "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                    bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                    bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14],
                    bytes[15]);
      return {buf.data()};
    }

    // ---- Migrations ----

    [[nodiscard]] std::vector<PostgresMigration> default_migrations() {
      return {
          {.version = 1, .name = "0001_init", .up_sql = R"sql(
CREATE TABLE IF NOT EXISTS audit_events (
  id               TEXT   PRIMARY KEY,
  at_micros        BIGINT NOT NULL,
  actor_user_id    TEXT   NOT NULL,
  actor_session_id TEXT   NOT NULL,
  lab_id           TEXT,
  action           TEXT   NOT NULL,
  entity_kind      TEXT   NOT NULL,
  entity_id        TEXT,
  before_json      TEXT   NOT NULL DEFAULT '{}',
  after_json       TEXT   NOT NULL DEFAULT '{}',
  request_id       TEXT   NOT NULL,
  prev_hash        TEXT   NOT NULL,
  this_hash        TEXT   NOT NULL UNIQUE
);
CREATE INDEX IF NOT EXISTS audit_events_at_idx          ON audit_events(at_micros);
CREATE INDEX IF NOT EXISTS audit_events_actor_idx       ON audit_events(actor_user_id);
CREATE INDEX IF NOT EXISTS audit_events_entity_kind_idx ON audit_events(entity_kind);
CREATE INDEX IF NOT EXISTS audit_events_lab_idx         ON audit_events(lab_id);

CREATE OR REPLACE FUNCTION fmgr_audit_immutable()
RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  RAISE EXCEPTION 'audit_events is append-only: % are not permitted', TG_OP;
END; $$;

DO $$ BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_trigger WHERE tgname = 'audit_events_no_update') THEN
    CREATE TRIGGER audit_events_no_update
      BEFORE UPDATE ON audit_events
      FOR EACH ROW EXECUTE FUNCTION fmgr_audit_immutable();
  END IF;
  IF NOT EXISTS (SELECT 1 FROM pg_trigger WHERE tgname = 'audit_events_no_delete') THEN
    CREATE TRIGGER audit_events_no_delete
      BEFORE DELETE ON audit_events
      FOR EACH ROW EXECUTE FUNCTION fmgr_audit_immutable();
  END IF;
END $$;
)sql"},
          {.version = 2, .name = "0002_identity", .up_sql = R"sql(
CREATE TABLE IF NOT EXISTS labs (
  id                 TEXT    PRIMARY KEY,
  name               TEXT    NOT NULL CHECK (length(name) > 0),
  contact            TEXT    NOT NULL,
  created_at_micros  BIGINT  NOT NULL,
  settings_json      JSONB   NOT NULL DEFAULT '{}',
  is_phi_enabled     BOOLEAN NOT NULL DEFAULT FALSE,
  archived_at_micros BIGINT
);
CREATE TABLE IF NOT EXISTS users (
  id                  TEXT   PRIMARY KEY,
  primary_email       TEXT   NOT NULL CHECK (length(primary_email) > 2),
  display_name        TEXT   NOT NULL CHECK (length(display_name) > 0),
  status              TEXT   NOT NULL CHECK (status IN ('active', 'disabled')),
  created_at_micros   BIGINT NOT NULL,
  auth_bindings_json  JSONB  NOT NULL DEFAULT '[]',
  totp_secret_enc     TEXT,
  default_lab_id      TEXT   REFERENCES labs(id) DEFERRABLE INITIALLY DEFERRED
);
CREATE UNIQUE INDEX IF NOT EXISTS users_primary_email_lower_unique
  ON users(lower(primary_email));
CREATE TABLE IF NOT EXISTS lab_memberships (
  user_id            TEXT   NOT NULL REFERENCES users(id) DEFERRABLE INITIALLY DEFERRED,
  lab_id             TEXT   NOT NULL REFERENCES labs(id)  DEFERRABLE INITIALLY DEFERRED,
  scope_filters_json JSONB  NOT NULL DEFAULT '{}',
  invited_by         TEXT   REFERENCES users(id) DEFERRABLE INITIALLY DEFERRED,
  joined_at_micros   BIGINT NOT NULL,
  revoked_at_micros  BIGINT,
  role_id            TEXT,
  PRIMARY KEY (user_id, lab_id)
);
)sql"},
          {.version = 3, .name = "0003_roles", .up_sql = R"sql(
CREATE TABLE IF NOT EXISTS permissions (
  key         TEXT PRIMARY KEY,
  description TEXT NOT NULL CHECK (length(description) > 0)
);
CREATE TABLE IF NOT EXISTS roles (
  id                 TEXT    PRIMARY KEY,
  lab_id             TEXT    REFERENCES labs(id) DEFERRABLE INITIALLY DEFERRED,
  kind               TEXT    NOT NULL CHECK (kind IN
    ('system_admin','lab_admin','member','read_only','api_client','custom')),
  name               TEXT    NOT NULL CHECK (length(name) > 0),
  description        TEXT    NOT NULL,
  is_builtin         BOOLEAN NOT NULL DEFAULT FALSE,
  created_at_micros  BIGINT  NOT NULL,
  archived_at_micros BIGINT
);
CREATE UNIQUE INDEX IF NOT EXISTS roles_builtin_name_unique
  ON roles(name) WHERE lab_id IS NULL;
CREATE UNIQUE INDEX IF NOT EXISTS roles_lab_name_unique
  ON roles(lab_id, name) WHERE lab_id IS NOT NULL;
CREATE TABLE IF NOT EXISTS role_permissions (
  role_id        TEXT NOT NULL REFERENCES roles(id)       DEFERRABLE INITIALLY DEFERRED,
  permission_key TEXT NOT NULL REFERENCES permissions(key) DEFERRABLE INITIALLY DEFERRED,
  PRIMARY KEY (role_id, permission_key)
);
DO $$
BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_constraint WHERE conname = 'lab_memberships_role_id_fk'
  ) THEN
    ALTER TABLE lab_memberships
      ADD CONSTRAINT lab_memberships_role_id_fk
      FOREIGN KEY (role_id) REFERENCES roles(id) DEFERRABLE INITIALLY DEFERRED;
  END IF;
END $$;
INSERT INTO permissions (key, description) VALUES
  ('sample.read','Read sample records.'),
  ('sample.write','Create or modify samples.'),
  ('sample.checkout','Check samples in or out.'),
  ('sample.delete_soft','Tombstone a sample.'),
  ('sample.delete_hard','Permanently delete tombstoned sample (SystemAdmin only).'),
  ('box.configure','Define or modify boxes and box types.'),
  ('freezer.configure','Configure freezers and storage layout.'),
  ('custom_field.define','Define custom field schemas.'),
  ('item_type.define','Define item-type taxonomy.'),
  ('user.invite','Invite or revoke lab members.'),
  ('user.manage_roles','Assign roles to lab members.'),
  ('audit.read','Browse audit log entries.'),
  ('audit.export','Export signed audit log files.'),
  ('backup.run','Trigger a backup or restore-drill.'),
  ('share.request','Open a cross-lab share request.'),
  ('share.approve','Approve or reject a share request.'),
  ('phi.read','Read PHI-tagged custom fields.'),
  ('lab.configure','Modify lab settings.'),
  ('lab.enable_phi','Toggle PHI mode on a lab.'),
  ('key.rotate','Rotate cryptographic keys.'),
  ('session.revoke','Revoke another user''s sessions.')
  ON CONFLICT (key) DO NOTHING;
INSERT INTO roles (id,lab_id,kind,name,description,is_builtin,created_at_micros) VALUES
  ('00000000-0000-0000-0000-000000000001',NULL,'system_admin','SystemAdmin',
   'Deployment-wide administrator (PHI access requires separate grant).',TRUE,0),
  ('00000000-0000-0000-0000-000000000002',NULL,'lab_admin','LabAdmin',
   'Full administrative control of one lab.',TRUE,0),
  ('00000000-0000-0000-0000-000000000003',NULL,'member','Member',
   'Read+write samples within scoped freezers/projects.',TRUE,0),
  ('00000000-0000-0000-0000-000000000004',NULL,'read_only','ReadOnly',
   'Read-only access; cannot check out.',TRUE,0),
  ('00000000-0000-0000-0000-000000000005',NULL,'api_client','ApiClient',
   'Conservative machine-client default; expand via custom roles.',TRUE,0)
  ON CONFLICT (id) DO NOTHING;
INSERT INTO role_permissions (role_id,permission_key) VALUES
  ('00000000-0000-0000-0000-000000000001','sample.read'),
  ('00000000-0000-0000-0000-000000000001','sample.write'),
  ('00000000-0000-0000-0000-000000000001','sample.checkout'),
  ('00000000-0000-0000-0000-000000000001','sample.delete_soft'),
  ('00000000-0000-0000-0000-000000000001','sample.delete_hard'),
  ('00000000-0000-0000-0000-000000000001','box.configure'),
  ('00000000-0000-0000-0000-000000000001','freezer.configure'),
  ('00000000-0000-0000-0000-000000000001','custom_field.define'),
  ('00000000-0000-0000-0000-000000000001','item_type.define'),
  ('00000000-0000-0000-0000-000000000001','user.invite'),
  ('00000000-0000-0000-0000-000000000001','user.manage_roles'),
  ('00000000-0000-0000-0000-000000000001','audit.read'),
  ('00000000-0000-0000-0000-000000000001','audit.export'),
  ('00000000-0000-0000-0000-000000000001','backup.run'),
  ('00000000-0000-0000-0000-000000000001','share.request'),
  ('00000000-0000-0000-0000-000000000001','share.approve'),
  ('00000000-0000-0000-0000-000000000001','lab.configure'),
  ('00000000-0000-0000-0000-000000000001','lab.enable_phi'),
  ('00000000-0000-0000-0000-000000000001','key.rotate'),
  ('00000000-0000-0000-0000-000000000001','session.revoke'),
  ('00000000-0000-0000-0000-000000000002','sample.read'),
  ('00000000-0000-0000-0000-000000000002','sample.write'),
  ('00000000-0000-0000-0000-000000000002','sample.checkout'),
  ('00000000-0000-0000-0000-000000000002','sample.delete_soft'),
  ('00000000-0000-0000-0000-000000000002','box.configure'),
  ('00000000-0000-0000-0000-000000000002','freezer.configure'),
  ('00000000-0000-0000-0000-000000000002','custom_field.define'),
  ('00000000-0000-0000-0000-000000000002','item_type.define'),
  ('00000000-0000-0000-0000-000000000002','user.invite'),
  ('00000000-0000-0000-0000-000000000002','user.manage_roles'),
  ('00000000-0000-0000-0000-000000000002','audit.read'),
  ('00000000-0000-0000-0000-000000000002','audit.export'),
  ('00000000-0000-0000-0000-000000000002','share.request'),
  ('00000000-0000-0000-0000-000000000002','share.approve'),
  ('00000000-0000-0000-0000-000000000002','lab.configure'),
  ('00000000-0000-0000-0000-000000000003','sample.read'),
  ('00000000-0000-0000-0000-000000000003','sample.write'),
  ('00000000-0000-0000-0000-000000000003','sample.checkout'),
  ('00000000-0000-0000-0000-000000000003','sample.delete_soft'),
  ('00000000-0000-0000-0000-000000000003','share.request'),
  ('00000000-0000-0000-0000-000000000004','sample.read'),
  ('00000000-0000-0000-0000-000000000004','audit.read'),
  ('00000000-0000-0000-0000-000000000005','sample.read'),
  ('00000000-0000-0000-0000-000000000005','sample.checkout')
  ON CONFLICT DO NOTHING;
)sql"},
          {.version = 4, .name = "0004_layout", .up_sql = R"sql(
CREATE TABLE IF NOT EXISTS storage_containers (
  id                 TEXT   PRIMARY KEY,
  lab_id             TEXT   NOT NULL REFERENCES labs(id) DEFERRABLE INITIALLY DEFERRED,
  parent_id          TEXT   REFERENCES storage_containers(id) DEFERRABLE INITIALLY DEFERRED,
  kind               TEXT   NOT NULL CHECK (kind IN
    ('compartment','shelf','rack','drawer','custom')),
  name               TEXT   NOT NULL CHECK (length(name) > 0),
  label              TEXT   NOT NULL,
  ordering_index     BIGINT NOT NULL DEFAULT 0,
  capacity_hint_json JSONB  NOT NULL DEFAULT '{}',
  created_at_micros  BIGINT NOT NULL,
  archived_at_micros BIGINT,
  CHECK (id <> parent_id)
);
CREATE INDEX IF NOT EXISTS storage_containers_parent_idx ON storage_containers(parent_id);
CREATE INDEX IF NOT EXISTS storage_containers_lab_idx    ON storage_containers(lab_id);
CREATE TABLE IF NOT EXISTS freezers (
  id                 TEXT             PRIMARY KEY,
  lab_id             TEXT             NOT NULL REFERENCES labs(id) DEFERRABLE INITIALLY DEFERRED,
  name               TEXT             NOT NULL CHECK (length(name) > 0),
  location           TEXT             NOT NULL,
  model              TEXT             NOT NULL,
  temp_target_c      DOUBLE PRECISION,
  layout_root_id     TEXT             NOT NULL
    REFERENCES storage_containers(id) DEFERRABLE INITIALLY DEFERRED,
  created_at_micros  BIGINT           NOT NULL,
  archived_at_micros BIGINT
);
CREATE INDEX IF NOT EXISTS freezers_lab_id_idx ON freezers(lab_id);
CREATE UNIQUE INDEX IF NOT EXISTS freezers_lab_name_unique
  ON freezers(lab_id, name) WHERE archived_at_micros IS NULL;
ALTER TABLE storage_containers ENABLE ROW LEVEL SECURITY;
ALTER TABLE storage_containers FORCE ROW LEVEL SECURITY;
CREATE POLICY fmgr_storage_containers_lab ON storage_containers USING (
  lab_id = ANY(string_to_array(current_setting('app.current_lab_ids', true), ',')));
ALTER TABLE freezers ENABLE ROW LEVEL SECURITY;
ALTER TABLE freezers FORCE ROW LEVEL SECURITY;
CREATE POLICY fmgr_freezers_lab ON freezers USING (
  lab_id = ANY(string_to_array(current_setting('app.current_lab_ids', true), ',')));
)sql"},
          {.version = 5, .name = "0005_box_types", .up_sql = R"sql(
CREATE TABLE IF NOT EXISTS container_types (
  id                    TEXT   PRIMARY KEY,
  lab_id                TEXT   NOT NULL REFERENCES labs(id) DEFERRABLE INITIALLY DEFERRED,
  name                  TEXT   NOT NULL CHECK (length(name) > 0),
  size_class            TEXT   NOT NULL CHECK (length(size_class) > 0),
  outer_dimensions_json JSONB  NOT NULL DEFAULT '{}',
  material              TEXT   NOT NULL,
  supplier_sku          TEXT   NOT NULL,
  created_at_micros     BIGINT NOT NULL,
  archived_at_micros    BIGINT
);
CREATE INDEX IF NOT EXISTS container_types_lab_idx           ON container_types(lab_id);
CREATE INDEX IF NOT EXISTS container_types_lab_size_class_idx ON container_types(lab_id,size_class);
CREATE TABLE IF NOT EXISTS box_types (
  id                 TEXT   PRIMARY KEY,
  lab_id             TEXT   NOT NULL REFERENCES labs(id) DEFERRABLE INITIALLY DEFERRED,
  name               TEXT   NOT NULL CHECK (length(name) > 0),
  manufacturer       TEXT   NOT NULL,
  sku                TEXT   NOT NULL,
  created_at_micros  BIGINT NOT NULL,
  archived_at_micros BIGINT
);
CREATE INDEX IF NOT EXISTS box_types_lab_idx ON box_types(lab_id);
CREATE UNIQUE INDEX IF NOT EXISTS box_types_lab_name_unique
  ON box_types(lab_id, name) WHERE archived_at_micros IS NULL;
CREATE TABLE IF NOT EXISTS box_type_positions (
  box_type_id TEXT    NOT NULL
    REFERENCES box_types(id) ON DELETE CASCADE DEFERRABLE INITIALLY DEFERRED,
  label       TEXT    NOT NULL CHECK (length(label) > 0),
  row_index   INTEGER NOT NULL CHECK (row_index >= 0),
  col_index   INTEGER NOT NULL CHECK (col_index >= 0),
  z_index     INTEGER CHECK (z_index IS NULL OR z_index >= 0),
  PRIMARY KEY (box_type_id, label)
);
CREATE TABLE IF NOT EXISTS box_type_position_accepts (
  box_type_id    TEXT NOT NULL,
  position_label TEXT NOT NULL,
  size_class     TEXT NOT NULL CHECK (length(size_class) > 0),
  PRIMARY KEY (box_type_id, position_label, size_class),
  FOREIGN KEY (box_type_id, position_label)
    REFERENCES box_type_positions(box_type_id, label)
    ON DELETE CASCADE DEFERRABLE INITIALLY DEFERRED
);
ALTER TABLE container_types ENABLE ROW LEVEL SECURITY;
ALTER TABLE container_types FORCE ROW LEVEL SECURITY;
CREATE POLICY fmgr_container_types_lab ON container_types USING (
  lab_id = ANY(string_to_array(current_setting('app.current_lab_ids', true), ',')));
ALTER TABLE box_types ENABLE ROW LEVEL SECURITY;
ALTER TABLE box_types FORCE ROW LEVEL SECURITY;
CREATE POLICY fmgr_box_types_lab ON box_types USING (
  lab_id = ANY(string_to_array(current_setting('app.current_lab_ids', true), ',')));
)sql"},
          {.version = 6, .name = "0006_boxes", .up_sql = R"sql(
CREATE TABLE IF NOT EXISTS boxes (
  id                   TEXT   PRIMARY KEY,
  lab_id               TEXT   NOT NULL REFERENCES labs(id) DEFERRABLE INITIALLY DEFERRED,
  box_type_id          TEXT   NOT NULL REFERENCES box_types(id) DEFERRABLE INITIALLY DEFERRED,
  storage_container_id TEXT   NOT NULL
    REFERENCES storage_containers(id) DEFERRABLE INITIALLY DEFERRED,
  label                TEXT   NOT NULL CHECK (length(label) > 0),
  serial               TEXT,
  barcode              TEXT,
  created_at_micros    BIGINT NOT NULL,
  archived_at_micros   BIGINT
);
CREATE INDEX IF NOT EXISTS boxes_lab_idx              ON boxes(lab_id);
CREATE INDEX IF NOT EXISTS boxes_storage_container_idx ON boxes(storage_container_id);
CREATE INDEX IF NOT EXISTS boxes_box_type_idx         ON boxes(box_type_id);
CREATE UNIQUE INDEX IF NOT EXISTS boxes_lab_label_unique
  ON boxes(lab_id, label) WHERE archived_at_micros IS NULL;
ALTER TABLE boxes ENABLE ROW LEVEL SECURITY;
ALTER TABLE boxes FORCE ROW LEVEL SECURITY;
CREATE POLICY fmgr_boxes_lab ON boxes USING (
  lab_id = ANY(string_to_array(current_setting('app.current_lab_ids', true), ',')));
)sql"},
          {.version = 7, .name = "0007_item_types", .up_sql = R"sql(
CREATE TABLE IF NOT EXISTS item_types (
  id                 TEXT   PRIMARY KEY,
  lab_id             TEXT   NOT NULL REFERENCES labs(id)       DEFERRABLE INITIALLY DEFERRED,
  parent_id          TEXT   REFERENCES item_types(id)          DEFERRABLE INITIALLY DEFERRED,
  name               TEXT   NOT NULL CHECK (length(name) > 0),
  created_at_micros  BIGINT NOT NULL,
  archived_at_micros BIGINT,
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
    CHECK (scope_kind IN ('sample','box','freezer','container')),
  item_type_id       TEXT    REFERENCES item_types(id)          DEFERRABLE INITIALLY DEFERRED,
  key                TEXT    NOT NULL CHECK (length(key) > 0),
  label              TEXT    NOT NULL CHECK (length(label) > 0),
  data_type          TEXT    NOT NULL,
  required           BOOLEAN NOT NULL DEFAULT FALSE,
  validation_json    JSONB   NOT NULL DEFAULT '{}',
  indexed            BOOLEAN NOT NULL DEFAULT FALSE,
  is_phi             BOOLEAN NOT NULL DEFAULT FALSE,
  created_at_micros  BIGINT  NOT NULL,
  archived_at_micros BIGINT
);
CREATE INDEX IF NOT EXISTS cfd_lab_idx  ON custom_field_definitions(lab_id);
CREATE INDEX IF NOT EXISTS cfd_type_idx ON custom_field_definitions(item_type_id);
CREATE UNIQUE INDEX IF NOT EXISTS cfd_lab_scope_type_key_unique
  ON custom_field_definitions(lab_id, scope_kind, COALESCE(item_type_id,''), key)
  WHERE archived_at_micros IS NULL;
ALTER TABLE item_types ENABLE ROW LEVEL SECURITY;
ALTER TABLE item_types FORCE ROW LEVEL SECURITY;
CREATE POLICY fmgr_item_types_lab ON item_types USING (
  lab_id = ANY(string_to_array(current_setting('app.current_lab_ids', true), ',')));
ALTER TABLE custom_field_definitions ENABLE ROW LEVEL SECURITY;
ALTER TABLE custom_field_definitions FORCE ROW LEVEL SECURITY;
CREATE POLICY fmgr_cfd_lab ON custom_field_definitions USING (
  lab_id = ANY(string_to_array(current_setting('app.current_lab_ids', true), ',')));
)sql"},
          {.version = 8, .name = "0008_samples", .up_sql = R"sql(
CREATE TABLE IF NOT EXISTS projects (
  id                 TEXT   PRIMARY KEY,
  lab_id             TEXT   NOT NULL REFERENCES labs(id)  DEFERRABLE INITIALLY DEFERRED,
  name               TEXT   NOT NULL CHECK (length(name) > 0),
  owner_user_id      TEXT   NOT NULL REFERENCES users(id) DEFERRABLE INITIALLY DEFERRED,
  created_at_micros  BIGINT NOT NULL,
  archived_at_micros BIGINT
);
CREATE INDEX IF NOT EXISTS projects_lab_idx ON projects(lab_id);
CREATE UNIQUE INDEX IF NOT EXISTS projects_lab_name_unique
  ON projects(lab_id, name) WHERE archived_at_micros IS NULL;
CREATE TABLE IF NOT EXISTS samples (
  id                      TEXT  PRIMARY KEY,
  lab_id                  TEXT  NOT NULL REFERENCES labs(id)           DEFERRABLE INITIALLY DEFERRED,
  item_type_id            TEXT  NOT NULL REFERENCES item_types(id)     DEFERRABLE INITIALLY DEFERRED,
  name                    TEXT  NOT NULL CHECK (length(name) > 0),
  barcode                 TEXT,
  container_type_id       TEXT  REFERENCES container_types(id)         DEFERRABLE INITIALLY DEFERRED,
  box_id                  TEXT  REFERENCES boxes(id)                   DEFERRABLE INITIALLY DEFERRED,
  position_label          TEXT,
  volume_value            BIGINT,
  volume_unit             TEXT,
  mass_value              BIGINT,
  mass_unit               TEXT,
  status                  TEXT  NOT NULL DEFAULT 'active'
    CHECK (status IN ('active','checked_out','depleted','destroyed','tombstoned')),
  parent_sample_id        TEXT  REFERENCES samples(id)                 DEFERRABLE INITIALLY DEFERRED,
  created_by              TEXT  NOT NULL,
  created_at_micros       BIGINT NOT NULL,
  last_modified_by        TEXT  NOT NULL,
  last_modified_at_micros BIGINT NOT NULL,
  custom_fields_json      JSONB NOT NULL DEFAULT '{}',
  phi_fields_enc_json     JSONB NOT NULL DEFAULT '{}',
  CHECK ((box_id IS NULL) = (position_label IS NULL))
);
CREATE INDEX IF NOT EXISTS samples_lab_idx    ON samples(lab_id);
CREATE INDEX IF NOT EXISTS samples_box_idx    ON samples(box_id);
CREATE INDEX IF NOT EXISTS samples_parent_idx ON samples(parent_sample_id);
CREATE INDEX IF NOT EXISTS samples_status_idx ON samples(status);
CREATE UNIQUE INDEX IF NOT EXISTS samples_position_unique
  ON samples(box_id, position_label)
  WHERE status IN ('active','checked_out') AND box_id IS NOT NULL;
CREATE TABLE IF NOT EXISTS sample_projects (
  sample_id  TEXT NOT NULL REFERENCES samples(id)  DEFERRABLE INITIALLY DEFERRED,
  project_id TEXT NOT NULL REFERENCES projects(id) DEFERRABLE INITIALLY DEFERRED,
  PRIMARY KEY (sample_id, project_id)
);
CREATE TABLE IF NOT EXISTS checkout_events (
  id             TEXT   PRIMARY KEY,
  sample_id      TEXT   NOT NULL REFERENCES samples(id) DEFERRABLE INITIALLY DEFERRED,
  lab_id         TEXT   NOT NULL REFERENCES labs(id)    DEFERRABLE INITIALLY DEFERRED,
  user_id        TEXT   NOT NULL REFERENCES users(id)   DEFERRABLE INITIALLY DEFERRED,
  action         TEXT   NOT NULL CHECK (action IN ('out','in','destroy')),
  reason         TEXT,
  at_micros      BIGINT NOT NULL,
  volume_delta   BIGINT,
  volume_unit    TEXT,
  location_after TEXT
);
CREATE INDEX IF NOT EXISTS checkout_events_sample_idx ON checkout_events(sample_id);
CREATE INDEX IF NOT EXISTS checkout_events_lab_idx    ON checkout_events(lab_id);
CREATE INDEX IF NOT EXISTS checkout_events_user_idx   ON checkout_events(user_id);
ALTER TABLE projects ENABLE ROW LEVEL SECURITY;
ALTER TABLE projects FORCE ROW LEVEL SECURITY;
CREATE POLICY fmgr_projects_lab ON projects USING (
  lab_id = ANY(string_to_array(current_setting('app.current_lab_ids', true), ',')));
ALTER TABLE samples ENABLE ROW LEVEL SECURITY;
ALTER TABLE samples FORCE ROW LEVEL SECURITY;
CREATE POLICY fmgr_samples_lab ON samples USING (
  lab_id = ANY(string_to_array(current_setting('app.current_lab_ids', true), ',')));
ALTER TABLE checkout_events ENABLE ROW LEVEL SECURITY;
ALTER TABLE checkout_events FORCE ROW LEVEL SECURITY;
CREATE POLICY fmgr_checkout_events_lab ON checkout_events USING (
  lab_id = ANY(string_to_array(current_setting('app.current_lab_ids', true), ',')));
)sql"},
          {.version = 9, .name = "0009_share_requests", .up_sql = R"sql(
CREATE TABLE IF NOT EXISTS share_requests (
  id                TEXT   PRIMARY KEY,
  source_lab_id     TEXT   NOT NULL REFERENCES labs(id)  DEFERRABLE INITIALLY DEFERRED,
  target_lab_id     TEXT   NOT NULL REFERENCES labs(id)  DEFERRABLE INITIALLY DEFERRED,
  requested_by      TEXT   NOT NULL REFERENCES users(id) DEFERRABLE INITIALLY DEFERRED,
  scope_json        TEXT   NOT NULL DEFAULT '{}',
  status            TEXT   NOT NULL DEFAULT 'pending'
    CHECK (status IN ('pending','approved','rejected','revoked')),
  created_at_micros BIGINT NOT NULL,
  decided_at_micros BIGINT,
  CHECK (source_lab_id != target_lab_id)
);
CREATE INDEX IF NOT EXISTS share_requests_source_lab_idx ON share_requests(source_lab_id);
CREATE INDEX IF NOT EXISTS share_requests_target_lab_idx ON share_requests(target_lab_id);
CREATE INDEX IF NOT EXISTS share_requests_status_idx     ON share_requests(status);
CREATE TABLE IF NOT EXISTS share_request_approvals (
  share_request_id  TEXT   NOT NULL REFERENCES share_requests(id) DEFERRABLE INITIALLY DEFERRED,
  approver_role     TEXT   NOT NULL
    CHECK (approver_role IN ('source_admin','target_admin','system_admin')),
  approver_user_id  TEXT   NOT NULL REFERENCES users(id) DEFERRABLE INITIALLY DEFERRED,
  decided_at_micros BIGINT NOT NULL,
  note              TEXT,
  PRIMARY KEY (share_request_id, approver_role)
);
CREATE INDEX IF NOT EXISTS share_request_approvals_request_idx
  ON share_request_approvals(share_request_id);
)sql"},
          {.version = 10, .name = "0010_sessions", .up_sql = R"sql(
CREATE TABLE IF NOT EXISTS sessions (
  id                  TEXT   PRIMARY KEY,
  user_id             TEXT   NOT NULL REFERENCES users(id) DEFERRABLE INITIALLY DEFERRED,
  token_hash          TEXT   NOT NULL CHECK (length(token_hash) > 0),
  token_prefix        TEXT   NOT NULL CHECK (length(token_prefix) > 0),
  created_at_micros   BIGINT NOT NULL,
  last_seen_at_micros BIGINT NOT NULL,
  ip                  TEXT,
  user_agent          TEXT,
  revoked_at_micros   BIGINT
);
CREATE UNIQUE INDEX IF NOT EXISTS sessions_prefix_active_unique
  ON sessions(token_prefix) WHERE revoked_at_micros IS NULL;
CREATE INDEX IF NOT EXISTS sessions_user_id_idx ON sessions(user_id);
CREATE TABLE IF NOT EXISTS api_tokens (
  id                TEXT   PRIMARY KEY,
  user_id           TEXT   NOT NULL REFERENCES users(id)  DEFERRABLE INITIALLY DEFERRED,
  lab_id            TEXT   REFERENCES labs(id)            DEFERRABLE INITIALLY DEFERRED,
  name              TEXT   NOT NULL CHECK (length(name) > 0),
  scope_json        TEXT   NOT NULL DEFAULT '[]',
  token_hash        TEXT   NOT NULL CHECK (length(token_hash) > 0),
  token_prefix      TEXT   NOT NULL CHECK (length(token_prefix) > 0),
  created_at_micros BIGINT NOT NULL,
  expires_at_micros BIGINT,
  revoked_at_micros BIGINT
);
CREATE UNIQUE INDEX IF NOT EXISTS api_tokens_prefix_active_unique
  ON api_tokens(token_prefix) WHERE revoked_at_micros IS NULL;
CREATE INDEX IF NOT EXISTS api_tokens_user_id_idx ON api_tokens(user_id);
CREATE INDEX IF NOT EXISTS api_tokens_lab_id_idx  ON api_tokens(lab_id);
)sql"},
          // SQLite needed to replace migration 1's stub; Postgres started with the correct schema.
          {.version = 11,
           .name = "0011_audit_events_full",
           .up_sql = "SELECT 1; -- no-op: audit_events already has full schema from migration 1"},
          {.version = 12, .name = "0012_sessions_mfa", .up_sql = R"sql(
ALTER TABLE sessions ADD COLUMN IF NOT EXISTS mfa_complete BOOLEAN NOT NULL DEFAULT TRUE;
)sql"},
      };
    }

    // BLAKE2b-256 hex digest of `data` via libsodium. Stable across platforms/compilers.
    [[nodiscard]] std::string blake2b_hex(const std::string& data) {
      if (sodium_init() < 0) {
        throw std::runtime_error("libsodium initialisation failed");
      }
      std::array<unsigned char, crypto_generichash_BYTES> hash{};
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      crypto_generichash(hash.data(), hash.size(),
                         reinterpret_cast<const unsigned char*>(data.data()), data.size(),
                         nullptr, 0);
      std::string hex;
      hex.reserve(hash.size() * 2);
      for (const unsigned char byte : hash) {
        constexpr char k_nibbles[] = "0123456789abcdef";
        hex += k_nibbles[byte >> 4U];
        hex += k_nibbles[byte & 0x0fU];
      }
      return hex;
    }

    [[nodiscard]] std::string checksum(const PostgresMigration& migration) {
      return blake2b_hex(std::to_string(migration.version) + ":" + migration.name + ":" +
                         migration.up_sql);
    }

  } // namespace

  std::vector<PostgresMigration> default_postgres_migrations() {
    return default_migrations();
  }

  // ---- PostgresBackendState ----

  struct PostgresBackendState {
    explicit PostgresBackendState(PostgresBackendOptions input_options)
        : options(std::move(input_options)) {
      if (options.migrations.empty()) {
        options.migrations = default_migrations();
      }
      connections.reserve(options.pool_size);
      connection_available.assign(options.pool_size, true);
      for (std::size_t i = 0; i < options.pool_size; ++i) {
        connections.emplace_back(std::make_unique<pqxx::connection>(options.connection_string));
      }
    }

    [[nodiscard]] std::size_t acquire(std::chrono::milliseconds timeout) {
      std::unique_lock lock(pool_mutex);
      const bool found = pool_cv.wait_for(lock, timeout, [this] {
        return std::ranges::any_of(connection_available, [](bool available) { return available; });
      });
      if (!found) { throw Unavailable("postgres connection pool exhausted"); }
      for (std::size_t i = 0; i < connection_available.size(); ++i) {
        if (connection_available[i]) {
          connection_available[i] = false;
          // Health-check: reconnect if the connection dropped (DB restart, idle timeout, etc.).
          if (!connections.at(i)->is_open()) {
            try {
              connections.at(i) = std::make_unique<pqxx::connection>(options.connection_string);
            } catch (const std::exception&) {
              connection_available[i] = true; // return to pool on reconnect failure
              pool_cv.notify_one();
              throw Unavailable("postgres reconnect failed");
            }
          }
          return i;
        }
      }
      throw Unavailable("postgres connection pool exhausted");
    }

    void release(std::size_t idx) {
      {
        std::scoped_lock lock(pool_mutex);
        connection_available[idx] = true;
      }
      pool_cv.notify_one();
    }

    PostgresBackendOptions options;
    std::vector<std::unique_ptr<pqxx::connection>> connections;
    std::vector<bool> connection_available;
    std::mutex pool_mutex;
    std::condition_variable pool_cv;
    std::mutex audit_mutex;
    bool fail_next_audit_append{false};
  };

  // ---- PostgresTransaction::Impl ----

  class PostgresTransaction::Impl {
  public:
    Impl(std::shared_ptr<PostgresBackendState> input_state, IsolationLevel input_isolation,
         std::size_t input_conn_idx)
        : state(std::move(input_state)), isolation(input_isolation), conn_idx(input_conn_idx) {
      // Emplace work referencing the acquired connection.
      work.emplace(*state->connections.at(conn_idx));
    }

    // Destroy the pqxx::work (rolls back if uncommitted) then release connection.
    ~Impl() {
      work.reset(); // abort if still active
      if (!released) {
        state->release(conn_idx);
        released = true;
      }
    }

    struct AuditMutation {
      std::string entity_kind;
      std::string entity_id;
      std::string action;
      MutationContext context;
    };

    std::shared_ptr<PostgresBackendState> state;
    IsolationLevel isolation;
    std::size_t conn_idx;
    std::optional<pqxx::work> work; // reset explicitly before releasing connection
    std::vector<AuditMutation> audit_mutations;
    bool completed{false};
    bool released{false};
  };

  // ---- PostgresTransaction ----

  PostgresTransaction::PostgresTransaction(const std::shared_ptr<PostgresBackendState>& state,
                                           IsolationLevel isolation_level) {
    const std::size_t idx = state->acquire(state->options.pool_acquire_timeout);
    // Pass state by copy so that if Impl ctor throws, the outer 'state' is still valid
    // and we can release the acquired connection manually.
    try {
      impl_ = std::make_unique<Impl>(state, isolation_level, idx);
    } catch (...) {
      state->release(idx);
      throw;
    }
    // impl_ is live; its destructor will release the connection on further exceptions.
    try {
      if (isolation_level == IsolationLevel::Serializable) {
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        impl_->work->exec("SET TRANSACTION ISOLATION LEVEL SERIALIZABLE");
      } else if (isolation_level == IsolationLevel::RepeatableRead) {
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        impl_->work->exec("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ");
      }
    } catch (const pqxx::sql_error& err) {
      throw_pqxx_error(err);
    }
  }

  PostgresTransaction::~PostgresTransaction() = default;

  pqxx::work& PostgresTransaction::work() {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    return *impl_->work;
  }

  void PostgresTransaction::set_session_var(std::string_view key, std::string_view value) {
    try {
      // set_config(name, value, is_local=true) scopes to the current transaction.
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      impl_->work->exec("SELECT set_config($1, $2, true)",
                        pqxx::params{"app." + std::string(key), std::string(value)});
    } catch (const pqxx::sql_error& err) {
      throw_pqxx_error(err);
    }
  }

  void PostgresTransaction::note_mutation(std::string entity_kind, std::string entity_id,
                                          const MutationContext& context, std::string action) {
    impl_->audit_mutations.push_back(Impl::AuditMutation{
        .entity_kind = std::move(entity_kind),
        .entity_id = std::move(entity_id),
        .action = std::move(action),
        .context = context,
    });
  }

  void PostgresTransaction::commit() {
    if (impl_->completed) {
      throw ConstraintViolation("postgres transaction already completed");
    }
    impl_->completed = true;

    try {
      {
        std::scoped_lock lock(impl_->state->audit_mutex);
        if (impl_->state->fail_next_audit_append && !impl_->audit_mutations.empty()) {
          impl_->state->fail_next_audit_append = false;
          throw ConstraintViolation("audit append failed");
        }
      }

      if (!impl_->audit_mutations.empty()) {
        if (sodium_init() < 0) { throw ConstraintViolation("libsodium initialisation failed"); }

        // Advisory lock serialises concurrent audit appends within this DB.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        impl_->work->exec("SELECT pg_advisory_xact_lock(8675309)");

        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        const auto last = impl_->work->exec(
            "SELECT this_hash FROM audit_events ORDER BY at_micros DESC, id DESC LIMIT 1");
        std::string prev_hash =
            last.empty() ? std::string(audit::zero_hash()) : last[0][0].as<std::string>();

        const auto now_micros =
            static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());

        for (const auto& mutation : impl_->audit_mutations) {
          const auto event_id = generate_random_uuid();
          const std::string before_str =
              mutation.context.before_json.has_value()
                  ? mutation.context.before_json->dump()
                  : "{}";
          const std::string after_str =
              mutation.context.after_json.has_value()
                  ? mutation.context.after_json->dump()
                  : "{}";
          const nlohmann::json lab_id_val =
              mutation.context.lab_id.has_value()
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

          pqxx::params audit_params;
          audit_params.append(event_id);
          audit_params.append(now_micros);
          audit_params.append(mutation.context.actor_user_id.to_string());
          audit_params.append(mutation.context.actor_session_id);
          audit_params.append(mutation.context.lab_id); // optional: binds NULL if empty
          audit_params.append(mutation.action);
          audit_params.append(mutation.entity_kind);
          audit_params.append(mutation.entity_id);
          audit_params.append(before_str);
          audit_params.append(after_str);
          audit_params.append(mutation.context.request_id);
          audit_params.append(prev_hash);
          audit_params.append(this_hash);
          // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
          impl_->work->exec(
              "INSERT INTO audit_events "
              "(id,at_micros,actor_user_id,actor_session_id,lab_id,action,"
              "entity_kind,entity_id,before_json,after_json,request_id,prev_hash,this_hash) "
              "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13)",
              audit_params);

          prev_hash = this_hash;
        }
      }

      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      impl_->work->commit();
    } catch (const pqxx::sql_error& err) {
      impl_->work.reset();
      impl_->state->release(impl_->conn_idx);
      impl_->released = true;
      throw_pqxx_error(err);
    } catch (...) {
      impl_->work.reset();
      impl_->state->release(impl_->conn_idx);
      impl_->released = true;
      throw;
    }

    impl_->work.reset();
    impl_->state->release(impl_->conn_idx);
    impl_->released = true;
  }

  void PostgresTransaction::rollback() {
    if (!impl_->completed) {
      impl_->completed = true;
      impl_->work.reset(); // pqxx::work destructor calls abort()
      impl_->state->release(impl_->conn_idx);
      impl_->released = true;
    }
  }

  // ---- PostgresBackend ----

  PostgresBackend::PostgresBackend(PostgresBackendOptions options)
      : state_(std::make_shared<PostgresBackendState>(std::move(options))) {}

  PostgresBackend::~PostgresBackend() = default;
  PostgresBackend::PostgresBackend(PostgresBackend&&) noexcept = default;
  PostgresBackend& PostgresBackend::operator=(PostgresBackend&&) noexcept = default;

  void PostgresBackend::migrate_to_latest() {
    const std::size_t idx = state_->acquire(state_->options.pool_acquire_timeout);
    auto& conn = *state_->connections.at(idx);
    try {
      pqxx::work txn(conn);
      txn.exec(R"sql(
CREATE TABLE IF NOT EXISTS schema_migrations (
  version                 INTEGER PRIMARY KEY,
  name                    TEXT    NOT NULL,
  checksum                TEXT    NOT NULL,
  applied_at_unix_seconds BIGINT  NOT NULL
))sql");

      auto migrations = state_->options.migrations;
      std::ranges::sort(migrations, {}, &PostgresMigration::version);

      int previous_version = 0;
      for (const auto& migration : migrations) {
        if (migration.version <= previous_version) {
          throw MigrationFailure("postgres migration versions must be strictly increasing");
        }
        previous_version = migration.version;

        const auto expected_cs = checksum(migration);
        const auto cs_result = txn.exec("SELECT checksum FROM schema_migrations WHERE version = $1",
                                        pqxx::params{migration.version});

        if (!cs_result.empty()) {
          const auto applied_cs = cs_result[0][0].as<std::string>();
          if (applied_cs != expected_cs) {
            throw MigrationFailure("postgres migration checksum changed: " + migration.name);
          }
          continue;
        }

        txn.exec(migration.up_sql);
        const auto applied_at =
            static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
        txn.exec("INSERT INTO schema_migrations "
                 "(version,name,checksum,applied_at_unix_seconds) VALUES ($1,$2,$3,$4)",
                 pqxx::params{migration.version, migration.name, expected_cs, applied_at});
      }
      txn.commit();
    } catch (const pqxx::sql_error& err) {
      state_->release(idx);
      throw MigrationFailure(err.what());
    } catch (const BackendError&) {
      state_->release(idx);
      throw;
    } catch (const std::exception& err) {
      state_->release(idx);
      throw MigrationFailure(err.what());
    }
    state_->release(idx);
  }

  SchemaVersion PostgresBackend::current_version() const {
    const std::size_t idx = state_->acquire(state_->options.pool_acquire_timeout);
    auto& conn = *state_->connections.at(idx);
    int version = 0;
    try {
      pqxx::work txn(conn);
      const auto result = txn.exec("SELECT COALESCE(MAX(version),0) FROM schema_migrations");
      if (!result.empty()) { version = result[0][0].as<int>(); }
      txn.commit();
    } catch (const pqxx::sql_error& err) {
      if (std::string_view(err.sqlstate()) != "42P01") {
        // 42P01 = "undefined_table" (schema_migrations not yet created) — treat as version 0.
        // Any other error (permissions denied, connection lost, etc.) is a real failure.
        state_->release(idx);
        throw Unavailable(err.what());
      }
    }
    state_->release(idx);
    return SchemaVersion{version};
  }

  std::unique_ptr<ITransaction> PostgresBackend::begin(IsolationLevel isolation_level) {
    auto txn =
        std::unique_ptr<PostgresTransaction>(new PostgresTransaction(state_, isolation_level));
    for (const auto& [unused_type, factory] : repository_factories_) {
      (void)unused_type;
      factory(*txn);
    }
    return txn;
  }

  Capabilities PostgresBackend::caps() const {
    Capabilities result;
    result.row_level_security = true;
    result.json_path_equality = true;
    result.json_path_indexes = true;
    result.native_uuid = false;
    result.listen_notify = true;
    return result;
  }

  void PostgresBackend::fail_next_audit_append_for_tests() {
    std::scoped_lock lock(state_->audit_mutex);
    state_->fail_next_audit_append = true;
  }

  std::size_t PostgresBackend::audit_event_count_for_tests() const {
    const std::size_t idx = state_->acquire(state_->options.pool_acquire_timeout);
    auto& conn = *state_->connections.at(idx);
    std::size_t count = 0;
    try {
      pqxx::work txn(conn);
      const auto result = txn.exec("SELECT COUNT(*) FROM audit_events");
      if (!result.empty()) { count = result[0][0].as<std::size_t>(); }
      txn.commit();
      // NOLINTNEXTLINE(bugprone-empty-catch)
    } catch (const pqxx::sql_error&) {
      // audit_events doesn't exist yet — return 0
    }
    state_->release(idx);
    return count;
  }

  void PostgresBackend::downgrade_to_zero_for_tests() {
    const std::size_t idx = state_->acquire(state_->options.pool_acquire_timeout);
    auto& conn = *state_->connections.at(idx);
    try {
      pqxx::work txn(conn);
      txn.exec("DELETE FROM schema_migrations");
      txn.commit();
    } catch (const pqxx::sql_error& err) {
      state_->release(idx);
      throw BackendError(BackendErrorCode::ConstraintViolation, err.what());
    }
    state_->release(idx);
  }

} // namespace fmgr::storage
