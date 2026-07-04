
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
