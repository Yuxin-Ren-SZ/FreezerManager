
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
