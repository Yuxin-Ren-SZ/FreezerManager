-- SPDX-License-Identifier: AGPL-3.0-or-later
-- D6: ItemType hierarchy + CustomFieldDefinition catalog.
-- ItemType uses an adjacency-list tree per lab; cycle prevention is enforced at the
-- application layer (not via a DB trigger) on the SQLite backend.
-- CustomFieldDefinition key uniqueness is per (lab, scope, item_type_or_global, key);
-- COALESCE(item_type_id, '') treats NULL as a consistent sentinel since '' is not a UUID.

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
