-- SPDX-License-Identifier: AGPL-3.0-or-later

-- D3: Freezer + StorageContainer recursive layout.
--
-- `freezers.layout_root_id` and `storage_containers.id` form a mutual
-- reference: a freezer's layout root container exists in the same table as
-- its descendants. The two FKs are DEFERRABLE INITIALLY DEFERRED so a single
-- transaction can insert the root storage_container and the parent freezer
-- in either order before commit.
--
-- Cycle prevention on storage_containers.parent_id is enforced in the
-- application layer (LayoutRepositories), since SQLite cannot express a
-- recursive CHECK across rows.

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
