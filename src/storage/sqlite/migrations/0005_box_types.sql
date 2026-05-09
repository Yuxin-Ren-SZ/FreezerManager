-- SPDX-License-Identifier: AGPL-3.0-or-later

-- D4: ContainerType + BoxType + Position geometry.
--
-- Position accepts are stored as child rows so D5 placement checks can
-- join against box geometry without reparsing JSON.

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
