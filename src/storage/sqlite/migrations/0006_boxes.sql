-- SPDX-License-Identifier: AGPL-3.0-or-later

-- D5: Box instances — each an instance of a BoxType placed in a StorageContainer.
--
-- storage_container_id is NOT NULL and has no ON DELETE CASCADE; when a
-- StorageContainer is tombstoned the application propagates the archive to its
-- Boxes so audit history of sample locations is preserved.

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
